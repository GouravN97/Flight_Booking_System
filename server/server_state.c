#define _POSIX_C_SOURCE 200809L
#include "server_state.h"
#include "admin_ipc.h"
#include "auth_service.h"
#include "storage_service.h"
#include "token.h"
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int server_init(ServerState *state) {
    memset(state, 0, sizeof(*state));
    pthread_mutex_init(&state->state_lock, NULL);
    pthread_mutex_init(&state->flights_lock, NULL);
    pthread_mutex_init(&state->requests_lock, NULL);
    pthread_mutex_init(&state->waitlist_lock, NULL);

    if (load_or_create_auth_secret(state->auth_secret, sizeof(state->auth_secret)) != 0) {
        pthread_mutex_destroy(&state->state_lock);
        pthread_mutex_destroy(&state->flights_lock);
        pthread_mutex_destroy(&state->requests_lock);
        pthread_mutex_destroy(&state->waitlist_lock);
        return -1;
    }

    if (storage_init(state) != 0) {
        pthread_mutex_destroy(&state->state_lock);
        pthread_mutex_destroy(&state->flights_lock);
        pthread_mutex_destroy(&state->requests_lock);
        pthread_mutex_destroy(&state->waitlist_lock);
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
            pthread_mutex_destroy(&state->flights_lock);
            pthread_mutex_destroy(&state->requests_lock);
            pthread_mutex_destroy(&state->waitlist_lock);
            return -1;
        }
    }

    state->shm_fd = shm_open(ADMIN_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (state->shm_fd < 0) {
        pthread_mutex_destroy(&state->state_lock);
        pthread_mutex_destroy(&state->flights_lock);
        pthread_mutex_destroy(&state->requests_lock);
        pthread_mutex_destroy(&state->waitlist_lock);
        return -1;
    }
    ftruncate(state->shm_fd, ADMIN_SHM_SIZE);

    state->admin_bus = mmap(NULL, ADMIN_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, state->shm_fd, 0);
    if (state->admin_bus == MAP_FAILED) {
        pthread_mutex_destroy(&state->state_lock);
        pthread_mutex_destroy(&state->flights_lock);
        pthread_mutex_destroy(&state->requests_lock);
        pthread_mutex_destroy(&state->waitlist_lock);
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
        for (int s = 0; s < state->flights[i].num_seats; s++) {
            pthread_mutex_destroy(&state->flights[i].seat_mutexes[s]);
        }
    }

    pthread_mutex_unlock(&state->state_lock);
    pthread_mutex_destroy(&state->state_lock);
    pthread_mutex_destroy(&state->flights_lock);
    pthread_mutex_destroy(&state->requests_lock);
    pthread_mutex_destroy(&state->waitlist_lock);
}
