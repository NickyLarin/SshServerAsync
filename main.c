#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>

#include "common.h"
#include "queue.h"
#include "pass_pair.h"


#define MAX_CONNECTIONS 256
#define CONNECTION_TIMEOUT 300
#define MAX_LOGIN_ATTEMPTS 3

#define LOGIN_REQUEST 0
#define LOGIN_CHECK 1
#define PASSWORD_REQUEST 2
#define PASSWORD_CHECK 3
#define AUTHENTICATED 4

// Структура содержащая информацию о соединении
struct Connection {
    int connectionfd;
    int ptm;
    int authentication;  // 0 - Новое соединение 1 - Запрошен логин 2 - Логин проверен, запрошен пароль 3 - Пароль проверен
    struct PassPair *pair;
    time_t lastRequest;
};

// Структура для передачи аргументов в функции при создании потока
struct WorkerArgs {
    struct Queue *queue;
    pthread_mutex_t *mutex;
    pthread_cond_t *condition;
};

// Глобальная переменная для завершения работы по сигналу
volatile sig_atomic_t done = 0;

// Глобальные переменные для струтуры со списком дескрипторов соединений и мьютексом для синхронизации доступа к ним
struct Connection connections[MAX_CONNECTIONS];
pthread_mutex_t connectionsMutex;

// Глобальная переменная с парами логин-пароль;
struct PassPair *passPairs;
intmax_t lengthPassPairs;

// Глобальные переменные дескрипторов socketfd и epoll
int socketfd = -1;
int epollfd = -1;

void setSocketFd(int _socketfd) {
    if (socketfd == -1) {
        socketfd = _socketfd;
    }
}

void setEpollFd(int _epollfd) {
    if (epollfd == -1) {
        epollfd = _epollfd;
    }
}

// Перехватчик сигнала
void handleSigInt(int signum) {
    done = 1;
}


// Инициализируем структуру аргументов
void initWorkerArgs(struct WorkerArgs *workerArgs, struct Queue *queue, pthread_mutex_t *mutex,
                    pthread_cond_t *condition) {
    workerArgs->queue = queue;
    workerArgs->mutex = mutex;
    workerArgs->condition = condition;
}

// Получение доступных адресов
struct addrinfo* getAvailiableAddresses(char *port) {
    struct addrinfo* addresses;

    // Требования к адрессам
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    
    int status = getaddrinfo(NULL, port, &hints, &addresses);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return NULL;
    }
    return addresses;
}

//  Получаем дескриптор сокета привязанный к адресу
int getSocket(struct addrinfo* addresses) {
    int yes = 1;
    int socketfd = 0;

    // Перебор списка подходящих адрессов
    for (struct addrinfo *address = addresses; address != NULL; address = address->ai_next) {
        // Создание неблокирующегося сокета
        socketfd = socket(address->ai_family, address->ai_socktype | SOCK_NONBLOCK, address->ai_protocol);
        if (socketfd == -1) {
            perror("creating socket error\n");
            continue;
        }
        // Установка опции SO_REUSEADDR
        if (setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            perror("setsockopt error");
            continue;
        }
        // Привязка сокета к адресу
        if(bind(socketfd, address->ai_addr, address->ai_addrlen) == -1) {
            close(socketfd);
            perror("binding socket error\n");
            continue;
        }
        return socketfd;
    }
    perror("socket bind error\n");
    return -1;
}


// Добавляем соединение в список
void addConnectionIntoList(int connectionfd) {
    struct Connection connection;
    memset(&connection, 0, sizeof(struct Connection));
    connection.connectionfd = connectionfd;
    connection.ptm = -1;
    connection.authentication = 0;
    connection.lastRequest = time(NULL);
    connection.pair = NULL;
    pthread_mutex_lock(&connectionsMutex);
    for (int i = 0; i < sizeof(connections)/sizeof(connections[0]); i++) {
        if (connections[i].connectionfd == 0) {
            connections[i] = connection;
            break;
        }
    }
    pthread_mutex_unlock(&connectionsMutex);
}

// Удаляем соединения из списка
void removeConnectionFromList(struct Connection *connection) {
    pthread_mutex_lock(&connectionsMutex);
    for (int i = 0; i < sizeof(connections)/sizeof(connections[0]); i++) {
        if (connection == &connections[i]) {
            memset(&connections[i], 0, sizeof(connections[i]));
            break;
        }
    }
    pthread_mutex_unlock(&connectionsMutex);
}

// Добавляем новое соединение в epoll
int acceptConnection() {
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    int connectionfd = accept(socketfd, &addr, &addrlen);

    if (connectionfd == -1) {
        perror("acception connection error");
        return -1;
    }
    if (setNonBlock(connectionfd) == -1) {
        fprintf(stderr, "Making connection descriptor %d non-block error\n", connectionfd);
        return -1;
    }
    if (addToEpoll(epollfd, connectionfd, EPOLLET | EPOLLIN) == -1) {
        fprintf(stderr, "Adding connection to epoll\n");
        return -1;
    }
    addConnectionIntoList(connectionfd);
    return connectionfd;
}


// Закрываем соединение
int closeConnection(struct Connection *connection) {
    if (close(connection->connectionfd) == -1) {
        perror("closing connection");
        return -1;
    }
    removeConnectionFromList(connection);
}

// Проверяем не наступил ли таймаут соединения
// Обновляем если не наступил
// Возвращаем 0, если таймаут не наступил и был обновлен
// Возвращаем 1, если таймаут наступил
int checkConnectionTimeout(struct Connection *connection) {
    if (difftime(time(NULL), connection->lastRequest) > CONNECTION_TIMEOUT) {
        return 1;
    } else {
        connection->lastRequest = time(NULL);
        return 0;
    }
}

// Ищем и соединение в массиве
struct Connection *getConnection(int fd) {
    struct Connection *result = NULL;
    pthread_mutex_lock(&connectionsMutex);
    for (int i = 0; i < sizeof(connections)/sizeof(connections[0]); i++) {
        if (fd == connections[i].connectionfd || fd == connections[i].ptm)
            result = &connections[i];
    }
    pthread_mutex_unlock(&connectionsMutex);
    return result;
}

// Проверяем аутентификацию
int checkAuthentication(struct Connection *connection) {
    if (connection->authentication < AUTHENTICATED) {
        return 1;
    }
    return 0;
}

// Посылаем сообщение
int sendMsg(int connectionfd, char *msg) {
    intmax_t count = write(connectionfd, msg, strlen(msg));
    if (count != strlen(msg)) {
        return -1;
    }
    return 0;
}

// Получаем пару логин-пароль по логину
struct PassPair *getPair(char *login) {
    char *result = cleanString(login);
    for (int i = 0; i < lengthPassPairs; i++) {
        printf("Login in file: %s\n", passPairs[i].login);
        if (strncmp(passPairs[i].login, result, strlen(passPairs[i].login)) == 0) {
            return &passPairs[i];
        }
    }
    fprintf(stderr, "Wrong login: %s\n", result);
    return NULL;
}

// Проверяем пароль
int checkPassword(struct PassPair *pair, char *password) {
    char *result = cleanString(password);
    if (strncmp(pair->pass, result, strlen(pair->pass)) != 0) {
        fprintf(stderr, "Wrong password: %s\n", result);
        return -1;
    }
    return 0;
}

// Пройти аутентификацию
int passAuthentication(struct Connection *connection) {
    printf("Thread: %d get here with: %d auth: %d\n", pthread_self(), connection->connectionfd, connection->authentication);
    switch(connection->authentication) {
        case LOGIN_REQUEST: {
            if (sendMsg(connection->connectionfd, "Enter login: ") == -1) {
                fprintf(stderr, "Error: sending login msg\n");
                return -1;
            }
            connection->authentication = LOGIN_CHECK;
            break;
        }
        case LOGIN_CHECK: {
            char *login = NULL;
            int size = readNonBlock(connection->connectionfd, &login, 0);
            connection->pair = getPair(login);
            if (connection->pair == NULL) {
                if(sendMsg(connection->connectionfd, "Wrong login, try again\n") == -1) {
                    fprintf(stderr, "Error: sending wrong login msg\n");
                    return -1;
                }
                connection->authentication = LOGIN_REQUEST;
                break;
            }
            free(login);
            connection->authentication = PASSWORD_REQUEST;
            break;
        }
        case PASSWORD_REQUEST: {
            if (sendMsg(connection->connectionfd, "Enter password: ") == -1) {
                fprintf(stderr, "Error: sending password msg\n");
                return -1;
            }
            connection->authentication = PASSWORD_CHECK;
            break;
        }
        case PASSWORD_CHECK: {
            char *password = NULL;
            int size = readNonBlock(connection->connectionfd, &password, 0);
            if (checkPassword(connection->pair, password) == -1) {
                if (sendMsg(connection->connectionfd, "Wrong password, try again\n") == -1) {
                    fprintf(stderr, "Error: sending wrong password msg\n");
                    return -1;
                }
                connection->authentication = PASSWORD_REQUEST;
                break;
            }
            free(password);
            if (sendMsg(connection->connectionfd, "Authentication complete!") == -1) {
                fprintf(stderr, "Error: sending password msg\n");
                return -1;
            }
            connection->authentication = AUTHENTICATED;
            break;
        }
        default:
            break;
    }
}

// Обрабатываем новое сообщение
int handleEvent(int fd) {
    struct Connection *connection = getConnection(fd);
    if (connection == NULL) {
        fprintf(stderr, "Error: connection from epoll wasn't found in list\n");
        return -1;
    }
    if (checkConnectionTimeout(connection) == 1) {
        if (closeConnection(connection) == -1) {
            fprintf(stderr, "Error: closing connection for timeout\n");
            return -1;
        }
    }
    if (checkAuthentication(connection)) {
        passAuthentication(connection);
    } else {

    }
}

// Обрабатываем событие
void *worker(void *args) {
    // Получаем аргументы в новом потоке
    struct WorkerArgs *workerArgs = args;
    while (!done) {
        // Ожидание поступления события в очередь
        pthread_mutex_lock(workerArgs->mutex);
        while (isEmptyQueue(workerArgs->queue)) {
            pthread_cond_wait(workerArgs->condition, workerArgs->mutex);
        }
        struct epoll_event event;
        popQueue(workerArgs->queue, &event);
        pthread_mutex_unlock(workerArgs->mutex);
        if (event.data.fd == socketfd) {
            int connectionfd = acceptConnection();
            if (connectionfd == -1) {
                fprintf(stderr, "Error: accepting new connection\n");
                continue;
            }
            if (handleEvent(connectionfd) == -1) {
                fprintf(stderr, "Error: handling new connection, after accepting\n");
                continue;
            };
        } else {
            if (handleEvent(event.data.fd) == -1) {
                fprintf(stderr, "Error: handling event\n");
                continue;
            }
        }
    }
}

// Читаем пароли из файла
int readPasswordsFromFile(char *path) {
    struct PassPair *passwords;
    int size = 8;
    passwords = (struct PassPair *)malloc(size * sizeof(struct PassPair));
    FILE *ptr;
    ptr = fopen(path, "r");
    intmax_t count;
    size_t length = 0;
    do {
        count = fread(passwords, sizeof(struct PassPair), 1, ptr);
        length += count;
        if (size == length) {
            size *= 2;
            passwords = (struct PassPair *)realloc(passwords, size * sizeof(struct PassPair));
        }
    } while (count == sizeof(struct PassPair));
    passPairs = (struct PassPair *)malloc(length * sizeof(struct PassPair));
    memcpy(passPairs, passwords, length * sizeof(struct PassPair));
    lengthPassPairs = length;
    free(passwords);
    fclose(ptr);
}

///////////////////////////////////////////////////////////////////////////////
//
// MAIN
//
///////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
    // Обработка сигнала
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handleSigInt;
    sigaction(SIGINT, &act, 0);

    // Проверка количества аргументов
    if (argc < 4) {
        fprintf(stderr, "Too few arguments\n");
        exit(EXIT_FAILURE);
    }

    // Количество потоков - 1й параметр запуска
    int numberOfWorkers = atoi(argv[1]);

    // Порт - 2й параметр запуска
    char port[4];
    strcpy(port, argv[2]);

    // Путь к файлу с паролями - 3й параметр запуска
    readPasswordsFromFile(argv[3]);

    // Инициализация connections
    memset(connections, 0, sizeof(connections));

    struct addrinfo* addresses = getAvailiableAddresses(port);
    if (!addresses) 
        exit(EXIT_FAILURE);

    // Получаем дескриптор сокета
    int _socketfd = getSocket(addresses);
    if (_socketfd == -1) {
        exit(EXIT_FAILURE);
    }

    setSocketFd(_socketfd);

    // Начинаем слушать сокет
    if (listen(socketfd, SOMAXCONN) == -1) {
        perror("listen\n");
        exit(EXIT_FAILURE);
    }

    // Создаём потоки
    pthread_t workers[numberOfWorkers];
    pthread_mutex_t mutex;
    pthread_mutexattr_t mutexattr;
    pthread_mutexattr_init(&mutexattr);
    pthread_mutexattr_settype(&mutexattr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&mutex, &mutexattr);
    pthread_mutex_init(&connectionsMutex, &mutexattr);
    pthread_cond_t condition;
    pthread_cond_init(&condition, NULL);

    // Создаём очередь
    struct Queue queue;
    initQueue(&queue, sizeof(struct epoll_event));

    struct WorkerArgs workerArgs;
    initWorkerArgs(&workerArgs, &queue, &mutex, &condition);

    for (int i = 0; i < sizeof(workers) / sizeof(pthread_t); i++) {
        pthread_create(&workers[i], NULL, worker, (void *) &workerArgs);
    }

    // Создаём epoll
    int _epollfd = epoll_create1(0);
    if (_epollfd == -1) {
        perror("epoll_create error\n");
        exit(EXIT_FAILURE);
    }

    setEpollFd(_epollfd);

    // Добавляем сокет в epoll
    if (addToEpoll(epollfd, socketfd, EPOLLET | EPOLLIN) == -1)
        exit(EXIT_FAILURE);

    int maxEventNum = numberOfWorkers;
    struct epoll_event events[maxEventNum];

    int timeout = -1;
    printf("Main thread: %d\n", (int)pthread_self());
    while(!done) {
        int eventsNumber = epoll_wait(epollfd, events, maxEventNum, timeout);
        if (!eventsNumber)
            printf("No events\n");
        for (int i = 0; i < eventsNumber; i++) {
            pthread_mutex_lock(&mutex);
            pushQueue(&queue, &events[i]);
            pthread_cond_signal(&condition);
            pthread_mutex_unlock(&mutex);
        }
    }

    // Освобождение ресурсов
    pthread_mutex_destroy(&connectionsMutex);
    pthread_mutex_destroy(&mutex);
    pthread_mutexattr_destroy(&mutexattr);
    free(passPairs);
    destroyQueue(&queue);
    close(socketfd);
    close(epollfd);
    printf("DONE!!!");

    return 0;
}
