#ifndef QUEUE_H
    #include <stdlib.h>
    struct Queue {
        void **data;
        int head;
        int last;
        int maxSize;
        size_t sizeOfElement;
    };
    int initQueue(struct Queue *queue, size_t sizeOfElement);
    int isEmptyQueue(struct Queue *queue);
    int pushQueue(struct Queue *queue, void *element);
    void moveElementsInQueue(struct Queue *queue);
    int popQueue(struct Queue *queue, void *element);
    void destroyQueue(struct Queue *queue);
    #define QUEUE_H
#endif
