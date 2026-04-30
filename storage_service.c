#include "storage_service.h"
#include "db_handler.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct seat_persist {
    int booked;
    char passenger_id[64];
} SeatPersist;

typedef struct flight_persist {
    int in_use;
    char flight_number[24];
    char source[64];
    char destination[64];
    int num_seats;
    int day;
    int month;
    int year;
    double price;
    SeatPersist seats[MAX_SEATS_PER_FLIGHT];
} FlightPersist;

typedef struct admin_persist {
    int in_use;
    char admin_id[64];
    char password[64];
    char message[256];
} AdminPersist;

static pthread_mutex_t g_db_api_lock = PTHREAD_MUTEX_INITIALIZER;

static int lock_db_file(const char *db_name, short lock_type, int *lock_fd_out) {
    int fd = open(db_name, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    struct flock lk;
    memset(&lk, 0, sizeof(lk));
    lk.l_type = lock_type;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0; // lock whole file

    if (fcntl(fd, F_SETLKW, &lk) != 0) {
        close(fd);
        return -1;
    }
    *lock_fd_out = fd;
    return 0;
}

static void unlock_db_file(int lock_fd) {
    struct flock lk;
    memset(&lk, 0, sizeof(lk));
    lk.l_type = F_UNLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    (void)fcntl(lock_fd, F_SETLK, &lk);
    close(lock_fd);
}

static int ensure_db(const char *db_name, int num_records, int record_size) {
    if (access(db_name, F_OK) == 0) {
        return 0;
    }

    if (create_db((char *)db_name, num_records, record_size) != 0) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);

    int lock_fd = -1;
    if (lock_db_file(db_name, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }

    if (open_db((char *)db_name) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }

    void *zero = calloc(1, (size_t)record_size);
    if (zero == NULL) {
        close_db((char *)db_name);
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }

    for (int i = 0; i < num_records; i++) {
        if (store_db((char *)db_name, i, zero) != 0) {
            free(zero);
            close_db((char *)db_name);
            unlock_db_file(lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
    }
    free(zero);
    int rc = close_db((char *)db_name);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

static void init_flight_runtime(Flight *flight) {
    int available = 0;
    pthread_mutex_init(&flight->flight_mutex, NULL);
    for (int i = 0; i < flight->num_seats; i++) {
        flight->seats[i].seat_number = i + 1;
        pthread_mutex_init(&flight->seats[i].seat_mutex, NULL);
        if (!flight->seats[i].booked) {
            available++;
        }
    }
    sem_init(&flight->seats_sem, 0, (unsigned int)available);
}

int storage_flush_flights(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(FLIGHTS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(FLIGHTS_DB_FILE) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        FlightPersist p;
        memset(&p, 0, sizeof(p));
        p.in_use = state->flights[i].in_use;
        snprintf(p.flight_number, sizeof(p.flight_number), "%s", state->flights[i].flight_number);
        snprintf(p.source, sizeof(p.source), "%s", state->flights[i].source);
        snprintf(p.destination, sizeof(p.destination), "%s", state->flights[i].destination);
        p.num_seats = state->flights[i].num_seats;
        p.day = state->flights[i].day;
        p.month = state->flights[i].month;
        p.year = state->flights[i].year;
        p.price = state->flights[i].price;
        for (int s = 0; s < MAX_SEATS_PER_FLIGHT; s++) {
            p.seats[s].booked = state->flights[i].seats[s].booked ? 1 : 0;
            snprintf(p.seats[s].passenger_id, sizeof(p.seats[s].passenger_id), "%s", state->flights[i].seats[s].passenger_id);
        }
        if (update_db(FLIGHTS_DB_FILE, i, &p) != 0) {
            close_db(FLIGHTS_DB_FILE);
            unlock_db_file(lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
    }
    int rc = close_db(FLIGHTS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_flush_admins(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(ADMINS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(ADMINS_DB_FILE) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_ADMINS; i++) {
        AdminPersist p;
        memset(&p, 0, sizeof(p));
        p.in_use = state->admins[i].in_use;
        snprintf(p.admin_id, sizeof(p.admin_id), "%s", state->admins[i].admin_id);
        snprintf(p.password, sizeof(p.password), "%s", state->admins[i].password);
        snprintf(p.message, sizeof(p.message), "%s", state->admins[i].message);
        if (update_db(ADMINS_DB_FILE, i, &p) != 0) {
            close_db(ADMINS_DB_FILE);
            unlock_db_file(lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
    }
    int rc = close_db(ADMINS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_flush_users(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(USERS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(USERS_DB_FILE) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_USERS; i++) {
        if (update_db(USERS_DB_FILE, i, &state->users[i]) != 0) {
            close_db(USERS_DB_FILE);
            unlock_db_file(lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
    }
    int rc = close_db(USERS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_flush_bookings(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_BOOKINGS; i++) {
        if (update_db(BOOKINGS_DB_FILE, i, &state->bookings[i]) != 0) {
            close_db(BOOKINGS_DB_FILE);
            unlock_db_file(lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
    }
    int rc = close_db(BOOKINGS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_delete_booking_by_index(int booking_index) {
    if (booking_index < 0 || booking_index >= MAX_BOOKINGS) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (delete_db(BOOKINGS_DB_FILE, booking_index) != 0) {
        close_db(BOOKINGS_DB_FILE);
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = close_db(BOOKINGS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_init(ServerState *state) {
    if (ensure_db(FLIGHTS_DB_FILE, MAX_FLIGHTS, (int)sizeof(FlightPersist)) != 0 ||
        ensure_db(ADMINS_DB_FILE, MAX_ADMINS, (int)sizeof(AdminPersist)) != 0 ||
        ensure_db(USERS_DB_FILE, MAX_USERS, (int)sizeof(UserEntry)) != 0 ||
        ensure_db(BOOKINGS_DB_FILE, MAX_BOOKINGS, (int)sizeof(BookingRecord)) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_db_api_lock);
    int flights_lock_fd = -1;
    if (lock_db_file(FLIGHTS_DB_FILE, F_RDLCK, &flights_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(FLIGHTS_DB_FILE) != 0) {
        unlock_db_file(flights_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        FlightPersist *p = (FlightPersist *)read_db(FLIGHTS_DB_FILE, i);
        if (p == NULL) {
            close_db(FLIGHTS_DB_FILE);
            unlock_db_file(flights_lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
        if (p->in_use) {
            state->flights[i].in_use = 1;
            snprintf(state->flights[i].flight_number, sizeof(state->flights[i].flight_number), "%s", p->flight_number);
            snprintf(state->flights[i].source, sizeof(state->flights[i].source), "%s", p->source);
            snprintf(state->flights[i].destination, sizeof(state->flights[i].destination), "%s", p->destination);
            state->flights[i].num_seats = p->num_seats;
            state->flights[i].day = p->day;
            state->flights[i].month = p->month;
            state->flights[i].year = p->year;
            state->flights[i].price = p->price;
            for (int s = 0; s < MAX_SEATS_PER_FLIGHT; s++) {
                state->flights[i].seats[s].booked = p->seats[s].booked != 0;
                snprintf(state->flights[i].seats[s].passenger_id,
                         sizeof(state->flights[i].seats[s].passenger_id),
                         "%s",
                         p->seats[s].passenger_id);
            }
            init_flight_runtime(&state->flights[i]);
        }
        free(p);
    }
    if (close_db(FLIGHTS_DB_FILE) != 0) {
        unlock_db_file(flights_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    unlock_db_file(flights_lock_fd);

    int admins_lock_fd = -1;
    if (lock_db_file(ADMINS_DB_FILE, F_RDLCK, &admins_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(ADMINS_DB_FILE) != 0) {
        unlock_db_file(admins_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_ADMINS; i++) {
        AdminPersist *p = (AdminPersist *)read_db(ADMINS_DB_FILE, i);
        if (p == NULL) {
            close_db(ADMINS_DB_FILE);
            unlock_db_file(admins_lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
        state->admins[i].in_use = p->in_use;
        snprintf(state->admins[i].admin_id, sizeof(state->admins[i].admin_id), "%s", p->admin_id);
        snprintf(state->admins[i].password, sizeof(state->admins[i].password), "%s", p->password);
        snprintf(state->admins[i].message, sizeof(state->admins[i].message), "%s", p->message);
        free(p);
    }
    if (close_db(ADMINS_DB_FILE) != 0) {
        unlock_db_file(admins_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    unlock_db_file(admins_lock_fd);

    int users_lock_fd = -1;
    if (lock_db_file(USERS_DB_FILE, F_RDLCK, &users_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(USERS_DB_FILE) != 0) {
        unlock_db_file(users_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_USERS; i++) {
        UserEntry *u = (UserEntry *)read_db(USERS_DB_FILE, i);
        if (u == NULL) {
            close_db(USERS_DB_FILE);
            unlock_db_file(users_lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
        state->users[i] = *u;
        free(u);
    }
    if (close_db(USERS_DB_FILE) != 0) {
        unlock_db_file(users_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    unlock_db_file(users_lock_fd);

    int bookings_lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_RDLCK, &bookings_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE) != 0) {
        unlock_db_file(bookings_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_BOOKINGS; i++) {
        BookingRecord *b = (BookingRecord *)read_db(BOOKINGS_DB_FILE, i);
        if (b == NULL) {
            close_db(BOOKINGS_DB_FILE);
            unlock_db_file(bookings_lock_fd);
            pthread_mutex_unlock(&g_db_api_lock);
            return -1;
        }
        state->bookings[i] = *b;
        free(b);
    }
    int rc = close_db(BOOKINGS_DB_FILE);
    unlock_db_file(bookings_lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

void storage_shutdown(ServerState *state) {
    storage_flush_flights(state);
    storage_flush_admins(state);
    storage_flush_users(state);
    storage_flush_bookings(state);
}
