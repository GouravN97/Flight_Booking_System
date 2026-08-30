#ifndef WAITLIST_SERVICE_H
#define WAITLIST_SERVICE_H

#include "server_state.h"

int enqueue_waitlist(ServerState *state,
                     const char *user_id,
                     const char *source,
                     const char *destination,
                     int day,
                     int month,
                     int year,
                     int seat_count);

int fulfill_waitlist_for_flight(ServerState *state, int flight_idx);

int format_and_consume_notifications(ServerState *state,
                                     const char *user_id,
                                     char *out,
                                     size_t out_size);

#endif
