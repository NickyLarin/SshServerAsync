//
// Created by nicky on 11/22/15.
//
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>


#include "common.h"
#include "handle_request.h"

int handleRequest(int connectionfd) {
    int ptm, pts;
    ptm = posix_openpt(O_RDWR);
    if (ptm == -1) {
        perror("Getting pseudo terminal error");
        return -1;
    }
    if (grantpt(ptm) == -1) {
        perror("grantpt error");
        return -1;
    }
    if (unlockpt(ptm) == -1) {
        perror("unlockpt error");
        return -1;
    }
    pts = open(ptsname(ptm), O_RDWR);

    // Делаем дескрипторы не блокирующимися
    if (setNonBlock(ptm) == -1) {
        fprintf(stderr, "Setting ptm to Non-block error");
        return -1;
    }
    if (setNonBlock(pts) == -1) {
        fprintf(stderr, "Setting pts to Non-block error");
        return -1;
    }

    if (fork()) { // Родительский процесс
        // Создаём epoll
        int epollfd = epoll_create1(0);
        if (epollfd == -1) {
            perror("epoll_create error\n");
            return -1;
        }

        // Добавляем сокет в epoll
        if (addToEpoll(epollfd, ) == -1)
            return -1;

        int maxEventNum = numberOfThreads;
        struct epoll_event events[maxEventNum];

    }
    else {  // Дочерний процесс

    }
}
