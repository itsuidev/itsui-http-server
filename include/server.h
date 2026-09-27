#ifndef SERVER_H
#define SERVER_H

#include <stddef.h>

int start_server(int port);
int accept_client(int server_fd);
int set_client_timeout(int client_fd, int seconds);
int send_all(int client_fd, const char *buffer, size_t length);
int read_request(int client_fd, char *buffer, size_t buffer_size);

#endif