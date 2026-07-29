#ifndef SERVER_STATE_H
#define SERVER_STATE_H

#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>

#define MAX_FLIGHTS 1000
#define MAX_SEATS_PER_FLIGHT 25
#define MAX_USERS 10000
#define MAX_BOOKINGS_PER_FLIGHT 1000
#define MAX_ADMINS 128
#define ADMIN_SHM_NAME "/flight_admin_bus" //not needed 
#define ADMIN_SHM_SIZE 4096
#define FLIGHTS_DB_FILE "flights.db"
#define ADMINS_DB_FILE "admins.db"
#define USERS_DB_FILE "users.db"
#define BOOKINGS_DB_FILE "bookings.db"
#define AUTO_CREATE_FLIGHT_THRESHOLD 20
#define MAX_FLIGHT_REQUESTS 10000

typedef struct seat {
    int seat_number;
    bool booked;
    char passenger_id[64];
} Seat;

typedef struct booking_record {
    int in_use;
    char booking_id[64];
    char user_id[64];
    char flight_number[24];
    int seat_count;
    int seat_numbers[MAX_SEATS_PER_FLIGHT];
} BookingRecord;

typedef struct flight {
    int in_use;
    char flight_number[24];
    char source[64];
    char destination[64];
    int num_seats; 
    int day; int month; int year; //date of the flight
    double price;
    Seat seats[MAX_SEATS_PER_FLIGHT];
    sem_t seats_sem;
    pthread_mutex_t seat_mutexes[MAX_SEATS_PER_FLIGHT];
    BookingRecord bookings[MAX_BOOKINGS_PER_FLIGHT];
    int next_booking_slot;
} Flight;

typedef struct user_entry {
    int in_use;
    char id[64];
    char password[64];
} UserEntry;

typedef struct admin_entry {
    int in_use;
    char admin_id[64];
    char password[64];
    char message[256];
} AdminEntry;

typedef struct flight_request {
    int in_use;
    char source[64];
    char destination[64];
    int day;
    int month;
    int year;
    int request_count;
    int auto_created;
} FlightRequest;

typedef struct server_state {
    pthread_mutex_t state_lock;
    pthread_mutex_t flights_lock;
    pthread_mutex_t requests_lock;
    Flight flights[MAX_FLIGHTS];
    AdminEntry admins[MAX_ADMINS];
    UserEntry users[MAX_USERS];
    FlightRequest flight_requests[MAX_FLIGHT_REQUESTS];
    int next_flight_slot;
    int next_request_slot;

    int shm_fd;
    char *admin_bus;
} ServerState;

int server_init(ServerState *state);
void server_shutdown(ServerState *state);

#endif
