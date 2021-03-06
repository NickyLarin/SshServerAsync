//
// Created by nicky on 11/22/15.
//
#include <stdio.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"

#define WRITE_BUFFER_SIZE 8
#define READ_BUFFER_SIZE 8
#define BUFFER_SIZE 1024

// Делаем дескриптор не блокирующимся
int setNonBlock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK)) {
        perror("Making descriptor non-block");
        return -1;
    }
    return 0;
}

// Добавляем дескриптор в epoll
int addToEpoll(int epollfd, int fd, uint32_t flags) {
    struct epoll_event event;
    memset(&event, 0, sizeof(struct epoll_event));
    event.data.fd = fd;
    event.events = flags;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("adding fd to epoll error");
        return -1;
    }
    return 0;
}

// Изменяем дескриптор в epoll
int changeEpoll(int epollfd, int fd, uint32_t flags) {
    struct epoll_event event;
    memset(&event, 0, sizeof(struct epoll_event));
    event.data.fd = fd;
    event.events = flags;
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event) == -1) {
        perror("moddin fd in epoll error\n");
        return -1;
    }
    return 0;
}

// Запись в неблокирующийся дескриптор
int writeNonBlock(int fd, char *string) {
    for (int i = 0; i < sizeof(string)/sizeof(char); i += WRITE_BUFFER_SIZE) {
        if (write(fd, string + i, WRITE_BUFFER_SIZE) < WRITE_BUFFER_SIZE && (errno & EAGAIN)) {
            return -1;
        }
    }
    return 0;
}

// Чтение из неблокирующегося дескриптора
ssize_t readNonBlock(int fd, char **buffer, size_t beginSize) {
    size_t size = beginSize;
    if (size < 1)
        size = READ_BUFFER_SIZE;
    if (*buffer == NULL) {
        *buffer = (char *)malloc(size * sizeof(char));
    }
    intmax_t count = 0;
    int legth = 0;
    do {
        count = read(fd, ((*buffer) + legth), READ_BUFFER_SIZE);
        legth += count;
        if (legth == size) {
            size *= 2;
            *buffer = realloc(*buffer, size * sizeof(char));
        }
    } while (count > 0);
    switch(count) {
        case -1: {
            if (errno != EAGAIN) {
                perror("reading non-block error");
                return -1;
            }
            break;
        }
        case 0: {
            printf("Connection have been closed");
        }
        default:
            break;
    }
    size = strlen(*buffer);
    *buffer = (char *)realloc(*buffer, size * sizeof(char));
    return size;
}

// Очистить строку от символов \n
char *cleanString(char *string) {
    int length = strlen(string);
    char *tmp = (char *)malloc(length);
    memcpy(tmp, string, length);
    int count = 0;
    for (int i = 0; i < length; i++) {
        if (tmp[i] == '\n' || tmp[i] == '\r')
            count++;
        else
            break;
    }
    tmp += count;
    length = strlen(tmp);
    count = 0;
    for (int i = 1; i < length; i++) {
        if (tmp[length-i] == '\n' || tmp[length-i] == '\r')
            count++;
        else
            break;
    }
    length-=count;
    tmp[length] = '\0';
    return tmp;
}

// Послать сообщение
int sendMessage(int dest, int source) {
    char buffer[BUFFER_SIZE];
    ssize_t readCount = read(source, buffer, BUFFER_SIZE);
    ssize_t writeCount;
    while (readCount > -1) {
        writeCount = write(dest, buffer, (size_t) readCount);
        readCount = read(source, buffer, BUFFER_SIZE);
    }
    if (readCount == -1 && errno != EAGAIN) {
        perror("reading message from fd");
        return -1;
    }
    if (writeCount == -1 && errno != EAGAIN) {
        perror("writing message to fd");
        return -1;
    }
    return 0;
}