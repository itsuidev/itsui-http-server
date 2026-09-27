#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

#include "server.h"
#include "http.h"
#include "common.h"

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    int server_fd = start_server(PORT);

    while(1) {
        int client_fd = accept_client(server_fd);
        set_client_timeout(client_fd, CLIENT_TIMEOUT_SEC);

        pid_t pid = fork();

        if (pid == -1) {
            send_response(client_fd, HTTP_INTERNAL_SERVER_ERROR, "Internal Server Error", "Server error\n", "", WITH_BODY);
            close(client_fd);
            continue;
        }

        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd);
            close(client_fd);
            _exit(0);
        }

        close(client_fd);
    }

    close(server_fd);
    return 0;
}