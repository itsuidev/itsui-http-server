#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "server.h"
#include "http.h"
#include "common.h"

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int server_fd = start_server(PORT);

    while(1) {
        int client_fd = accept_client(server_fd);
        set_client_timeout(client_fd, CLIENT_TIMEOUT_SEC);
        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}