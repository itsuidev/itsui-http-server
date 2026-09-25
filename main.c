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

typedef struct {
    char method[16];
    char path[256];
    char version[16];
} HttpRequest;

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

int main(int argc, char **argv) {
    int server_fd = start_server(PORT);

    while(1) {
        struct sockaddr_in client_address;
        socklen_t client_len = sizeof(client_address);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_address, &client_len);
        check_error(client_fd >= 0, "Error while accepting client");

        char buffer[BUFFER_SIZE] = {0};

        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1);
        check_error(bytes_read >= 0, "Error while reading from client");

        char *line_end = strpbrk(buffer, "\r\n");
        if (line_end != NULL) {
            *line_end = '\0'; 
        }

        HttpRequest request;
        int parse_status = parse_request_line(buffer, &request);

        if (parse_status < 0) printf("Invalid HTTP request\n");
        else {
            printf("Method: %s\n", request.method);
            printf("Path: %s\n", request.path);
            printf("Version: %s\n", request.version);
        }

        const char *http_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "Hello custom C HTTP server\n";

        ssize_t bytes_written = write(client_fd, http_response, strlen(http_response));
        check_error(bytes_written >= 0, "Error while writing to client");

        close(client_fd);
        printf("Client connection closed.\n");
    }

    close(server_fd);
    exit(EXIT_SUCCESS);
}