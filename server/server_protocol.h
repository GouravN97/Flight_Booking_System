#ifndef SERVER_PROTOCOL_H
#define SERVER_PROTOCOL_H

#include "server_state.h"

void handle_client(ServerState *state, int client_fd);

#endif
