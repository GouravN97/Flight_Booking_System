#ifndef ADMIN_IPC_H
#define ADMIN_IPC_H

#include "server_state.h"

int publish_admin_message(ServerState *state, const char *message);
int publish_admin_request(ServerState *state, const char *message);
int read_admin_message(ServerState *state, char *out, size_t out_size);
int create_admin_user(ServerState *state, const char *admin_id, const char *password);
int verify_admin_user(ServerState *state, const char *admin_id, const char *password);
int format_admin_list(ServerState *state, char *out, size_t out_size);
int publish_admin_change(ServerState *state, const char *actor_admin_id, const char *change);

#endif
