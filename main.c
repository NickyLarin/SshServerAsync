#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "common.h"
#include "queue.h"


#define MAX_CONNECTIONS 256
#define CONNECTION_TIMEOUT 300

#define CONN_FD_TYPE 1
#define PTM_FD_TYPE 2
#define OTHER_FD_TYPE 3

#define NOT_AUTHENTICATED 0
#define LOGIN_REQUESTED 1
#define PASSWORD_REQUESTED 2
#define AUTHENTICATED 3

// Структура содержащая информацию о соединении
struct Connection {
    int connectionfd;
    int ptm;
    int authentication;  // 0 - Новое соединение 1 - Запрошен логин 2 - Логин проверен, запрошен пароль 3 - Пароль проверен
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
    pthread_mutex_lock(&connectionsMutex);
    for (int i = 0; i < sizeof(connections)/sizeof(connections[0]); i++) {
        if (connections[i].connectionfd == 0) {
            connections[i] = connection;
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
        fprintf(stderr, "Making connection descriptor %d non-block error", connectionfd);
        return -1;
    }
    if (addToEpoll(epollfd, connectionfd) == -1) {
        fprintf(stderr, "Adding connection to epoll");
        return -1;
    }
    addConnectionIntoList(connectionfd);
    return 0;
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

// Пройти аутентификацию
int passAuthentication(struct Connection *connection) {
    switch(connection->authentication) {
        case NOT_AUTHENTICATED: {
            char message[] = "Login: ";
            int count = write(connection->connectionfd, message, sizeof(message)/sizeof(char));
            if (count < sizeof(message)/sizeof(char) && (errno & EAGAIN)) {
                perror("writing to connectionfd");
                return -1;
            }
            connection->authentication++;
            break;
        }
        case LOGIN_REQUESTED: {
            int size = 128;
            char *buffer = malloc(size * sizeof(char));
            int count = 0;
            do {
                count += read(connection->connectionfd, buffer, size);
                if (count == size) {
                    size *= 2;
                    buffer = realloc(buffer, size * sizeof(char));
                }
            } while (count > 0 && (errno & ~EAGAIN));
            printf("SERVER RECEIVE LOGIN: %s\n", buffer);

            free(buffer);
            char message[] = "Password: ";
            count = write(connection->connectionfd, message, sizeof(message)/sizeof(char));
            if (count < sizeof(message)/sizeof(char) && (errno & EAGAIN)) {
                perror("writing to connectionfd");
                return -1;
            }
            connection->authentication++;
            break;
        }
        case PASSWORD_REQUESTED: {
            int size = 128;
            char *buffer = malloc(size * sizeof(char));
            int count = 0;
            do {
                count += read(connection->connectionfd, buffer, size);
                if (count == size) {
                    size *= 2;
                    buffer = realloc(buffer, size * sizeof(char));
                }
            } while (count > 0 && (errno & ~EAGAIN));
            printf("SERVER RECEIVE PASS: %s\n", buffer);
            free(buffer);
            char message[] = "Authentication proceeded!\n";
            count = write(connection->connectionfd, message, sizeof(message)/sizeof(char));
            if (count < sizeof(message)/sizeof(char) && (errno & EAGAIN)) {
                perror("writing to connectionfd");
                return -1;
            }
            connection->authentication++;
            break;
        }
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

// Определяем тип дескриптора (connection, ptm или другой)


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


        printf("Starting handle event. Thread: %d\n", (int)pthread_self());
        if (event.data.fd == socketfd) {
            if (acceptConnection() == -1) {
                fprintf(stderr, "Error: accepting new connection\n");
                continue;
            }
        } else {
            if (handleEvent(event.data.fd)) {

            }
        }
    }
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
    if (argc < 3) {
        fprintf(stderr, "Too few arguments\n");
        exit(EXIT_FAILURE);
    }

    // Количество потоков - 1й параметр запуска
    int numberOfWorkers = atoi(argv[1]);

    // Порт - 2й параметр запуска
    char port[4];
    strcpy(port, argv[2]);

    // Инициализация fdLists
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
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

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
    if (addToEpoll(epollfd, socketfd) == -1)
        exit(EXIT_FAILURE);

    int maxEventNum = numberOfWorkers;
    struct epoll_event events[maxEventNum];

    int timeout = -1;
    printf("Main thread: %d\n", (int)pthread_self());
    while(!done) {
        printf("Start waiting for epoll events\n");
        int eventsNumber = epoll_wait(epollfd, events, maxEventNum, timeout);
        if (!eventsNumber)
            printf("No events\n");
        for (int i = 0; i < eventsNumber; i++) {
            printf("Handling event %d of %d\n", i + 1, eventsNumber);
            pthread_mutex_lock(&mutex);
            pushQueue(&queue, &events[i]);
            pthread_cond_signal(&condition);
            pthread_mutex_unlock(&mutex);
        }
    }

    destroyQueue(&queue);
    close(socketfd);
    close(epollfd);
    printf("DONE!!!");

    return 0;
}
