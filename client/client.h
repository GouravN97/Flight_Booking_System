#ifndef CLIENT_H
#define CLIENT_H

#include <stdio.h>

#define USER_DB "users.dat"
#define FLIGHT_DB "flights.dat"
#define BOOKING_DB "bookings.dat"
#define BOOK_SEATS_ADMIN_REQUESTED 1

int login(char * id, char * password);
int signup(char * id, char * password);
int view_available_flights(char* source, char * destination);
int book_seats(char *user_id,
               char *source,
               char *destination,
               int day,
               int month,
               int year,
               int num_seats);
int cancel_booking(char *booking_id);
int view_flight_details(char *flight_number);
int view_my_bookings(char *user_id);
int send_admin_message(char *message);
int read_admin_shared_message(void);
int admin_login(char *admin_id, char *password);
int admin_create_user(char *actor_admin_id, char *actor_password, char *new_admin_id, char *new_admin_password);
int admin_create_flight(char *flight_number,
                        char *source,
                        char *destination,
                        int day,
                        int month,
                        int year,
                        int num_seats,
                        double price,
                        char *actor_admin_id,
                        char *actor_password);
int admin_change_flight_timing(char *flight_number, int day, int month, int year, char *actor_admin_id, char *actor_password);
int admin_update_flight_price(char *flight_number, double price, char *actor_admin_id, char *actor_password);
int admin_list_all_flights(char *actor_admin_id, char *actor_password);
int admin_list_all_admins(char *actor_admin_id, char *actor_password);

int client_set_server(const char *host, int port);
int client_ping_server(void);

#endif
