#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include "server_state.h"

int storage_init(ServerState *state);
int storage_flush_flights(ServerState *state);
int storage_flush_admins(ServerState *state);
int storage_flush_users(ServerState *state);
int storage_flush_bookings(ServerState *state);
int storage_flush_flight_bookings(ServerState *state, int flight_index);
int storage_delete_booking(int flight_index, int booking_index);
int storage_write_waitlist_entry(ServerState *state, int index);
int storage_delete_waitlist_entry(int index);
int storage_write_notification_entry(ServerState *state, int index);
int storage_delete_notification_entry(int index);
void storage_shutdown(ServerState *state);

#endif
