#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include "common.h"

typedef enum {
    WITH_BODY,
    WITHOUT_BODY
} ResponseBody;

typedef enum {
    DECODE_QUERY,
    DECODE_PATH
} DecodeMode;

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

typedef enum {
    HTTP_OK = 200,
    HTTP_BAD_REQUEST = 400,
    HTTP_NOT_FOUND = 404,
    HTTP_METHOD_NOT_ALLOWED = 405,
    HTTP_INTERNAL_SERVER_ERROR = 500,
    HTTP_NOT_IMPLEMENTED = 501
} HttpStatus;

typedef enum {
    REQUEST_VALID,
    REQUEST_MALFORMED,
    REQUEST_METHOD_NOT_ALLOWED,
    REQUEST_METHOD_UNRECOGNISED
} ValidationResult;

const char *status_text(HttpStatus status);

int parse_request_line(const char *buffer, HttpRequest *request);
ValidationResult validate_request(const HttpRequest *request);
int get_header(const HttpRequest *request, const char *name, char *value, size_t value_size);
int parse_headers(char *buffer, HttpRequest *request);
int send_response(int client_fd, HttpStatus status, const char *body, const char *extra_headers, ResponseBody body_mode);
int get_query_param(const HttpRequest *request, const char *name, char *value, size_t value_size);
int decode_component(char *str, DecodeMode mode);
void handle_request(int client_fd, const HttpRequest *request);
void handle_client(int client_fd);

#endif