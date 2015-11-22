//
// Created by nicky on 11/22/15.
//

#ifndef COMMON_H
    int setNonBlock(int fd);
    int addToEpoll(int epollfd, int fd);
    #define COMMON_H
#endif //COMMON_H
