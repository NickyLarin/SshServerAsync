#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "freadline.h"

#define MAX_FILENAME_LEN 20
#define MAX_LOGIN_LEN 13
#define MAX_PASS_LEN 13

// Глобальная переменная для завершения работы по сигналу
volatile sig_atomic_t done = 0;

// Перехватчик сигнала
void handleSigInt(int signum) {
    done = 1;
}

// Структура для хранения пары логин-пароль в файле
struct Data {
    char login[MAX_LOGIN_LEN];
    char pass[MAX_PASS_LEN];
};

int main(int argc, char *argv[]) {
    // Обработка сигнала
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = handleSigInt;
    sigaction(SIGINT, &act, 0);

    // Получаем имя файла и режим работы из параметров 
    char filename[MAX_FILENAME_LEN];
    char mode[3];
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            int length = strlen(argv[i+1]);
            if (length < 1 || length > sizeof(filename)/sizeof(filename[0])) {
                fprintf(stderr, "Error: wrong filename length\n");
                exit(EXIT_FAILURE);
            }
            strcpy(filename, argv[i+1]);
        }
        if (strcmp(argv[i], "-a") == 0) {
            strcpy(mode, "ab");
            break;
        }
        if (strcmp(argv[i], "-w") == 0) {
            strcpy(mode, "wb");
            break;
        }
    }
    printf("Filename: %s\n", filename);
    printf("Mode: %s\n", mode);

    // Начинаем запись в файл
    FILE *ptr;
    ptr = fopen(filename, mode);
    if (ptr == NULL) {
        fprintf(stderr, "Error: opening file for writing");
        exit(EXIT_FAILURE);
    }

    char login[MAX_LOGIN_LEN];
    char pass[MAX_PASS_LEN];

    do {
        printf("\nEnter login to add: \n");
        int status = fReadLine(login, sizeof(login)/sizeof(login[0]), stdin);
        if(status < 0) {
            fprintf(stderr, "Error: reading line from stdin\n");
            exit(EXIT_FAILURE);
        } else if (status > 0) {
            printf("Too long login, try again\n");
            continue;
        }
        if (strcmp(login, "") != 0) {
            printf("Enter password to add:\n");
            status = fReadLine(pass, sizeof(pass)/sizeof(pass[0]), stdin);
            if(status < 0) {
                fprintf(stderr, "Error: reading line from stdin\n");
                exit(EXIT_FAILURE);
            } else if (status > 0) {
                printf("Too long pass, try again\n");
                continue;
            }
            if(strcmp(pass, "") != 0) {
                struct Data data;
                strcpy(data.login, login);
                strcpy(data.pass, pass);
                fwrite(&data, sizeof(struct Data), 1, ptr);
                printf("Data writing done\n");
            }
        }
    } while (strcmp(login, "") != 0 && !done);

    fclose(ptr);
    return 0;
}
