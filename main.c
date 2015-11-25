#define _XOPEN_SOURCE 600
#define _BSD_SOURCE
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
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

#include "common.h"
#include "queue.h"
#include "pass_pair.h"


#define MAX_CONNECTIONS 256
#define CONNECTION_TIMEOUT 300
#define MAX_PASSWORD_ATTEMPTS 5

#define LOGIN_REQUEST 0
#define LOGIN_CHECK 1
#define PASSWORD_REQUEST 2
#define PASSWORD_CHECK 3
#define AUTHENTICATED 4

// Структура содержащая информацию об аутентификации
struct Authentication {
    int status;  // 0 - Новое соединение 1 - Запрошен логин 2 - Логин проверен, запрошен пароль 3 - Пароль проверен
    int attempts;
};

// Структура содержащая информацию о соединении
struct Connection {
    int connectionfd;
    int ptm;
    struct Authentication auth;
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
struct addrinfo*getAvailableAddresses(char *port) {
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
    struct Authentication auth;
    memset(&auth, 0, sizeof(struct Authentication));
    connection.auth = auth;
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
    return 0;
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
    if (connection->auth.status < AUTHENTICATED) {
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
        if (strncmp(passPairs[i].login, result, strlen(passPairs[i].login)) == 0) {
            return &passPairs[i];
        }
    }
    fprintf(stderr, "Wrong login: %s\n", result);
    return NULL;
}

// Сверяем пароль
int verifyPassword(struct PassPair *pair, char *password) {
    char *result = cleanString(password);
    if (strncmp(pair->pass, result, strlen(pair->pass)) != 0) {
        fprintf(stderr, "Login: %s\nWrong password: %s\n", pair->login, result);
        return -1;
    }
    return 0;
}

// Запрашиваем логин
int requestLogin(struct Connection *connection) {
    if (sendMsg(connection->connectionfd, "Enter login: ") == -1) {
        fprintf(stderr, "Error: sending login msg\n");
        return -1;
    }
    connection->auth.status = LOGIN_CHECK;
    return 0;
}

// Запрашиваем пароль
int requestPassword(struct Connection *connection) {
    if (sendMsg(connection->connectionfd, "Enter password: ") == -1) {
        fprintf(stderr, "Error: sending password msg\n");
        return -1;
    }
    connection->auth.status = PASSWORD_CHECK;
    return 0;
}

// Проверяем логин
int checkLogin(struct Connection *connection) {
    char *login = NULL;
    if (readNonBlock(connection->connectionfd, &login, 0) == -1) {
        fprintf(stderr, "Error: receiving login from connection %d\n", connection->connectionfd);
        return -1;
    }
    connection->pair = getPair(login);
    if (connection->pair == NULL) {
        if (sendMsg(connection->connectionfd, "Wrong login, try again\n") == -1) {
            fprintf(stderr, "Error: sending wrong login msg\n");
            return -1;
        }
        requestLogin(connection);
    } else {
        connection->auth.status = PASSWORD_REQUEST;
        requestPassword(connection);
    }
    free(login);
    return 0;
}

// Проверяем пароль
int checkPassword(struct Connection *connection) {
    char *password = NULL;
    if (readNonBlock(connection->connectionfd, &password, 0) == -1) {
        fprintf(stderr, "Error: receiving password from connection %d\n", connection->connectionfd);
        return -1;
    }
    if (verifyPassword(connection->pair, password) == -1) {
        if (connection->auth.attempts == MAX_PASSWORD_ATTEMPTS) {
            fprintf(stderr, "Many password enter attempts for user: %s\n", connection->pair->login);
            if (sendMsg(connection->connectionfd, "Too many password enter attempts\n") == -1) {
                fprintf(stderr, "Error: sending wrong too many attempts msg\n");
                return -1;
            }
            closeConnection(connection);
            return -1;
        }
        if (sendMsg(connection->connectionfd, "Wrong password, try again\n\n") == -1) {
            fprintf(stderr, "Error: sending wrong password msg\n");
            return -1;
        }
        connection->auth.attempts++;
        requestPassword(connection);
    } else {
        if (sendMsg(connection->connectionfd, "Authentication complete!\n") == -1) {
            fprintf(stderr, "Error: sending password msg\n");
            return -1;
        }
        connection->auth.status = AUTHENTICATED;
    }
    free(password);
    return 0;
}

// Пройти аутентификацию
int passAuthentication(struct Connection *connection) {
    switch (connection->auth.status) {
        case LOGIN_REQUEST:
            requestLogin(connection);
            break;
        case LOGIN_CHECK:
            checkLogin(connection);
            break;
        case PASSWORD_REQUEST:
            requestPassword(connection);
            break;
        case PASSWORD_CHECK:
            checkPassword(connection);
            break;
        default:
            fprintf(stderr, "Error: wrong authentication status of connectionfd %d\n", connection->connectionfd);
            return -1;
    }
    return 0;
}

int createPty(struct Connection *connection) {
    int ptm, pts;
    ptm = posix_openpt(O_RDWR);
    if (ptm == -1) {
        perror("creating new plm");
        return -1;
    }
    if (grantpt(ptm) == -1) {
        perror("granting pt access");
        return -1;
    }
    if (unlockpt(ptm) == -1) {
        perror("unlocking pt");
        return -1;
    }
    pts = open(ptsname(ptm), O_RDWR);
    if (pts == -1) {
        perror("opening pts");
        return -1;
    }
    if (setNonBlock(ptm) == -1) {
        fprintf(stderr, "Error: making ptm non-block\n");
        return -1;
    }
    if (setNonBlock(pts) == -1) {
        fprintf(stderr, "Error: making pts non-block\n");
        return -1;
    }
    connection->ptm = ptm;

    if (fork()) {
        if (close(pts) == -1) {
            perror("closing pts in parent process");
            return -1;
        }
        if (addToEpoll(epollfd, ptm, EPOLLET | EPOLLIN) == -1) {
            fprintf(stderr, "Error: adding ptm to epoll\n");
            return -1;
        }
    } else {
        if (close(ptm) == -1) {
            perror("closing ptm in child process");
            return -1;
        }
        struct termios oldSettings, newSettings;
        if (tcgetattr(pts, &oldSettings) == -1) {
            perror("getting old terminal settings\n");
            return -1;
        }
        newSettings = oldSettings;
        cfmakeraw(&newSettings);
        if (tcsetattr(pts, TCSANOW, &newSettings) == -1) {
            perror("setting new terminal settings\n");
            return -1;
        }
        close(0);
        close(1);
        close(2);

        dup(pts);
        dup(pts);
        dup(pts);

        close(pts);

        ioctl(0, TIOCSCTTY, 1);
        execvp("/bin/bash", NULL);
    }
    return 0;
}

int workWithPty(struct Connection *connection) {
    if (connection->ptm == -1) {
        if (createPty(connection) == -1) {
            fprintf(stderr, "Error: creating new pty\n");
            return -1;
        }
    }
    return 0;
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
    if (checkAuthentication(connection) > 0) {
        passAuthentication(connection);
    } else {
        workWithPty(connection);
    }
    return 0;
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
                fprintf(stderr, "Error: handling event\n");
                continue;
            }
        } else {
            if (handleEvent(event.data.fd) == -1) {
                fprintf(stderr, "Error: handling event\n");
                continue;
            }
        }
    }
    return NULL;
}

// Читаем пароли из файла
int readPasswordsFromFile(char *path) {
    struct PassPair *passwords;
    int size = 8;
    passwords = (struct PassPair *)malloc(size * sizeof(struct PassPair));
    FILE *ptr;
    ptr = fopen(path, "r");
    if (ptr == NULL) {
        perror("opening passwords file");
        return -1;
    }
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
    return 0;
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

    struct addrinfo* addresses = getAvailableAddresses(port);
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
