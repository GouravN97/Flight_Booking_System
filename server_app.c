#include "server.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static ServerState g_state;

static void handle_signal(int signo) {
    (void)signo;
    server_shutdown(&g_state);
    exit(0);
}

int main(int argc, char **argv) {
    uint16_t port = 9090;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p <= 0 || p > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
        }
        port = (uint16_t)p;
    }

    if (server_init(&g_state) != 0) {
        fprintf(stderr, "Failed to initialize server state\n");
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("Server listening on port %u\n", (unsigned int)port);
    if (start_server(&g_state, port) != 0) {
        fprintf(stderr, "Failed to start server\n");
        server_shutdown(&g_state);
        return 1;
    }

    server_shutdown(&g_state);
    return 0;
}
