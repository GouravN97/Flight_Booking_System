#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include "server_state.h"

int storage_init(ServerState *state);
int storage_flush_flights(ServerState *state);
int storage_flush_admins(ServerState *state);
int storage_flush_users(ServerState *state);
int storage_flush_bookings(ServerState *state);
int storage_delete_booking_by_index(int booking_index);
void storage_shutdown(ServerState *state);

#endif
