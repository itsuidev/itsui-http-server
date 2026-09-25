#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define check_error(cond, msg)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            perror(msg);                                                                           \
            fprintf(stderr, "File: %s\nFunction: %s\nLine: %d\n", __FILE__, __func__, __LINE__);   \
            exit(EXIT_FAILURE);                                                                    \
        }                                                                                          \
    } while (0)

#define PORT 8080
#define BACKLOG 5
#define BUFFER_SIZE 1024
#define MAX_HEADERS 32

#define HTTP_OK 200
#define HTTP_BAD_REQUEST 400
#define HTTP_NOT_FOUND 404
#define HTTP_METHOD_NOT_ALLOWED 405

typedef struct {
    char name[64];
    char value[256];
} HttpHeader;

typedef struct {
    char method[16];
    char path[256];
    char version[16];

    HttpHeader headers[MAX_HEADERS];
    int header_count;
} HttpRequest;

int send_all(int client_fd, const char *buffer, size_t length);
void send_response(int client_fd, int status_code, const char *status_text, const char *body, const char *extra_headers);

int start_server(int port) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    check_error(server_fd >= 0, "Error while creating socket");
    printf("Socket created successfuly. File descriptor is: %d\n", server_fd);

    int opt = 1;
    int opt_status = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    check_error(opt_status == 0, "Error while setting socket options");

    struct sockaddr_in server_address;
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(port);

    int bind_status = bind(server_fd, (struct sockaddr *)&server_address, sizeof(server_address));
    check_error(bind_status == 0, "Error while binding socket");
    printf("Socket bound to port %d successfully.\n", port);

    int listen_status = listen(server_fd, BACKLOG);
    check_error(listen_status == 0, "Error while listening");
    printf("Server is listening on port %d...\n", port);

    return server_fd;
}

int parse_request_line(const char *buffer, HttpRequest *request) {
    int result = sscanf(buffer, "%15s %255s %15s",
        request->method,
        request->path,
        request->version    
    );

    if (result != 3) return -1;
    return 0;
}

int validate_request(const HttpRequest *request) {
    if (strcmp(request->method, "GET") != 0) return 1;
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

int send_all(int client_fd, const char *buffer, size_t length) {
    size_t total_sent = 0;

    while (total_sent < length) {
        ssize_t bytes_written = write(client_fd, buffer + total_sent, length - total_sent);
        if (bytes_written <= 0) return -1;
        total_sent += bytes_written;
    }
    
    return 0;
}

void send_response(int client_fd, int status_code, const char *status_text, const char *body, const char *extra_headers) {
    char response[1024];

    int response_length = snprintf(
        response,
        sizeof(response),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n"
        "%s",
        status_code,
        status_text,
        strlen(body),
        extra_headers,
        body
    );

    check_error(
        response_length >= 0 && (size_t)response_length < sizeof(response),
        "Error while creating HTTP response"
    );

    int send_status = send_all(client_fd, response, response_length);
    check_error(send_status == 0, "Error while writing HTTP response");
}

void handle_request(int client_fd, const HttpRequest *request) {
    if (strcmp(request->path, "/") == 0) {
        send_response(client_fd, HTTP_OK, "OK", "Welcome to my C HTTP server!\n", "");
        return;
    }

    if (strcmp(request->path, "/hello") == 0) {
        send_response(client_fd, HTTP_OK, "OK", "Hello!\n", "");
        return;
    }

    if (strcmp(request->path, "/about") == 0) {
        send_response(client_fd, HTTP_OK, "OK", "This is my custom C HTTP server.\n", "");
        return;
    }

    send_response(client_fd, HTTP_NOT_FOUND, "Not Found", "Not Found\n", "");
}

int main(int argc, char **argv) {
    int server_fd = start_server(PORT);

    while(1) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_len);
        check_error(client_fd >= 0, "Error while accepting client");

        char buffer[BUFFER_SIZE] = {0};
        size_t total_read = 0;

        while (strstr(buffer, "\r\n\r\n") == NULL) {
            if (total_read >= BUFFER_SIZE - 1) {
                send_response(client_fd,  HTTP_BAD_REQUEST, "Bad Request", "Request too large\n", "");
                close(client_fd);
                break;
            }

            ssize_t bytes_read = read(client_fd, buffer + total_read, BUFFER_SIZE - 1 - total_read);
            check_error(bytes_read >= 0, "Error while reading from client");

            if (bytes_read == 0) {
                close(client_fd);
                break;
            }

            total_read += bytes_read;
            buffer[total_read] = '\0';
        }

        printf("Finished reading request.\n");
        printf("Buffer:\n%s\n", buffer);
        
        if (strstr(buffer, "\r\n\r\n") == NULL) continue;

        HttpRequest request;

        int parse_status = parse_request_line(buffer, &request);
        if (parse_status < 0) {
            send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "");
            close(client_fd);
            continue;
        }

        int validation_status = validate_request(&request);
        if (validation_status == -1) {
            send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "");
            close(client_fd);
            continue;
        }
         if (validation_status == 1) {
            send_response(client_fd, HTTP_METHOD_NOT_ALLOWED, "Method Not Allowed", "Method Not Allowed\n", "Allow: GET\r\n");
            close(client_fd);
            continue;
        }

        printf("Method: %s\n", request.method);
        printf("Path: %s\n", request.path);
        printf("Version: %s\n", request.version);

        int headers_status = parse_headers(buffer, &request);
        if (headers_status < 0) {
            send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Bad Request\n", "");
            close(client_fd);
            continue;
        }
        
        for (int i = 0; i < request.header_count; i++) {
            printf("Header name: %s\n", request.headers[i].name);
            printf("Header value: %s\n", request.headers[i].value);
        }

        handle_request(client_fd, &request);
        close(client_fd);
        printf("Client connection closed.\n");
    }

    close(server_fd);
    exit(EXIT_SUCCESS);
}