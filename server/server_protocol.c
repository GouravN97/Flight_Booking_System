#include "server_protocol.h"
#include "admin_ipc.h"
#include "auth_service.h"
#include "booking_service.h"
#include "flight_service.h"
#include "token.h"
#include "waitlist_service.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void write_response(int fd, const char *response) {
    send(fd, response, strlen(response), 0);
}

static int write_token_ok(int fd, ServerState *state, char role, const char *subject) {
    char token[AUTH_TOKEN_SIZE];
    if (issue_auth_token(state->auth_secret, sizeof(state->auth_secret), role, subject, token, sizeof(token)) != 0) {
        write_response(fd, "ERR token issue failed\n");
        return -1;
    }

    char login_response[4096];
    size_t off = (size_t)snprintf(login_response, sizeof(login_response), "OK %s\n", token);
    if (role == AUTH_ROLE_USER && off < sizeof(login_response)) {
        (void)format_and_consume_notifications(state,
                                               subject,
                                               login_response + off,
                                               sizeof(login_response) - off);
    }
    write_response(fd, login_response);
    return 0;
}

static int require_token(ServerState *state,
                         const char *token,
                         char required_role,
                         char *subject,
                         size_t subject_size) {
    char role = 0;
    if (verify_auth_token(state->auth_secret,
                          sizeof(state->auth_secret),
                          token,
                          required_role,
                          subject,
                          subject_size,
                          &role) != 0) {
        return -1;
    }
    if (role == AUTH_ROLE_USER) {
        return find_user_index(state, subject) >= 0 ? 0 : -1;
    }
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (state->admins[i].in_use &&
            strncmp(state->admins[i].admin_id, subject, sizeof(state->admins[i].admin_id)) == 0 &&
            state->admins[i].password[0] != '\0') {
            return 0;
        }
    }
    return -1;
}

void handle_client(ServerState *state, int client_fd) {
    char buffer[1024];
    char response[4096];
    ssize_t n = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        return;
    }
    buffer[n] = '\0';

    if (strncmp(buffer, "PING", 4) == 0) {
        write_response(client_fd, "ONLINE\n");
        return;
    }

    if (strncmp(buffer, "SIGNUP ", 7) == 0) {
        char id[64], password[64];
        if (sscanf(buffer, "SIGNUP %63s %63s", id, password) != 2) {
            write_response(client_fd, "ERROR: invalid SIGNUP request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (signup_user(state, id, password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERROR: user exists or limit reached\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        (void)write_token_ok(client_fd, state, AUTH_ROLE_USER, id);
        return;
    }

    if (strncmp(buffer, "LOGIN ", 6) == 0) {
        char id[64], password[64];
        if (sscanf(buffer, "LOGIN %63s %63s", id, password) != 2) {
            write_response(client_fd, "ERR invalid LOGIN request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (login_user(state, id, password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR invalid credentials\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        (void)write_token_ok(client_fd, state, AUTH_ROLE_USER, id);
        return;
    }

    if (strncmp(buffer, "ADMIN_LOGIN ", 12) == 0) {
        char admin_id[64], password[64];
        if (sscanf(buffer, "ADMIN_LOGIN %63s %63s", admin_id, password) != 2) {
            write_response(client_fd, "ERR invalid ADMIN_LOGIN request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, admin_id, password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR invalid admin credentials\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        (void)write_token_ok(client_fd, state, AUTH_ROLE_ADMIN, admin_id);
        return;
    }

    if (strncmp(buffer, "LIST ", 5) == 0) {
        char token[AUTH_TOKEN_SIZE], source[64], destination[64], subject[64];
        if (sscanf(buffer, "LIST %191s %63s %63s", token, source, destination) != 3) {
            write_response(client_fd, "ERR invalid LIST request\n");
            return;
        }
        if (require_token(state, token, 0, subject, sizeof(subject)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }

        size_t off = 0;
        off += (size_t)snprintf(response + off, sizeof(response) - off, "Flights:\n");
        pthread_mutex_lock(&state->flights_lock);
        for (int i = 0; i < MAX_FLIGHTS; i++) {
            if (!state->flights[i].in_use) {
                continue;
            }
            if (strncmp(state->flights[i].source, source, sizeof(state->flights[i].source)) != 0 ||
                strncmp(state->flights[i].destination, destination, sizeof(state->flights[i].destination)) != 0) {
                continue;
            }
            int available = 0;
            sem_getvalue(&state->flights[i].seats_sem, &available);
            off += (size_t)snprintf(response + off,
                                    sizeof(response) - off,
                                    "%s date:%02d/%02d/%04d seats:%d/%d price:%.2f\n",
                                    state->flights[i].flight_number,
                                    state->flights[i].day,
                                    state->flights[i].month,
                                    state->flights[i].year,
                                    available,
                                    state->flights[i].num_seats,
                                    state->flights[i].price);
            if (off >= sizeof(response) - 1) {
                break;
            }
        }
        pthread_mutex_unlock(&state->flights_lock);
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "DETAIL ", 7) == 0) {
        char token[AUTH_TOKEN_SIZE], flight_number[24], subject[64];
        if (sscanf(buffer, "DETAIL %191s %23s", token, flight_number) != 2) {
            write_response(client_fd, "ERR invalid DETAIL request\n");
            return;
        }
        if (require_token(state, token, 0, subject, sizeof(subject)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }

        pthread_mutex_lock(&state->flights_lock);
        int idx = find_flight_index(state, flight_number);
        if (idx < 0) {
            pthread_mutex_unlock(&state->flights_lock);
            write_response(client_fd, "ERR flight not found\n");
            return;
        }

        int available = 0;
        sem_getvalue(&state->flights[idx].seats_sem, &available);
        snprintf(response,
                 sizeof(response),
                 "%s %s->%s date:%02d/%02d/%04d seats:%d/%d price:%.2f\n",
                 state->flights[idx].flight_number,
                 state->flights[idx].source,
                 state->flights[idx].destination,
                 state->flights[idx].day,
                 state->flights[idx].month,
                 state->flights[idx].year,
                 available,
                 state->flights[idx].num_seats,
                 state->flights[idx].price);
        pthread_mutex_unlock(&state->flights_lock);
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "BOOK ", 5) == 0) {
        char token[AUTH_TOKEN_SIZE], source[64], destination[64], booking_id[64], user_id[64];
        int day = 0, month = 0, year = 0, requested = 0;
        if (sscanf(buffer, "BOOK %191s %63s %63s %d %d %d %d",
                   token, source, destination, &day, &month, &year, &requested) != 7) {
            write_response(client_fd, "ERR invalid BOOK request\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_USER, user_id, sizeof(user_id)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }

        int booking_result = book_seats_for_user(state,
                                                 user_id,
                                                 source,
                                                 destination,
                                                 day,
                                                 month,
                                                 year,
                                                 requested,
                                                 booking_id,
                                                 sizeof(booking_id));

        if (booking_result == BOOKING_WAITLISTED) {
            write_response(client_fd, "WAITLISTED flight full; you will be booked when a new flight on this route is created\n");
            return;
        }
        if (booking_result == BOOKING_ADMIN_REQUESTED) {
            write_response(client_fd, "REQUESTED flight unavailable; admin creation request sent\n");
            return;
        }
        if (booking_result != 0) {
            write_response(client_fd, "ERR booking failed\n");
            return;
        }
        snprintf(response, sizeof(response), "OK %s\n", booking_id);
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "CANCEL ", 7) == 0) {
        char token[AUTH_TOKEN_SIZE], booking_id[64], user_id[64];
        if (sscanf(buffer, "CANCEL %191s %63s", token, booking_id) != 2) {
            write_response(client_fd, "ERR invalid CANCEL request\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_USER, user_id, sizeof(user_id)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        int fidx = -1;
        int bidx = find_booking_index(state, booking_id, &fidx);
        if (bidx < 0 || fidx < 0) {
            write_response(client_fd, "ERR booking not found\n");
            return;
        }
        if (strncmp(state->flights[fidx].bookings[bidx].user_id, user_id,
                    sizeof(state->flights[fidx].bookings[bidx].user_id)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (cancel_booking_by_id(state, booking_id) != 0) {
            write_response(client_fd, "ERR booking not found\n");
            return;
        }
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "MYBOOKINGS ", 11) == 0) {
        char token[AUTH_TOKEN_SIZE], user_id[64];
        if (sscanf(buffer, "MYBOOKINGS %191s", token) != 1) {
            write_response(client_fd, "ERR invalid MYBOOKINGS request\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_USER, user_id, sizeof(user_id)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (format_user_bookings(state, user_id, response, sizeof(response)) != 0) {
            write_response(client_fd, "ERR unable to fetch bookings\n");
            return;
        }
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "ADMINMSG ", 9) == 0) {
        char token[AUTH_TOKEN_SIZE], subject[64];
        if (sscanf(buffer, "ADMINMSG %191s", token) != 1) {
            write_response(client_fd, "ERR invalid ADMINMSG request\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_ADMIN, subject, sizeof(subject)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        const char *msg = buffer + 9;
        while (*msg != '\0' && *msg != ' ') {
            msg++;
        }
        if (*msg == ' ') {
            msg++;
        }
        pthread_mutex_lock(&state->state_lock);
        if (publish_admin_message(state, msg) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin bus unavailable\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "READADMIN ", 10) == 0 || strncmp(buffer, "READADMIN\n", 10) == 0) {
        char token[AUTH_TOKEN_SIZE], subject[64];
        if (sscanf(buffer, "READADMIN %191s", token) != 1) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_ADMIN, subject, sizeof(subject)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (read_admin_message(state, response, sizeof(response)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin bus unavailable\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "ADMIN_CREATE ", 13) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64], admin_id[64], password[64];
        if (sscanf(buffer, "ADMIN_CREATE %191s %63s %63s", token, admin_id, password) != 3) {
            write_response(client_fd, "ERR invalid ADMIN_CREATE request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (create_admin_user(state, admin_id, password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin create failed\n");
            return;
        }
        char change[256];
        snprintf(change, sizeof(change), "created admin %s", admin_id);
        publish_admin_change(state, actor_id, change);
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "ADMIN_CREATE_FLIGHT ", 20) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64], flight_number[24], source[64], destination[64];
        int day = 0, month = 0, year = 0, num_seats = 0;
        double price = 0.0;
        if (sscanf(buffer, "ADMIN_CREATE_FLIGHT %191s %23s %63s %63s %d %d %d %d %lf",
                   token, flight_number, source, destination,
                   &day, &month, &year, &num_seats, &price) != 9) {
            write_response(client_fd, "ERR invalid ADMIN_CREATE_FLIGHT request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);

        pthread_mutex_lock(&state->flights_lock);
        if (create_flight(state, flight_number, source, destination, day, month, year, num_seats, price) != 0) {
            pthread_mutex_unlock(&state->flights_lock);
            write_response(client_fd, "ERR create flight failed\n");
            return;
        }
        pthread_mutex_unlock(&state->flights_lock);

        pthread_mutex_lock(&state->state_lock);
        char change[256];
        snprintf(change,
                 sizeof(change),
                 "created flight %s %s->%s on %02d/%02d/%04d seats:%d price:%.2f",
                 flight_number,
                 source,
                 destination,
                 day,
                 month,
                 year,
                 num_seats,
                 price);
        publish_admin_change(state, actor_id, change);
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "ADMIN_CHANGE_TIMING ", 20) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64], flight_number[24];
        int day = 0, month = 0, year = 0;
        if (sscanf(buffer, "ADMIN_CHANGE_TIMING %191s %23s %d %d %d",
                   token, flight_number, &day, &month, &year) != 5) {
            write_response(client_fd, "ERR invalid ADMIN_CHANGE_TIMING request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (change_flight_timings(state, flight_number, day, month, year) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR change timing failed\n");
            return;
        }
        char change[256];
        snprintf(change, sizeof(change), "changed timing for %s to %02d/%02d/%04d", flight_number, day, month, year);
        publish_admin_change(state, actor_id, change);
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "ADMIN_UPDATE_PRICE ", 19) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64], flight_number[24];
        double price = 0.0;
        if (sscanf(buffer, "ADMIN_UPDATE_PRICE %191s %23s %lf", token, flight_number, &price) != 3) {
            write_response(client_fd, "ERR invalid ADMIN_UPDATE_PRICE request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (update_price(state, flight_number, price) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR update price failed\n");
            return;
        }
        char change[256];
        snprintf(change, sizeof(change), "updated price for %s to %.2f", flight_number, price);
        publish_admin_change(state, actor_id, change);
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "ADMIN_LIST_FLIGHTS ", 19) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64];
        if (sscanf(buffer, "ADMIN_LIST_FLIGHTS %191s", token) != 1) {
            write_response(client_fd, "ERR invalid ADMIN_LIST_FLIGHTS request\n");
            return;
        }
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }

        size_t off = 0;
        off += (size_t)snprintf(response + off, sizeof(response) - off, "All flights:\n");
        pthread_mutex_lock(&state->flights_lock);
        for (int i = 0; i < MAX_FLIGHTS; i++) {
            if (!state->flights[i].in_use) {
                continue;
            }
            int available = 0;
            sem_getvalue(&state->flights[i].seats_sem, &available);
            off += (size_t)snprintf(response + off,
                                    sizeof(response) - off,
                                    "%s %s->%s date:%02d/%02d/%04d seats:%d/%d price:%.2f\n",
                                    state->flights[i].flight_number,
                                    state->flights[i].source,
                                    state->flights[i].destination,
                                    state->flights[i].day,
                                    state->flights[i].month,
                                    state->flights[i].year,
                                    available,
                                    state->flights[i].num_seats,
                                    state->flights[i].price);
            if (off >= sizeof(response) - 1) {
                break;
            }
        }
        pthread_mutex_unlock(&state->flights_lock);
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "ADMIN_LIST_ADMINS ", 18) == 0) {
        char token[AUTH_TOKEN_SIZE], actor_id[64];
        if (sscanf(buffer, "ADMIN_LIST_ADMINS %191s", token) != 1) {
            write_response(client_fd, "ERR invalid ADMIN_LIST_ADMINS request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (require_token(state, token, AUTH_ROLE_ADMIN, actor_id, sizeof(actor_id)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unauthorized\n");
            return;
        }
        if (format_admin_list(state, response, sizeof(response)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR cannot list admins\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, response);
        return;
    }

    write_response(client_fd, "UNSUPPORTED\n");
}
