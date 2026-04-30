#include "booking_service.h"
#include "auth_service.h"
#include "flight_service.h"
#include "admin_ipc.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

int find_booking_index(ServerState *state, const char *booking_id) {
    for (int i = 0; i < MAX_BOOKINGS; i++) {
        if (state->bookings[i].in_use &&
            strncmp(state->bookings[i].booking_id, booking_id, sizeof(state->bookings[i].booking_id)) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_matching_flight_with_capacity(ServerState *state,
                                              const char *source,
                                              const char *destination,
                                              int day,
                                              int month,
                                              int year,
                                              int requested) {
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        if (!state->flights[i].in_use) {
            continue;
        }
        if (strncmp(state->flights[i].source, source, sizeof(state->flights[i].source)) != 0 ||
            strncmp(state->flights[i].destination, destination, sizeof(state->flights[i].destination)) != 0 ||
            state->flights[i].day != day ||
            state->flights[i].month != month ||
            state->flights[i].year != year) {
            continue;
        }

        int available = 0;
        sem_getvalue(&state->flights[i].seats_sem, &available);
        if (available >= requested) {
            return i;
        }
    }
    return -1;
}

static int has_matching_flight(ServerState *state,
                               const char *source,
                               const char *destination,
                               int day,
                               int month,
                               int year) {
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        if (!state->flights[i].in_use) {
            continue;
        }
        if (strncmp(state->flights[i].source, source, sizeof(state->flights[i].source)) == 0 &&
            strncmp(state->flights[i].destination, destination, sizeof(state->flights[i].destination)) == 0 &&
            state->flights[i].day == day &&
            state->flights[i].month == month &&
            state->flights[i].year == year) {
            return 1;
        }
    }
    return 0;
}

static void generate_booking_id(ServerState *state, char *out, size_t out_size) {
    for (int i = 1; i <= MAX_BOOKINGS; i++) {
        snprintf(out, out_size, "BK%06d", i);
        if (find_booking_index(state, out) < 0) {
            return;
        }
    }
    if (out_size > 0) {
        out[0] = '\0';
    }
}

static int request_matching_flight_from_admins(ServerState *state,
                                               const char *user_id,
                                               const char *source,
                                               const char *destination,
                                               int day,
                                               int month,
                                               int year,
                                               int requested) {
    int suggested_capacity = requested > 10 ? requested : 10;
    if (suggested_capacity > MAX_SEATS_PER_FLIGHT) {
        suggested_capacity = MAX_SEATS_PER_FLIGHT;
    }

    const char *reason = has_matching_flight(state, source, destination, day, month, year)
                             ? "all matching flights are full or do not have enough seats"
                             : "no matching flight exists";
    char admin_message[512];
    snprintf(admin_message,
             sizeof(admin_message),
             "Flight creation requested by user %s: %s->%s on %02d/%02d/%04d, seats requested:%d, suggested capacity:%d, reason:%s",
             user_id,
             source,
             destination,
             day,
             month,
             year,
             requested,
             suggested_capacity,
             reason);

    return publish_admin_request(state, admin_message);
}

int book_seats_for_user(ServerState *state,
                        const char *user_id,
                        const char *source,
                        const char *destination,
                        int day,
                        int month,
                        int year,
                        int requested,
                        char *out_booking_id,
                        size_t out_booking_id_size) {
    if (requested <= 0 || requested > MAX_SEATS_PER_FLIGHT) {
        return -1;
    }

    int user_idx = find_user_index(state, user_id);
    if (user_idx < 0) {
        return -2;
    }

    if (out_booking_id == NULL || out_booking_id_size == 0) {
        return -3;
    }

    int fidx = find_matching_flight_with_capacity(state, source, destination, day, month, year, requested);
    if (fidx < 0) {
        if (request_matching_flight_from_admins(state,
                                               user_id,
                                               source,
                                               destination,
                                               day,
                                               month,
                                               year,
                                               requested) != 0) {
            return -4;
        }
        return BOOKING_ADMIN_REQUESTED;
    }

    char booking_id[64];
    generate_booking_id(state, booking_id, sizeof(booking_id));
    if (booking_id[0] == '\0') {
        return -3;
    }

    Flight *f = &state->flights[fidx];
    pthread_mutex_lock(&f->flight_mutex);

    int acquired = 0;
    for (int i = 0; i < requested; i++) {
        if (sem_trywait(&f->seats_sem) != 0) {
            break;
        }
        acquired++;
    }
    if (acquired != requested) {
        for (int i = 0; i < acquired; i++) {
            sem_post(&f->seats_sem);
        }
        pthread_mutex_unlock(&f->flight_mutex);
        return -5;
    }

    int allocated[MAX_SEATS_PER_FLIGHT];
    int allocated_count = 0;
    for (int i = 0; i < f->num_seats && allocated_count < requested; i++) {
        pthread_mutex_lock(&f->seats[i].seat_mutex);
        if (!f->seats[i].booked) {
            f->seats[i].booked = true;
            snprintf(f->seats[i].passenger_id, sizeof(f->seats[i].passenger_id), "%s", user_id);
            allocated[allocated_count++] = f->seats[i].seat_number;
        }
        pthread_mutex_unlock(&f->seats[i].seat_mutex);
    }
    pthread_mutex_unlock(&f->flight_mutex);

    if (allocated_count != requested) {
        for (int i = 0; i < allocated_count; i++) {
            sem_post(&f->seats_sem);
        }
        return -6;
    }

    int slot = -1;
    for (int i = 0; i < MAX_BOOKINGS; i++) {
        if (!state->bookings[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        state->bookings[slot].in_use = 1;
        state->bookings[slot].seat_count = requested;
        snprintf(state->bookings[slot].booking_id, sizeof(state->bookings[slot].booking_id), "%s", booking_id);
        snprintf(state->bookings[slot].user_id, sizeof(state->bookings[slot].user_id), "%s", user_id);
        snprintf(state->bookings[slot].flight_number, sizeof(state->bookings[slot].flight_number), "%s", f->flight_number);
        for (int i = 0; i < requested; i++) {
            state->bookings[slot].seat_numbers[i] = allocated[i];
        }
    }
    if (slot < 0) {
        return -7;
    }
    if (storage_flush_flights(state) != 0 || storage_flush_bookings(state) != 0) {
        return -8;
    }
    snprintf(out_booking_id, out_booking_id_size, "%s", booking_id);
    return 0;
}

int cancel_booking_by_id(ServerState *state, const char *booking_id) {
    int bidx = find_booking_index(state, booking_id);
    if (bidx < 0) {
        return -1;
    }
    BookingRecord booking = state->bookings[bidx];
    state->bookings[bidx].in_use = 0;

    int fidx = find_flight_index(state, booking.flight_number);
    if (fidx >= 0) {
        Flight *f = &state->flights[fidx];
        pthread_mutex_lock(&f->flight_mutex);
        for (int i = 0; i < booking.seat_count; i++) {
            int seat_idx = booking.seat_numbers[i] - 1;
            if (seat_idx >= 0 && seat_idx < f->num_seats) {
                pthread_mutex_lock(&f->seats[seat_idx].seat_mutex);
                f->seats[seat_idx].booked = false;
                f->seats[seat_idx].passenger_id[0] = '\0';
                pthread_mutex_unlock(&f->seats[seat_idx].seat_mutex);
                sem_post(&f->seats_sem);
            }
        }
        pthread_mutex_unlock(&f->flight_mutex);
    }
    if (storage_flush_flights(state) != 0 || storage_delete_booking_by_index(bidx) != 0) {
        return -2;
    }
    return 0;
}

int format_user_bookings(ServerState *state, const char *user_id, char *out, size_t out_size) {
    if (user_id == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    size_t off = 0;
    off += (size_t)snprintf(out + off, out_size - off, "Bookings for %s:\n", user_id);
    int found = 0;

    for (int i = 0; i < MAX_BOOKINGS; i++) {
        if (!state->bookings[i].in_use) {
            continue;
        }
        if (strncmp(state->bookings[i].user_id, user_id, sizeof(state->bookings[i].user_id)) != 0) {
            continue;
        }

        int fidx = find_flight_index(state, state->bookings[i].flight_number);
        if (fidx >= 0) {
            off += (size_t)snprintf(out + off,
                                    out_size - off,
                                    "%s flight:%s %s->%s date:%02d/%02d/%04d seats:%d\n",
                                    state->bookings[i].booking_id,
                                    state->bookings[i].flight_number,
                                    state->flights[fidx].source,
                                    state->flights[fidx].destination,
                                    state->flights[fidx].day,
                                    state->flights[fidx].month,
                                    state->flights[fidx].year,
                                    state->bookings[i].seat_count);
        } else {
            off += (size_t)snprintf(out + off,
                                    out_size - off,
                                    "%s flight:%s seats:%d\n",
                                    state->bookings[i].booking_id,
                                    state->bookings[i].flight_number,
                                    state->bookings[i].seat_count);
        }
        found = 1;
        if (off >= out_size - 1) {
            break;
        }
    }

    if (!found) {
        snprintf(out, out_size, "No bookings found for %s\n", user_id);
    }
    return 0;
}
