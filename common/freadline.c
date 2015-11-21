#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freadline.h"

#define BEGIN_BUFFER_SIZE 64

// Прочитать строку размера size из файла fptr в dest
int fReadLine(char *dest, int size, FILE *fptr) {
    if (fptr == NULL) {
        fprintf(stderr, "Error: file pointer is null");
        return -1;
    }
    
    if (dest == NULL) {
        fprintf(stderr, "Error: destination pointer is null");
        return -1;
    }

    int bufferSize = BEGIN_BUFFER_SIZE;
    char *buffer = (char *)malloc(bufferSize * sizeof(char));

    if (buffer == NULL) {
        fprintf(stderr, "Error: Allocating memory for buffer");
        return -1;
    }

    char ch = getc(fptr);
    int count = 0;

    while ((ch != '\n') && (ch != EOF)) {
        if (count == bufferSize){
            bufferSize *= 2;
            buffer = realloc(buffer, bufferSize * sizeof(char));
            if(buffer == NULL){
                fprintf(stderr, "Error: Allocating memory");
                return -1;
            }
        }
        buffer[count] = ch;
        count++;
        ch = getc(fptr);
    }

    buffer[count] = '\0';
    if (strlen(buffer) > size) {
        return 1;
    }
    strcpy(dest, buffer);
    free(buffer);
    return 0;
}
