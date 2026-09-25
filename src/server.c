#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "server.h"
#include "common.h"

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

int accept_client(int server_fd) {
    struct sockaddr_in client_address;
    socklen_t client_len = sizeof(client_address);

    int client_fd = accept(
        server_fd,
        (struct sockaddr *)&client_address,
        &client_len
    );

    check_error(client_fd >= 0, "Error while accepting client");

    return client_fd;
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

int read_request(int client_fd, char *buffer, size_t buffer_size) {
    size_t total_read = 0;
    
    while (strstr(buffer, "\r\n\r\n") == NULL) {
        if (total_read >= buffer_size - 1) return -2;
        ssize_t bytes_read = read(client_fd, buffer + total_read, buffer_size - 1 - total_read);
        if (bytes_read <= 0) return -1;
        total_read += bytes_read;
        buffer[total_read] = '\0';
    }

    return 0;
}