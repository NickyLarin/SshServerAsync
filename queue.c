#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "queue.h"

#define BEGIN_QUEUE_SIZE 128

// Инициализируем очередь
int initQueue(struct Queue *queue, size_t sizeOfElement) {
    void **data = (void **)malloc(sizeof(void *) * BEGIN_QUEUE_SIZE);
    if (queue->data == NULL) {
        fprintf(stderr, "Error: initializing queue\n");
        return -1;
    }
    memset(data, 0, sizeof(void *) * BEGIN_QUEUE_SIZE);
    queue->data = data;
    queue->sizeOfElement = sizeOfElement;
    queue->maxSize = BEGIN_QUEUE_SIZE;
    queue->head = 0;
    queue->last = 0;
    return 0;
}

// Добавляем элемент в очередь
int pushQueue(struct Queue *queue, void *element) {
    if (queue->last == queue->maxSize) {
        queue->maxSize *= 2;
        void **data = (void **)realloc(queue->data, queue->maxSize * sizeof(void *));
        if (data == NULL) {
            fprintf(stderr, "Error: increasing size of queue array\n");
            return -1;
        }
        queue->data = data;
    }
    void *newElement = (void *)malloc(queue->sizeOfElement);
    if (newElement == NULL) {
        fprintf(stderr, "Error: allocating memory for new element in queue\n");
        return -1;
    }
    memcpy(newElement, element, queue->sizeOfElement);
    queue->data[queue->last] = newElement;
    queue->last += 1;
    return 0;
}

// Сдвигаем элементы в массиве в начало
void moveElementsInQueue(struct Queue *queue) {
    for (int i = 0; i < (queue->last-queue->head); i++) {
        queue->data[i] = queue->data[i+queue->head];
        queue->data[i+queue->head] = NULL;
    }
    queue->last = queue->last-queue->head;
    queue->head = 0;
}

// Достаём элемент из очереди
int popQueue(struct Queue *queue, void *element) {
    if (queue->head == queue->last) {
        fprintf(stderr, "Error: queue is empty\n");
        return -1;
    }
    if (queue->data[queue->head] == NULL) {
        fprintf(stderr, "Error: queue element is null");
        return -1;
    }
    if (element == NULL) {
        fprintf(stderr, "Error: queue get pointer to null");
    }
    memcpy(element, queue->data[queue->head], queue->sizeOfElement);
    free(queue->data[queue->head]);
    queue->head += 1;
    // Если 1/4 в начале очереди пустует, нужно сдвинуть элементы
    if (queue->head >= (queue->maxSize / 4)) {
        moveElementsInQueue(queue);
    }
    return 0;
}

// Уничтожить очередь
void destroyQueue(struct Queue *queue) {
    free(queue->data);
}
