#define _POSIX_C_SOURCE 200809L
#include "server_state.h"
#include "admin_ipc.h"
#include "storage_service.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


//should servers be zero-initialized?
int server_init(ServerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->state_lock, NULL);

    if (storage_init(state) != 0) {
        pthread_mutex_destroy(&state->state_lock);
        return -1;
    }

    int has_admin_with_password = 0;
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (state->admins[i].in_use && state->admins[i].password[0] != '\0') {
            has_admin_with_password = 1;
            break;
        }
    }
    if (!has_admin_with_password) {
        if (create_admin_user(state, "admin", "admin123") != 0) {
            pthread_mutex_destroy(&state->state_lock);
            return -1;
        }
    }

    state->shm_fd = shm_open(ADMIN_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (state->shm_fd < 0) {
        pthread_mutex_destroy(&state->state_lock);
        return -1;
    }
    ftruncate(state->shm_fd, ADMIN_SHM_SIZE);

    state->admin_bus = mmap(NULL, ADMIN_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, state->shm_fd, 0);
    if (state->admin_bus == MAP_FAILED) {
        pthread_mutex_destroy(&state->state_lock);
        return -1;
    }

    memset(state->admin_bus, 0, ADMIN_SHM_SIZE);
    return 0;
}

void server_shutdown(ServerState *state) {
    pthread_mutex_lock(&state->state_lock);
    storage_shutdown(state);

    if (state->admin_bus != NULL && state->admin_bus != MAP_FAILED) {
        munmap(state->admin_bus, ADMIN_SHM_SIZE);
    }
    if (state->shm_fd >= 0) {
        close(state->shm_fd);
        shm_unlink(ADMIN_SHM_NAME);
    }

    for (int i = 0; i < MAX_FLIGHTS; i++) {
        if (!state->flights[i].in_use) {
            continue;
        }
        sem_destroy(&state->flights[i].seats_sem);
        pthread_mutex_destroy(&state->flights[i].flight_mutex);
        for (int s = 0; s < state->flights[i].num_seats; s++) {
            pthread_mutex_destroy(&state->flights[i].seats[s].seat_mutex);
        }
    }

    pthread_mutex_unlock(&state->state_lock);
    pthread_mutex_destroy(&state->state_lock);
}
