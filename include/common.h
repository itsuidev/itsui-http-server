#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>

#define PORT 8000
#define BACKLOG 5
#define BUFFER_SIZE 1024
#define MAX_HEADERS 32

#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405

#define check_error(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            perror(msg);                                                                           \
            fprintf(stderr, "File: %s\nFunction: %s\nLine: %d\n", __FILE__, __func__, __LINE__);   \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#endif