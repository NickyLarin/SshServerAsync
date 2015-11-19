#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>

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

// Добавляем дескриптор в epoll
int addToEpoll(int epollfd, int fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("adding fd to epoll error\n");
        return -1;
    }
    return 0;
}

// Делаем дескриптор не блокирующимся
int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        perror("Making descriptor non-block");
        return -1;
    }
    return 0;
}

// Добавляем новое соединение в  epoll
int acceptConnection(int epollfd, int socketfd) {
    printf("Start accepting new conenction");
    struct sockaddr addr;
    socklen_t addrlen = sizeof(addr);
    int connectionfd = accept(socketfd, &addr, &addrlen);
    if (connectionfd == -1) {
        perror("acception connection error");
        return -1;
    }
    if (setNonBlock(connectionfd) == -1) {
        fprintf(stderr, "Making connection %d non-block error", connectionfd);
        return -1;
    }
    if (addToEpoll(epollfd, connectionfd) == -1) {
        fprintf(stderr, "Adding connection to epoll");
        return -1;
    }
    return 0;
}

// Обрабатываем новое сообщение
int handleRequest(int epollfd, int connectionfd) {
    printf("Start handling request");
    char buffer[1024];
    ssize_t count = read(connectionfd, buffer, sizeof(buffer));
    if(count > 0) {
        dprintf(connectionfd, "Hi, there!");
    }
}

// Обрабатываем событие
int handleEvent(struct epoll_event *event, int epollfd, int socketfd) {
    printf("Start handling event");
    if ((event->events & EPOLLERR) || (event->events & EPOLLHUP)) {
        fprintf(stderr, "Handle event error or event hangup\n");
        close(event->data.fd);
        return -1;
    }
    if (socketfd == event->data.fd) {
        acceptConnection(epollfd, socketfd);
    } else {
        handleRequest(epollfd, socketfd);
    }
}

volatile sig_atomic_t done = 0;

void handleSigInt(int signum) {
    done = 1;
}

///////////////////////////////////////////////////////////////////////////////
//
// MAIN
//
///////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[]) {
    // Обработка сигнала
    /*
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handleSigInt;
    sigaction(SIGINT, &act, 0);
    */

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
    int socketfd = getSocket(addresses);
    if (socketfd == -1) {
        exit(EXIT_FAILURE);
    }

    // Начинаем слушать сокет
    if (listen(socketfd, SOMAXCONN) == -1) {
        perror("listen\n");
        exit(EXIT_FAILURE);
    }

    // Создаём epoll
    int epollfd = epoll_create1(0);
    if (epollfd == -1) {
        perror("epoll_create error\n");
        exit(EXIT_FAILURE);
    }

    // Добавляем сокет в epoll
    if (addToEpoll(epollfd, socketfd) == -1)
        exit(EXIT_FAILURE);


    int maxEventNum = numberOfThreads;
    struct epoll_event events[maxEventNum];
    printf("I'm HERE!!");

    int timeout =-1;
    while(!done) {
        printf("Start waiting for epoll events");
        int eventsNumber = epoll_wait(epollfd, events, maxEventNum, timeout);
        if (eventsNumber == 0)
            printf("No events");
        for (int i = 0; i < eventsNumber; i++) {
            handleEvent(events + i, epollfd, socketfd);
        }
    }

    close(socketfd);
    close(epollfd);

    return 0;
}
