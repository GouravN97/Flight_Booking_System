#include "admin_ipc.h"
#include "password.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

static size_t bounded_strlen(const char *s, size_t max_len) {
    size_t i = 0;
    while (i < max_len && s[i] != '\0') {
        i++;
    }
    return i;
}

static int append_admin_bus_line(ServerState *state, const char *line) {
    if (state->admin_bus == NULL || line == NULL) {
        return -1;
    }

    size_t used = bounded_strlen(state->admin_bus, ADMIN_SHM_SIZE);
    size_t remaining = (used < ADMIN_SHM_SIZE) ? (ADMIN_SHM_SIZE - used) : 0;
    size_t line_len = strlen(line);

    if (remaining <= line_len + 1) {
        memset(state->admin_bus, 0, ADMIN_SHM_SIZE);
        used = 0;
        remaining = ADMIN_SHM_SIZE;
    }

    if (used > 0 && state->admin_bus[used - 1] != '\n') {
        snprintf(state->admin_bus + used, remaining, "\n");
        used = bounded_strlen(state->admin_bus, ADMIN_SHM_SIZE);
        remaining = (used < ADMIN_SHM_SIZE) ? (ADMIN_SHM_SIZE - used) : 0;
    }

    if (remaining == 0) {
        return -1;
    }
    snprintf(state->admin_bus + used, remaining, "%s", line);
    return 0;
}

int publish_admin_message(ServerState *state, const char *message) {
    if (state->admin_bus == NULL) {
        return -1;
    }
    if (append_admin_bus_line(state, message) != 0) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (!state->admins[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    state->admins[slot].in_use = 1;
    snprintf(state->admins[slot].admin_id, sizeof(state->admins[slot].admin_id), "admin-%d", slot + 1);
    snprintf(state->admins[slot].message, sizeof(state->admins[slot].message), "%s", message);

    return storage_flush_admins(state);
}

int publish_admin_request(ServerState *state, const char *message) {
    if (state->admin_bus == NULL) {
        return -1;
    }
    return append_admin_bus_line(state, message);
}

int read_admin_message(ServerState *state, char *out, size_t out_size) {
    if (state->admin_bus == NULL || out == NULL || out_size == 0) {
        return -1;
    }
    snprintf(out, out_size, "%s", state->admin_bus);
    return 0;
}

int create_admin_user(ServerState *state, const char *admin_id, const char *password) {
    if (admin_id == NULL || password == NULL || admin_id[0] == '\0' || password[0] == '\0') {
        return -1;
    }

    for (int i = 0; i < MAX_ADMINS; i++) {
        if (state->admins[i].in_use &&
            strncmp(state->admins[i].admin_id, admin_id, sizeof(state->admins[i].admin_id)) == 0) {
            return -1;
        }
    }

    int slot = -1;
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (!state->admins[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    state->admins[slot].in_use = 1;
    snprintf(state->admins[slot].admin_id, sizeof(state->admins[slot].admin_id), "%s", admin_id);
    if (hash_password(password, state->admins[slot].password, sizeof(state->admins[slot].password)) != 0) {
        memset(&state->admins[slot], 0, sizeof(state->admins[slot]));
        return -1;
    }
    state->admins[slot].message[0] = '\0';
    return storage_flush_admins(state);
}

int verify_admin_user(ServerState *state, const char *admin_id, const char *password) {
    if (admin_id == NULL || password == NULL) {
        return -1;
    }
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (!state->admins[i].in_use) {
            continue;
        }
        if (strncmp(state->admins[i].admin_id, admin_id, sizeof(state->admins[i].admin_id)) != 0) {
            continue;
        }
        return verify_password(password, state->admins[i].password);
    }
    return -1;
}

int format_admin_list(ServerState *state, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return -1;
    }
    size_t off = 0;
    off += (size_t)snprintf(out + off, out_size - off, "Admins:\n");
    int found = 0;
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (!state->admins[i].in_use) {
            continue;
        }
        off += (size_t)snprintf(out + off, out_size - off, "%s\n", state->admins[i].admin_id);
        found = 1;
        if (off >= out_size - 1) {
            break;
        }
    }
    if (!found) {
        snprintf(out, out_size, "No admins found\n");
    }
    return 0;
}

int publish_admin_change(ServerState *state, const char *actor_admin_id, const char *change) {
    if (actor_admin_id == NULL || change == NULL) {
        return -1;
    }
    char line[320];
    snprintf(line, sizeof(line), "[%s] %s", actor_admin_id, change);
    return append_admin_bus_line(state, line);
}
