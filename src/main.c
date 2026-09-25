#include <stdio.h>
#include <unistd.h>

#include "server.h"
#include "http.h"
#include "common.h"

int main(void) {
    int server_fd = start_server(PORT);

    while(1) {
        int client_fd = accept_client(server_fd);
        char buffer[BUFFER_SIZE] = {0};

        int read_status = read_request(client_fd, buffer, sizeof(buffer));
        if (read_status == -2) {
            send_response(client_fd, HTTP_BAD_REQUEST, "Bad Request", "Request too large\n", "");
            close(client_fd);
            continue;
        }

        if (read_status < 0) {
            close(client_fd);
            continue;
        }

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
    return 0;
}