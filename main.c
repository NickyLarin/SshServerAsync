#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>
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
    if (!status) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
        return NULL;
    }
    return addresses;
}

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
    char *port;
    strcpy(port, argv[2]);

    struct addrinfo* addresses = getAvailiableAddresses(port);
    if (!addresses) 
        exit(EXIT_FAILURE);

    // Получаем дескриптор сокета
    int socketfd = getSocket(addresses);
    if (socketfd == -1) {
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

    // Начинаем слушать сокет
    if (listen(socketfd, SOMAXCONN) == -1) {
        perror("listen\n");
        exit(EXIT_FAILURE);
    }

    close(socketfd);
    close(epollfd);

    return 0;
}
