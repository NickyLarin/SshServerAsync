//
// Created by nicky on 11/22/15.
//

#ifndef COMMON_H
    int setNonBlock(int fd);
    int addToEpoll(int epollfd, int fd, uint32_t flags);
    int changeEpoll(int epollfd, int fd, uint32_t flags);
    int writeNonBlock(int fd, char *string);
    ssize_t readNonBlock(int fd, char **buffer, size_t beginSize);
    char *cleanString(char *string);
    #define COMMON_H
#endif //COMMON_H
