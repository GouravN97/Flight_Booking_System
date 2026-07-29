#ifndef SERVER_MAIN_H
#define SERVER_MAIN_H

#include <stdint.h>
#include "server_state.h"

int start_server(ServerState *state, uint16_t port);

#endif
