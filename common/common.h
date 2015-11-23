//
// Created by nicky on 11/22/15.
//

#ifndef COMMON_H
    int setNonBlock(int fd);
    int addToEpoll(int epollfd, int fd);
    int writeNonBlock(int fd, char *string);
    int readNonBlock(int fd, char **buffer, size_t beginSize);
    #define COMMON_H
#endif //COMMON_H
