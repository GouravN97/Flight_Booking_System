#include "flight_service.h"
#include "admin_ipc.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

int suggested_flight_capacity(int requested_seats) {
    int capacity = requested_seats + 10;
    if (capacity > MAX_SEATS_PER_FLIGHT) {
        capacity = MAX_SEATS_PER_FLIGHT;
    }
    if (capacity < 1) {
        capacity = 1;
    }
    return capacity;
}

int route_date_has_flight(ServerState *state,
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

void init_flight_seat_mutexes(Flight *flight) {
    for (int i = 0; i < flight->num_seats; i++) {
        pthread_mutex_init(&flight->seat_mutexes[i], NULL);
    }
}

int find_flight_index(ServerState *state, const char *flight_number) {
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        if (state->flights[i].in_use &&
            strncmp(state->flights[i].flight_number, flight_number, sizeof(state->flights[i].flight_number)) == 0) {
            return i;
        }
    }
    return -1;
}

int create_flight(ServerState *state,
                  const char *flight_number,
                  const char *source,
                  const char *destination,
                  int day,
                  int month,
                  int year,
                  int num_seats,
                  double price) {
    if (num_seats <= 0 || num_seats > MAX_SEATS_PER_FLIGHT) {
        return -1;
    }

    if (find_flight_index(state, flight_number) >= 0) {
        return -1;
    }

    int slot = -1;
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        int idx = (state->next_flight_slot + i) % MAX_FLIGHTS;
        if (!state->flights[idx].in_use) {
            slot = idx;
            state->next_flight_slot = (idx + 1) % MAX_FLIGHTS;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    Flight *flight = &state->flights[slot];
    memset(flight, 0, sizeof(*flight));
    flight->in_use = 1;
    strncpy(flight->flight_number, flight_number, sizeof(flight->flight_number) - 1);
    strncpy(flight->source, source, sizeof(flight->source) - 1);
    strncpy(flight->destination, destination, sizeof(flight->destination) - 1);
    flight->day = day;
    flight->month = month;
    flight->year = year;
    flight->num_seats = num_seats;
    flight->price = price;

    sem_init(&flight->seats_sem, 0, (unsigned int)num_seats);

    for (int i = 0; i < num_seats; i++) {
        flight->seats[i].seat_number = i + 1;
        flight->seats[i].booked = false;
        flight->seats[i].passenger_id[0] = '\0';
    }
    init_flight_seat_mutexes(flight);
    return storage_flush_flights(state);
}

int change_flight_timings(ServerState *state,
                          const char *flight_number,
                          int day,
                          int month,
                          int year) {
    int idx = find_flight_index(state, flight_number);
    if (idx < 0) {
        return -1;
    }
    state->flights[idx].day = day;
    state->flights[idx].month = month;
    state->flights[idx].year = year;
    return storage_flush_flights(state);
}

int update_price(ServerState *state, const char *flight_number, double new_price) {
    int idx = find_flight_index(state, flight_number);
    if (idx < 0) {
        return -1;
    }
    state->flights[idx].price = new_price;
    return storage_flush_flights(state);
}
int track_flight_request(ServerState *state,
                         const char *source,
                         const char *destination,
                         int day,
                         int month,
                         int year,
                         int requested_seats) {
    pthread_mutex_lock(&state->requests_lock);
    
    // Find existing request or create new one
    int req_idx = -1;
    for (int i = 0; i < MAX_FLIGHT_REQUESTS; i++) {
        if (!state->flight_requests[i].in_use) {
            continue;
        }
        if (strncmp(state->flight_requests[i].source, source, sizeof(state->flight_requests[i].source)) == 0 &&
            strncmp(state->flight_requests[i].destination, destination, sizeof(state->flight_requests[i].destination)) == 0 &&
            state->flight_requests[i].day == day &&
            state->flight_requests[i].month == month &&
            state->flight_requests[i].year == year) {
            req_idx = i;
            break;
        }
    }
    
    if (req_idx < 0) {
        // Create new request entry
        for (int i = 0; i < MAX_FLIGHT_REQUESTS; i++) {
            int idx = (state->next_request_slot + i) % MAX_FLIGHT_REQUESTS;
            if (!state->flight_requests[idx].in_use) {
                req_idx = idx;
                state->next_request_slot = (idx + 1) % MAX_FLIGHT_REQUESTS;
                state->flight_requests[idx].in_use = 1;
                strncpy(state->flight_requests[idx].source, source, sizeof(state->flight_requests[idx].source) - 1);
                strncpy(state->flight_requests[idx].destination, destination, sizeof(state->flight_requests[idx].destination) - 1);
                state->flight_requests[idx].day = day;
                state->flight_requests[idx].month = month;
                state->flight_requests[idx].year = year;
                state->flight_requests[idx].request_count = 0;
                state->flight_requests[idx].auto_created = 0;
                break;
            }
        }
    }
    
    if (req_idx < 0) {
        pthread_mutex_unlock(&state->requests_lock);
        return -1;  // No space for new request
    }
    
    // Increment request count
    state->flight_requests[req_idx].request_count++;
    int request_count = state->flight_requests[req_idx].request_count;
    int already_auto_created = state->flight_requests[req_idx].auto_created;
    
    pthread_mutex_unlock(&state->requests_lock);
    
    if (request_count >= AUTO_CREATE_FLIGHT_THRESHOLD && !already_auto_created) {
        pthread_mutex_lock(&state->flights_lock);
        int flight_exists = route_date_has_flight(state, source, destination, day, month, year);
        pthread_mutex_unlock(&state->flights_lock);

        if (flight_exists) {
            return 0;
        }

        char flight_number[24];
        snprintf(flight_number, sizeof(flight_number), "AUTO%04d%02d%02d", year, month, day);

        int capacity = suggested_flight_capacity(requested_seats);

        pthread_mutex_lock(&state->flights_lock);
        int result = create_flight(state, flight_number, source, destination,
                                   day, month, year, capacity, 0.0);
        pthread_mutex_unlock(&state->flights_lock);

        if (result == 0) {
            pthread_mutex_lock(&state->requests_lock);
            state->flight_requests[req_idx].auto_created = 1;
            pthread_mutex_unlock(&state->requests_lock);

            char admin_message[512];
            snprintf(admin_message,
                     sizeof(admin_message),
                     "Auto-created flight %s for %s->%s on %02d/%02d/%04d (capacity:%d). Pending users may now book.",
                     flight_number,
                     source,
                     destination,
                     day,
                     month,
                     year,
                     capacity);
            publish_admin_request(state, admin_message);
            return 1;
        }
    }
    
    return 0;  // No auto-creation, just tracking
}