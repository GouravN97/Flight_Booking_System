#ifndef AUTH_SERVICE_H
#define AUTH_SERVICE_H

#include "server_state.h"

int find_user_index(ServerState *state, const char *id);
int signup_user(ServerState *state, const char *id, const char *password);
int login_user(ServerState *state, const char *id, const char *password);

#endif
