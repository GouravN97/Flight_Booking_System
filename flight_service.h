#ifndef FLIGHT_SERVICE_H
#define FLIGHT_SERVICE_H

#include "server_state.h"

int find_flight_index(ServerState *state, const char *flight_number);

int create_flight(ServerState *state,
                  const char *flight_number,
                  const char *source,
                  const char *destination,
                  int day,
                  int month,
                  int year,
                  int num_seats,
                  double price);

int change_flight_timings(ServerState *state,
                          const char *flight_number,
                          int day,
                          int month,
                          int year);

int update_price(ServerState *state, const char *flight_number, double new_price);

#endif
