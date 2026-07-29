#include "server_protocol.h"
#include "admin_ipc.h"
#include "auth_service.h"
#include "booking_service.h"
#include "flight_service.h"
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

static void write_response(int fd, const char *response) {
    send(fd, response, strlen(response), 0);
}

void handle_client(ServerState *state, int client_fd) {
    char buffer[512];
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
        write_response(client_fd, "OK\n");
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
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "LIST ", 5) == 0) {
        char source[64], destination[64];
        if (sscanf(buffer, "LIST %63s %63s", source, destination) != 2) {
            write_response(client_fd, "ERR invalid LIST request\n");
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
        char flight_number[24];
        if (sscanf(buffer, "DETAIL %23s", flight_number) != 1) {
            write_response(client_fd, "ERR invalid DETAIL request\n");
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
        char user_id[64], source[64], destination[64], booking_id[64];
        int day = 0, month = 0, year = 0, requested = 0;
        if (sscanf(buffer, "BOOK %63s %63s %63s %d %d %d %d",
                   user_id, source, destination, &day, &month, &year, &requested) != 7) {
            write_response(client_fd, "ERR invalid BOOK request\n");
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
        char booking_id[64];
        if (sscanf(buffer, "CANCEL %63s", booking_id) != 1) {
            write_response(client_fd, "ERR invalid CANCEL request\n");
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
        char user_id[64];
        if (sscanf(buffer, "MYBOOKINGS %63s", user_id) != 1) {
            write_response(client_fd, "ERR invalid MYBOOKINGS request\n");
            return;
        }
        if (format_user_bookings(state, user_id, response, sizeof(response)) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR unable to fetch bookings\n");
            return;
        }
        write_response(client_fd, response);
        return;
    }

    if (strncmp(buffer, "ADMINMSG ", 9) == 0) {
        pthread_mutex_lock(&state->state_lock);
        if (publish_admin_message(state, buffer + 9) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin bus unavailable\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "READADMIN", 9) == 0) {
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
        char actor_id[64], actor_password[64], admin_id[64], password[64];
        if (sscanf(buffer, "ADMIN_CREATE %63s %63s %63s %63s",
                   actor_id, actor_password, admin_id, password) != 4) {
            write_response(client_fd, "ERR invalid ADMIN_CREATE request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
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
        char actor_id[64], actor_password[64], flight_number[24], source[64], destination[64];
        int day = 0, month = 0, year = 0, num_seats = 0;
        double price = 0.0;
        if (sscanf(buffer, "ADMIN_CREATE_FLIGHT %63s %63s %23s %63s %63s %d %d %d %d %lf",
                   actor_id, actor_password, flight_number, source, destination,
                   &day, &month, &year, &num_seats, &price) != 10) {
            write_response(client_fd, "ERR invalid ADMIN_CREATE_FLIGHT request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
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
        char actor_id[64], actor_password[64], flight_number[24];
        int day = 0, month = 0, year = 0;
        if (sscanf(buffer, "ADMIN_CHANGE_TIMING %63s %63s %23s %d %d %d",
                   actor_id, actor_password, flight_number, &day, &month, &year) != 6) {
            write_response(client_fd, "ERR invalid ADMIN_CHANGE_TIMING request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
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
        char actor_id[64], actor_password[64], flight_number[24];
        double price = 0.0;
        if (sscanf(buffer, "ADMIN_UPDATE_PRICE %63s %63s %23s %lf",
                   actor_id, actor_password, flight_number, &price) != 4) {
            write_response(client_fd, "ERR invalid ADMIN_UPDATE_PRICE request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
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
        char actor_id[64], actor_password[64];
        if (sscanf(buffer, "ADMIN_LIST_FLIGHTS %63s %63s", actor_id, actor_password) != 2) {
            write_response(client_fd, "ERR invalid ADMIN_LIST_FLIGHTS request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
            return;
        }
        pthread_mutex_unlock(&state->state_lock);
        
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
        write_response(client_fd, "OK\n");
        return;
    }

    if (strncmp(buffer, "ADMIN_LIST_ADMINS ", 18) == 0) {
        char actor_id[64], actor_password[64];
        if (sscanf(buffer, "ADMIN_LIST_ADMINS %63s %63s", actor_id, actor_password) != 2) {
            write_response(client_fd, "ERR invalid ADMIN_LIST_ADMINS request\n");
            return;
        }
        pthread_mutex_lock(&state->state_lock);
        if (verify_admin_user(state, actor_id, actor_password) != 0) {
            pthread_mutex_unlock(&state->state_lock);
            write_response(client_fd, "ERR admin auth failed\n");
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
