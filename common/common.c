//
// Created by nicky on 11/22/15.
//
#include <stdio.h>
#include <fcntl.h>
#include <sys/epoll.h>

#include "common.h"

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
    event.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) == -1) {
        perror("adding fd to epoll error\n");
        return -1;
    }
    return 0;
}