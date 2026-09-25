#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "common.h"

typedef struct {
    char name[64];
    char value[256];
} HttpHeader;

typedef struct {
    char method[16];
    char path[256];
    char query[256];
    char version[16];

    HttpHeader headers[MAX_HEADERS];
    int header_count;
} HttpRequest;

int parse_request_line(const char *buffer, HttpRequest *request);
int validate_request(const HttpRequest *request);
int parse_headers(char *buffer, HttpRequest *request);
void send_response(int client_fd, int status_code, const char *status_text, const char *body, const char *extra_headers);
int get_query_param(const HttpRequest *request, const char *name, char *value, size_t value_size);
int url_decode(char *str);
void handle_request(int client_fd, const HttpRequest *request);

#endif