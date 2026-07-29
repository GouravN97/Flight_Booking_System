#include "booking_service.h"
#include "auth_service.h"
#include "flight_service.h"
#include "admin_ipc.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

static int find_booking_index_on_flight(Flight *flight, const char *booking_id) {
    for (int i = 0; i < MAX_BOOKINGS_PER_FLIGHT; i++) {
        if (flight->bookings[i].in_use &&
            strncmp(flight->bookings[i].booking_id, booking_id, sizeof(flight->bookings[i].booking_id)) == 0) {
            return i;
        }
    }
    return -1;
}

int find_booking_index(ServerState *state, const char *booking_id, int *flight_idx_out) {
    for (int fi = 0; fi < MAX_FLIGHTS; fi++) {
        if (!state->flights[fi].in_use) {
            continue;
        }
        int bi = find_booking_index_on_flight(&state->flights[fi], booking_id);
        if (bi >= 0) {
            if (flight_idx_out != NULL) {
                *flight_idx_out = fi;
            }
            return bi;
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

static void generate_booking_id(Flight *flight, char *out, size_t out_size) {
    for (int n = 1; n <= MAX_BOOKINGS_PER_FLIGHT; n++) {
        snprintf(out, out_size, "%s-BK%04d", flight->flight_number, n);
        if (find_booking_index_on_flight(flight, out) < 0) {
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
    int suggested_capacity = suggested_flight_capacity(requested);

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

    pthread_mutex_lock(&state->flights_lock);
    int fidx = find_matching_flight_with_capacity(state, source, destination, day, month, year, requested);
    pthread_mutex_unlock(&state->flights_lock);

    if (fidx < 0) {
        int auto_created = track_flight_request(state, source, destination, day, month, year, requested);

        if (auto_created == 1) {
            pthread_mutex_lock(&state->flights_lock);
            fidx = find_matching_flight_with_capacity(state, source, destination, day, month, year, requested);
            pthread_mutex_unlock(&state->flights_lock);
        }

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
    }

    Flight *f = &state->flights[fidx];

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
        return -5;
    }

    int allocated[MAX_SEATS_PER_FLIGHT];
    int allocated_count = 0;
    for (int i = 0; i < f->num_seats && allocated_count < requested; i++) {
        if (f->seats[i].booked) {
            continue;
        }
        pthread_mutex_lock(&f->seat_mutexes[i]);
        if (f->seats[i].booked) {
            pthread_mutex_unlock(&f->seat_mutexes[i]);
            continue;
        }
        f->seats[i].booked = true;
        snprintf(f->seats[i].passenger_id, sizeof(f->seats[i].passenger_id), "%s", user_id);
        allocated[allocated_count++] = f->seats[i].seat_number;
        pthread_mutex_unlock(&f->seat_mutexes[i]);
    }

    if (allocated_count != requested) {
        for (int i = 0; i < allocated_count; i++) {
            int seat_idx = allocated[i] - 1;
            if (seat_idx >= 0 && seat_idx < f->num_seats) {
                pthread_mutex_lock(&f->seat_mutexes[seat_idx]);
                f->seats[seat_idx].booked = false;
                f->seats[seat_idx].passenger_id[0] = '\0';
                pthread_mutex_unlock(&f->seat_mutexes[seat_idx]);
            }
        }
        for (int i = 0; i < acquired; i++) {
            sem_post(&f->seats_sem);
        }
        return -6;
    }

    int slot = -1;
    char booking_id[64];
    booking_id[0] = '\0';
    pthread_mutex_lock(&state->flights_lock);
    generate_booking_id(f, booking_id, sizeof(booking_id));
    if (booking_id[0] != '\0') {
        for (int i = 0; i < MAX_BOOKINGS_PER_FLIGHT; i++) {
            int idx = (f->next_booking_slot + i) % MAX_BOOKINGS_PER_FLIGHT;
            if (!f->bookings[idx].in_use) {
                slot = idx;
                f->next_booking_slot = (idx + 1) % MAX_BOOKINGS_PER_FLIGHT;
                break;
            }
        }
    }
    if (slot >= 0) {
        char flight_number[sizeof(f->flight_number)];
        snprintf(flight_number, sizeof(flight_number), "%s", f->flight_number);
        f->bookings[slot].in_use = 1;
        f->bookings[slot].seat_count = requested;
        snprintf(f->bookings[slot].booking_id, sizeof(f->bookings[slot].booking_id), "%s", booking_id);
        snprintf(f->bookings[slot].user_id, sizeof(f->bookings[slot].user_id), "%s", user_id);
        snprintf(f->bookings[slot].flight_number, sizeof(f->bookings[slot].flight_number), "%s", flight_number);
        for (int i = 0; i < requested; i++) {
            f->bookings[slot].seat_numbers[i] = allocated[i];
        }
    }
    pthread_mutex_unlock(&state->flights_lock);

    if (slot < 0) {
        for (int i = 0; i < allocated_count; i++) {
            int seat_idx = allocated[i] - 1;
            if (seat_idx >= 0 && seat_idx < f->num_seats) {
                pthread_mutex_lock(&f->seat_mutexes[seat_idx]);
                f->seats[seat_idx].booked = false;
                f->seats[seat_idx].passenger_id[0] = '\0';
                pthread_mutex_unlock(&f->seat_mutexes[seat_idx]);
            }
        }
        for (int i = 0; i < acquired; i++) {
            sem_post(&f->seats_sem);
        }
        return booking_id[0] == '\0' ? -3 : -7;
    }

    if (storage_flush_flights(state) != 0 || storage_flush_flight_bookings(state, fidx) != 0) {
        return -8;
    }
    snprintf(out_booking_id, out_booking_id_size, "%s", booking_id);
    return 0;
}

int cancel_booking_by_id(ServerState *state, const char *booking_id) {
    int fidx = -1;
    int bidx = find_booking_index(state, booking_id, &fidx);
    if (bidx < 0 || fidx < 0) {
        return -1;
    }

    BookingRecord booking;
    pthread_mutex_lock(&state->flights_lock);
    booking = state->flights[fidx].bookings[bidx];
    state->flights[fidx].bookings[bidx].in_use = 0;
    pthread_mutex_unlock(&state->flights_lock);

    Flight *f = &state->flights[fidx];
    for (int i = 0; i < booking.seat_count; i++) {
        int seat_idx = booking.seat_numbers[i] - 1;
        if (seat_idx >= 0 && seat_idx < f->num_seats) {
            pthread_mutex_lock(&f->seat_mutexes[seat_idx]);
            f->seats[seat_idx].booked = false;
            f->seats[seat_idx].passenger_id[0] = '\0';
            sem_post(&f->seats_sem);
            pthread_mutex_unlock(&f->seat_mutexes[seat_idx]);
        }
    }
    if (storage_flush_flights(state) != 0 || storage_delete_booking(fidx, bidx) != 0) {
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

    pthread_mutex_lock(&state->flights_lock);
    for (int fi = 0; fi < MAX_FLIGHTS; fi++) {
        if (!state->flights[fi].in_use) {
            continue;
        }
        Flight *flight = &state->flights[fi];
        for (int bi = 0; bi < MAX_BOOKINGS_PER_FLIGHT; bi++) {
            if (!flight->bookings[bi].in_use) {
                continue;
            }
            if (strncmp(flight->bookings[bi].user_id, user_id, sizeof(flight->bookings[bi].user_id)) != 0) {
                continue;
            }

            off += (size_t)snprintf(out + off,
                                    out_size - off,
                                    "%s flight:%s %s->%s date:%02d/%02d/%04d seats:%d\n",
                                    flight->bookings[bi].booking_id,
                                    flight->flight_number,
                                    flight->source,
                                    flight->destination,
                                    flight->day,
                                    flight->month,
                                    flight->year,
                                    flight->bookings[bi].seat_count);
            found = 1;
            if (off >= out_size - 1) {
                pthread_mutex_unlock(&state->flights_lock);
                return 0;
            }
        }
    }
    pthread_mutex_unlock(&state->flights_lock);

    if (!found) {
        snprintf(out, out_size, "No bookings found for %s\n", user_id);
    }
    return 0;
}
