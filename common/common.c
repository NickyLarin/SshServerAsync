//
// Created by nicky on 11/22/15.
//
#include <stdio.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#include "common.h"

#define WRITE_BUFFER_SIZE 8
#define READ_BUFFER_SIZE 8

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
int addToEpoll(int epollfd, int fd) {
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("adding fd to epoll error\n");
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
int readNonBlock(int fd, char *string) {
    int size = READ_BUFFER_SIZE;
    char *buffer = (char *)malloc(size * sizeof(char));
    int count = 0;
    do {
        count = read(fd, buffer, READ_BUFFER_SIZE);
        if (count + READ_BUFFER_SIZE <= size) {
            size *= 4;
            buffer = (char *) realloc(buffer, size * sizeof(char));
        }
    } while (count != 0 && (errno & EAGAIN));
    string = buffer;
    return 0;
}