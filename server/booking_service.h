#ifndef BOOKING_SERVICE_H
#define BOOKING_SERVICE_H

#include "server_state.h"

#define BOOKING_ADMIN_REQUESTED 1
#define BOOKING_WAITLISTED 2

int book_seats_on_flight(ServerState *state,
                         int flight_idx,
                         const char *user_id,
                         int requested,
                         char *out_booking_id,
                         size_t out_booking_id_size,
                         int flights_lock_held,
                         int persist);

int find_booking_index(ServerState *state, const char *booking_id, int *flight_idx_out);
int book_seats_for_user(ServerState *state,
                        const char *user_id,
                        const char *source,
                        const char *destination,
                        int day,
                        int month,
                        int year,
                        int requested,
                        char *out_booking_id,
                        size_t out_booking_id_size);
int cancel_booking_by_id(ServerState *state, const char *booking_id);
int format_user_bookings(ServerState *state, const char *user_id, char *out, size_t out_size);

#endif
