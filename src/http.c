#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "http.h"
#include "server.h"
#include "common.h"

int parse_request_line(const char *buffer, HttpRequest *request) {
    int result = sscanf(buffer, "%15s %255s %15s",
        request->method,
        request->path,
        request->version    
    );

    if (result != 3) return -1;

    char *question_mark = strchr(request->path, '?');

    if (question_mark != NULL) {
        *question_mark = '\0';

        strncpy(request->query, question_mark + 1, sizeof(request->query) - 1);
        request->query[sizeof(request->query) - 1] = '\0';
    } else {
        request->query[0] = '\0';
    }

    return 0;
}

int validate_request(const HttpRequest *request) {
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "HEAD") != 0) return 1;
    if (strcmp(request->version, "HTTP/1.1") != 0) return -1;
    if (request->path[0] != '/') return -1;
    return 0;
}

int parse_headers(char * buffer, HttpRequest * request) {
    request->header_count = 0;

    char *line = strtok(buffer, "\r\n");
    line = strtok(NULL, "\r\n");
    
    while (line != NULL) {
        char *colon = strchr(line, ':');
        if (colon == NULL) return -1;
        if (request->header_count >= MAX_HEADERS) return -1;

        HttpHeader *header = &request->headers[request->header_count];

        size_t name_length = colon - line;

        if (name_length >= sizeof(header->name)) {
            return -1;
        }

        memcpy(header->name, line, name_length);
        header->name[name_length] = '\0';

        char *value = colon + 1;
        while(*value == ' ') {
            value++;
        }

        size_t value_length = strlen(value);
        if (value_length >= sizeof(header->value)) return -1;
        memcpy(header->value, value, value_length);
        header->value[value_length] = '\0';

        request->header_count++;

        line = strtok(NULL, "\r\n");
    }

    return 0;
}

int send_response(int client_fd, int status_code, const char *status_text,
                  const char *body, const char *extra_headers, ResponseBody body_mode) {

    char header_buffer[BUFFER_SIZE];

    time_t now = time(NULL);
    struct tm *utc = gmtime(&now);
    char date[64];
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", utc);

    int header_length = snprintf(
        header_buffer,
        sizeof(header_buffer),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Date: %s\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        status_code,
        status_text,
        strlen(body), 
        date,
        extra_headers
    );
    if (header_length < 0 || (size_t)header_length >= (int)sizeof(header_buffer)) return -1;

    int header_status = send_all(client_fd, header_buffer, header_length);
    if (header_status != 0) return -1;

    size_t body_length = strlen(body);
    if (body_length == 0 || body_mode == WITHOUT_BODY) return 0;

    return send_all(client_fd, body, body_length);
}

static int hex_to_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }

    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    return -1;
}

int decode_component(char *str, DecodeMode mode) {
    char *read = str;
    char *write = str;

    while (*read != '\0') {
        if (*read == '%' && read[1] != '\0' && read[2] != '\0') {
            int high = hex_to_value(read[1]);
            int low = hex_to_value(read[2]);

            if (high < 0 || low < 0) return -1;

            int decoded = high * 16 + low;
            if (decoded == 0) return -1;

            *write = (char)decoded;
            read += 3;
            write++;
        } else if (*read == '+' && mode == DECODE_QUERY) {
            *write = ' ';
            read++;
            write++; 
        } else {
            *write = *read;
            read++;
            write++;
        }
    }
    *write = '\0';
    return 0;
}

int get_query_param(const HttpRequest *request, const char *name, char *value, size_t value_size) {
    char query[256];
    
    strncpy(query, request->query, sizeof(query) - 1);
    query[sizeof(query) - 1] = '\0';

    char *token = strtok(query, "&");

    while (token != NULL) {
        char *equals = strchr(token, '=');

        if (equals != NULL) {
            *equals = '\0';

            if (strcmp(token, name) == 0) {
                strncpy(value, equals + 1, value_size - 1);
                value[value_size - 1] = '\0';

                if (decode_component(value, DECODE_QUERY) < 0) return -1;

                return 0;
            }
        }

        token = strtok(NULL, "&");
    }

    return 1;
}

int get_header(const HttpRequest *request, const char *name, char *value, size_t value_size) {
    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) != 0) continue;
        strncpy(value, request->headers[i].value, value_size - 1);
        value[value_size - 1] = '\0';
        return 0;
    }
    return 1;
}

void handle_request(int client_fd, const HttpRequest *request) {
    ResponseBody body_mode = strcmp(request->method, "HEAD") == 0 ? WITHOUT_BODY : WITH_BODY;
    if (strcmp(request->path, "/") == 0) {
        send_response(client_fd, HTTP_OK, "OK", "Welcome to my C HTTP server!\n", "", body_mode);
        return;
    }

    if (strcmp(request->path, "/hello") == 0) {
        char name[256];

        int query_status = get_query_param(request, "name", name, sizeof(name));

        if (query_status == -1) {
            send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "", WITH_BODY);
            return;
        }
        
        if (query_status == 0) {
            char body[256];
            snprintf(body, sizeof(body), "Hello, %.246s!\n", name);
            send_response(client_fd, HTTP_OK, "OK", body, "", body_mode);
            return;
        }

        send_response(client_fd, HTTP_OK, "OK", "Hello!\n", "", body_mode);
        return;
    }

    if (strcmp(request->path, "/about") == 0) {
        send_response(client_fd, HTTP_OK, "OK", "This is my custom C HTTP server.\n", "", body_mode);
        return;
    }

    send_response(client_fd, HTTP_NOT_FOUND, "Not Found", "Not Found\n", "", body_mode);
}

void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};

    int read_status = read_request(client_fd, buffer, sizeof(buffer));
    if (read_status == -2) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Request too large\n", "", WITH_BODY);
        return;
    }

    if (read_status < 0) {
        return;
    }

    HttpRequest request;

    int parse_status = parse_request_line(buffer, &request);
    if (parse_status < 0) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "", WITH_BODY);
        return;
    }

    ResponseBody body_mode = strcmp(request.method, "HEAD") == 0 ? WITHOUT_BODY : WITH_BODY;

    int validation_status = validate_request(&request);
    if (validation_status == -1) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "", body_mode);
        return;
    }
    if (validation_status == 1) {
        send_response(client_fd, HTTP_METHOD_NOT_ALLOWED, "Method Not Allowed", "Method Not Allowed\n", "Allow: GET, HEAD\r\n", body_mode);
        return;
    }

    if (decode_component(request.path, DECODE_PATH) < 0) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "", body_mode);
        return;
    }

    int headers_status = parse_headers(buffer, &request);
    if (headers_status < 0) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "", body_mode);
        return;
    }

    char host[256];
    if (get_header(&request, "Host", host, sizeof(host)) != 0) {
        send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Missing Host header\n", "", body_mode);
        return;
    }
    
    handle_request(client_fd, &request);
}