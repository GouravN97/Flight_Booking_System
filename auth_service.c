#include "auth_service.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

int find_user_index(ServerState *state, const char *id) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (state->users[i].in_use && strncmp(state->users[i].id, id, sizeof(state->users[i].id)) == 0) {
            return i;
        }
    }
    return -1;
}

int signup_user(ServerState *state, const char *id, const char *password) {
    if (find_user_index(state, id) >= 0) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!state->users[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        return -1;
    }

    state->users[slot].in_use = 1;
    snprintf(state->users[slot].id, sizeof(state->users[slot].id), "%s", id);
    snprintf(state->users[slot].password, sizeof(state->users[slot].password), "%s", password);
    return storage_flush_users(state);
}

int login_user(ServerState *state, const char *id, const char *password) {
    int idx = find_user_index(state, id);
    if (idx < 0) {
        return -1;
    }

    if (strncmp(state->users[idx].password, password, sizeof(state->users[idx].password)) != 0) {
        return -1;
    }

    return 0;
}
