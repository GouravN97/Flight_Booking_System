#include "flight_service.h"
#include "storage_service.h"
#include <string.h>

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
        if (!state->flights[i].in_use) {
            slot = i;
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

    pthread_mutex_init(&flight->flight_mutex, NULL);
    sem_init(&flight->seats_sem, 0, (unsigned int)num_seats);

    for (int i = 0; i < num_seats; i++) {
        flight->seats[i].seat_number = i + 1;
        flight->seats[i].booked = false;
        flight->seats[i].passenger_id[0] = '\0';
        pthread_mutex_init(&flight->seats[i].seat_mutex, NULL);
    }
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
