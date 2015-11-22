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

#include "common.h"
#include "queue.h"


// Глобальная переменная для завершения работы по сигналу
volatile sig_atomic_t done = 0;

// Глобальные переменные дескрипторов socketfd и epoll
volatile int socketfd = -1;
volatile int epollfd = -1;

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

// Структура для передачи аргументов в функции при создании потока
struct ThreadArgs {
    struct Queue *queue;
    pthread_mutex_t *mutex;
    pthread_cond_t *condition;
};

// Структура для хранения в очереди информации о событии
struct Event {
    struct epoll_event *epollEvent;
    int epollfd;
    int socketfd;
};

// Инициализируем структуру события
void initEvent(struct Event *event, struct epoll_event *epollEvent) {
    event->epollEvent = epollEvent;
}

// Инициализируем структуру аргументов
void initThreadArgs(struct ThreadArgs *threadArgs, struct Queue *queue, pthread_mutex_t *mutex, pthread_cond_t *condition) {
    threadArgs->queue = queue;
    threadArgs->mutex = mutex;
    threadArgs->condition = condition;
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
    return 0;
}

// Обрабатываем новое сообщение
int handleRequest(int connectionfd) {
    char buffer[1024];
    ssize_t count = read(connectionfd, buffer, sizeof(buffer));
    switch (count) {
        case -1:
            if (errno != EAGAIN)
                perror("Reading data error");
            break;
        case 0:
            printf("Client closed the connection\n");
            break;
        default:
            dprintf(connectionfd, "Hi, There!\n");
    }
}

// Обрабатываем событие
void *handleEvent(void *args) {
    // Получаем аргументы в новом потоке
    struct ThreadArgs *threadArgs = args;
    while (!done) {
        // Ожидание поступления события в очередь
        pthread_mutex_lock(threadArgs->mutex);
        while (isEmptyQueue(threadArgs->queue)) {
            pthread_cond_wait(threadArgs->condition, threadArgs->mutex);
        }
        struct epoll_event event;
        popQueue(threadArgs->queue, &event);
        pthread_mutex_unlock(threadArgs->mutex);

        printf("Starting handle event\nThread: %d\n", (int)pthread_self());
        if ((event.events & EPOLLERR) || (event.events & EPOLLHUP)) {
            fprintf(stderr, "Handle event error or event hangup\n");
            close(event.data.fd);
            continue;
        }
        if (event.data.fd == socketfd) {
            if (acceptConnection() == -1) {
                fprintf(stderr, "Error: accepting new connection");
                continue;
            }
        } else {
            handleRequest(event.data.fd);
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
    int numberOfThreads = atoi(argv[1]);

    // Порт - 2й параметр запуска
    char port[4];
    strcpy(port, argv[2]);

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
    pthread_t threads[numberOfThreads];
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

    // Создаём очередь
    struct Queue queue;
    initQueue(&queue, sizeof(struct epoll_event));

    struct ThreadArgs threadArgs;
    initThreadArgs(&threadArgs, &queue, &mutex, &condition);

    for (int i = 0; i < sizeof(threads)/sizeof(pthread_t); i++) {
        pthread_create(&threads[i], NULL, handleEvent, (void *)&threadArgs);
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

    int maxEventNum = numberOfThreads;
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
