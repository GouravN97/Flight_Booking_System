#include "storage_service.h"
#include "db_handler.h"
#include "flight_service.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
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
    char password[PASSWORD_HASH_SIZE];
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
    return create_db((char *)db_name, num_records, record_size);
}

static void init_flight_runtime(Flight *flight);

#define LEGACY_MAX_BOOKINGS 100000

typedef struct {
    ServerState *state;
    int legacy_records_migrated;
} StorageLoadCtx;

static int load_flight_record(uint32_t key, void *record, void *ctx) {
    ServerState *state = ((StorageLoadCtx *)ctx)->state;
    if (key >= (uint32_t)MAX_FLIGHTS) {
        return 0;
    }

    FlightPersist *p = (FlightPersist *)record;
    if (!p->in_use) {
        return 0;
    }

    int i = (int)key;
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
    return 0;
}

static int load_admin_record(uint32_t key, void *record, void *ctx) {
    ServerState *state = ((StorageLoadCtx *)ctx)->state;
    if (key >= (uint32_t)MAX_ADMINS) {
        return 0;
    }

    AdminPersist *p = (AdminPersist *)record;
    state->admins[(int)key].in_use = p->in_use;
    snprintf(state->admins[(int)key].admin_id, sizeof(state->admins[(int)key].admin_id), "%s", p->admin_id);
    snprintf(state->admins[(int)key].password, sizeof(state->admins[(int)key].password), "%s", p->password);
    snprintf(state->admins[(int)key].message, sizeof(state->admins[(int)key].message), "%s", p->message);
    return 0;
}

static int load_user_record(uint32_t key, void *record, void *ctx) {
    ServerState *state = ((StorageLoadCtx *)ctx)->state;
    if (key >= (uint32_t)MAX_USERS) {
        return 0;
    }

    state->users[(int)key] = *(UserEntry *)record;
    return 0;
}

static int booking_db_key(int flight_index, int booking_index) {
    return flight_index * MAX_BOOKINGS_PER_FLIGHT + booking_index;
}

static int alloc_booking_slot_on_flight(Flight *flight) {
    for (int i = 0; i < MAX_BOOKINGS_PER_FLIGHT; i++) {
        int idx = (flight->next_booking_slot + i) % MAX_BOOKINGS_PER_FLIGHT;
        if (!flight->bookings[idx].in_use) {
            flight->next_booking_slot = (idx + 1) % MAX_BOOKINGS_PER_FLIGHT;
            return idx;
        }
    }
    return -1;
}

static void normalize_legacy_booking_id(BookingRecord *booking, const Flight *flight) {
    if (booking->booking_id[0] == '\0') {
        return;
    }
    const char *dash = strchr(booking->booking_id, '-');
    if (dash != NULL && dash[1] == 'B' && dash[2] == 'K') {
        return;
    }
    if (strncmp(booking->booking_id, "BK", 2) == 0) {
        int seq = 0;
        if (sscanf(booking->booking_id + 2, "%d", &seq) == 1 && seq > 0) {
            snprintf(booking->booking_id,
                     sizeof(booking->booking_id),
                     "%s-BK%04d",
                     flight->flight_number,
                     seq);
        } else {
            snprintf(booking->booking_id,
                     sizeof(booking->booking_id),
                     "%s-%s",
                     flight->flight_number,
                     booking->booking_id);
        }
    }
}

static int is_legacy_flat_booking_key(uint32_t key, int flight_idx) {
    if (key >= (uint32_t)LEGACY_MAX_BOOKINGS) {
        return 0;
    }
    return (int)(key / MAX_BOOKINGS_PER_FLIGHT) != flight_idx;
}

static int recreate_bookings_db_from_state(ServerState *state) {
    if (unlink(BOOKINGS_DB_FILE) != 0 && errno != ENOENT) {
        return -1;
    }
    if (create_db((char *)BOOKINGS_DB_FILE,
                  MAX_FLIGHTS * MAX_BOOKINGS_PER_FLIGHT,
                  (int)sizeof(BookingRecord)) != 0) {
        return -1;
    }
    return storage_flush_bookings(state);
}

static int load_booking_record(uint32_t key, void *record, void *ctx) {
    StorageLoadCtx *load_ctx = (StorageLoadCtx *)ctx;
    ServerState *state = load_ctx->state;
    if (key >= (uint32_t)(MAX_FLIGHTS * MAX_BOOKINGS_PER_FLIGHT)) {
        return 0;
    }

    BookingRecord *booking = (BookingRecord *)record;
    if (!booking->in_use) {
        return 0;
    }

    int flight_idx = find_flight_index(state, booking->flight_number);
    if (flight_idx < 0) {
        return 0;
    }

    Flight *flight = &state->flights[flight_idx];
    int decoded_flight = (int)(key / MAX_BOOKINGS_PER_FLIGHT);
    int decoded_slot = (int)(key % MAX_BOOKINGS_PER_FLIGHT);

    if (is_legacy_flat_booking_key(key, flight_idx)) {
        int slot = alloc_booking_slot_on_flight(flight);
        if (slot < 0) {
            return 0;
        }
        flight->bookings[slot] = *booking;
        normalize_legacy_booking_id(&flight->bookings[slot], flight);
        load_ctx->legacy_records_migrated = 1;
        return 0;
    }

    if (decoded_flight < 0 || decoded_flight >= MAX_FLIGHTS || decoded_slot < 0 ||
        decoded_slot >= MAX_BOOKINGS_PER_FLIGHT) {
        return 0;
    }

    if (state->flights[decoded_flight].bookings[decoded_slot].in_use) {
        int slot = alloc_booking_slot_on_flight(flight);
        if (slot < 0) {
            return 0;
        }
        flight->bookings[slot] = *booking;
        load_ctx->legacy_records_migrated = 1;
        return 0;
    }

    state->flights[decoded_flight].bookings[decoded_slot] = *booking;
    int next = (decoded_slot + 1) % MAX_BOOKINGS_PER_FLIGHT;
    if (state->flights[decoded_flight].next_booking_slot == decoded_slot) {
        state->flights[decoded_flight].next_booking_slot = next;
    }
    return 0;
}

static void init_flight_runtime(Flight *flight) {
    int available = 0;
    for (int i = 0; i < flight->num_seats; i++) {
        flight->seats[i].seat_number = i + 1;
        if (!flight->seats[i].booked) {
            available++;
        }
    }
    init_flight_seat_mutexes(flight);
    sem_init(&flight->seats_sem, 0, (unsigned int)available);
}

int storage_flush_flights(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(FLIGHTS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(FLIGHTS_DB_FILE, (int)sizeof(FlightPersist)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_FLIGHTS; i++) {
        if (!state->flights[i].in_use) {
            continue;
        }
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
    if (open_db(ADMINS_DB_FILE, (int)sizeof(AdminPersist)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_ADMINS; i++) {
        if (!state->admins[i].in_use) {
            continue;
        }
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
    if (open_db(USERS_DB_FILE, (int)sizeof(UserEntry)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int i = 0; i < MAX_USERS; i++) {
        if (!state->users[i].in_use) {
            continue;
        }
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

int storage_flush_flight_bookings(ServerState *state, int flight_index) {
    if (flight_index < 0 || flight_index >= MAX_FLIGHTS || !state->flights[flight_index].in_use) {
        return -1;
    }

    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE, (int)sizeof(BookingRecord)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }

    Flight *flight = &state->flights[flight_index];
    for (int bi = 0; bi < MAX_BOOKINGS_PER_FLIGHT; bi++) {
        if (!flight->bookings[bi].in_use) {
            continue;
        }
        int key = booking_db_key(flight_index, bi);
        if (update_db(BOOKINGS_DB_FILE, key, &flight->bookings[bi]) != 0) {
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

int storage_flush_bookings(ServerState *state) {
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE, (int)sizeof(BookingRecord)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    for (int fi = 0; fi < MAX_FLIGHTS; fi++) {
        if (!state->flights[fi].in_use) {
            continue;
        }
        for (int bi = 0; bi < MAX_BOOKINGS_PER_FLIGHT; bi++) {
            if (!state->flights[fi].bookings[bi].in_use) {
                continue;
            }
            int key = booking_db_key(fi, bi);
            if (update_db(BOOKINGS_DB_FILE, key, &state->flights[fi].bookings[bi]) != 0) {
                close_db(BOOKINGS_DB_FILE);
                unlock_db_file(lock_fd);
                pthread_mutex_unlock(&g_db_api_lock);
                return -1;
            }
        }
    }
    int rc = close_db(BOOKINGS_DB_FILE);
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_delete_booking(int flight_index, int booking_index) {
    if (flight_index < 0 || flight_index >= MAX_FLIGHTS || booking_index < 0 ||
        booking_index >= MAX_BOOKINGS_PER_FLIGHT) {
        return -1;
    }
    int key = booking_db_key(flight_index, booking_index);
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (lock_db_file(BOOKINGS_DB_FILE, F_WRLCK, &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(BOOKINGS_DB_FILE, (int)sizeof(BookingRecord)) != 0) {
        unlock_db_file(lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (delete_db(BOOKINGS_DB_FILE, key) != 0) {
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

static int open_locked_db(const char *db_name, int record_size, int *lock_fd_out) {
    if (lock_db_file(db_name, F_WRLCK, lock_fd_out) != 0) {
        return -1;
    }
    if (open_db((char *)db_name, record_size) != 0) {
        unlock_db_file(*lock_fd_out);
        return -1;
    }
    return 0;
}

int storage_write_waitlist_entry(ServerState *state, int index) {
    if (state == NULL || index < 0 || index >= MAX_WAITLIST) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (open_locked_db(WAITLIST_DB_FILE, (int)sizeof(WaitlistEntry), &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = update_db((char *)WAITLIST_DB_FILE, index, &state->waitlist[index]);
    if (close_db((char *)WAITLIST_DB_FILE) != 0) {
        rc = -1;
    }
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_delete_waitlist_entry(int index) {
    if (index < 0 || index >= MAX_WAITLIST) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (open_locked_db(WAITLIST_DB_FILE, (int)sizeof(WaitlistEntry), &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = delete_db((char *)WAITLIST_DB_FILE, index);
    if (close_db((char *)WAITLIST_DB_FILE) != 0) {
        rc = -1;
    }
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_write_notification_entry(ServerState *state, int index) {
    if (state == NULL || index < 0 || index >= MAX_NOTIFICATIONS) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (open_locked_db(NOTIFICATIONS_DB_FILE, (int)sizeof(UserNotification), &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = update_db((char *)NOTIFICATIONS_DB_FILE, index, &state->notifications[index]);
    if (close_db((char *)NOTIFICATIONS_DB_FILE) != 0) {
        rc = -1;
    }
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

int storage_delete_notification_entry(int index) {
    if (index < 0 || index >= MAX_NOTIFICATIONS) {
        return -1;
    }
    pthread_mutex_lock(&g_db_api_lock);
    int lock_fd = -1;
    if (open_locked_db(NOTIFICATIONS_DB_FILE, (int)sizeof(UserNotification), &lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = delete_db((char *)NOTIFICATIONS_DB_FILE, index);
    if (close_db((char *)NOTIFICATIONS_DB_FILE) != 0) {
        rc = -1;
    }
    unlock_db_file(lock_fd);
    pthread_mutex_unlock(&g_db_api_lock);
    return rc;
}

static int load_waitlist_record(uint32_t key, void *record, void *ctx) {
    ServerState *state = ((StorageLoadCtx *)ctx)->state;
    if (key >= (uint32_t)MAX_WAITLIST) {
        return 0;
    }
    WaitlistEntry *entry = (WaitlistEntry *)record;
    if (!entry->in_use) {
        return 0;
    }
    state->waitlist[(int)key] = *entry;
    int next = ((int)key + 1) % MAX_WAITLIST;
    if (state->next_waitlist_slot == (int)key) {
        state->next_waitlist_slot = next;
    }
    if (entry->seq + 1 > state->next_waitlist_seq) {
        state->next_waitlist_seq = entry->seq + 1;
    }
    return 0;
}

static int load_notification_record(uint32_t key, void *record, void *ctx) {
    ServerState *state = ((StorageLoadCtx *)ctx)->state;
    if (key >= (uint32_t)MAX_NOTIFICATIONS) {
        return 0;
    }
    UserNotification *entry = (UserNotification *)record;
    if (!entry->in_use) {
        return 0;
    }
    state->notifications[(int)key] = *entry;
    return 0;
}

int storage_init(ServerState *state) {
    if (ensure_db(FLIGHTS_DB_FILE, MAX_FLIGHTS, (int)sizeof(FlightPersist)) != 0 ||
        ensure_db(ADMINS_DB_FILE, MAX_ADMINS, (int)sizeof(AdminPersist)) != 0 ||
        ensure_db(USERS_DB_FILE, MAX_USERS, (int)sizeof(UserEntry)) != 0 ||
        ensure_db(BOOKINGS_DB_FILE, MAX_FLIGHTS * MAX_BOOKINGS_PER_FLIGHT, (int)sizeof(BookingRecord)) != 0 ||
        ensure_db(WAITLIST_DB_FILE, MAX_WAITLIST, (int)sizeof(WaitlistEntry)) != 0 ||
        ensure_db(NOTIFICATIONS_DB_FILE, MAX_NOTIFICATIONS, (int)sizeof(UserNotification)) != 0) {
        return -1;
    }

    pthread_mutex_lock(&g_db_api_lock);
    int flights_lock_fd = -1;
    if (lock_db_file(FLIGHTS_DB_FILE, F_RDLCK, &flights_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(FLIGHTS_DB_FILE, (int)sizeof(FlightPersist)) != 0) {
        unlock_db_file(flights_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    StorageLoadCtx load_ctx = {.state = state, .legacy_records_migrated = 0};
    if (foreach_record(load_flight_record, &load_ctx) != 0) {
        close_db(FLIGHTS_DB_FILE);
        unlock_db_file(flights_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
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
    if (open_db(ADMINS_DB_FILE, (int)sizeof(AdminPersist)) != 0) {
        unlock_db_file(admins_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (foreach_record(load_admin_record, &load_ctx) != 0) {
        close_db(ADMINS_DB_FILE);
        unlock_db_file(admins_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
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
    if (open_db(USERS_DB_FILE, (int)sizeof(UserEntry)) != 0) {
        unlock_db_file(users_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (foreach_record(load_user_record, &load_ctx) != 0) {
        close_db(USERS_DB_FILE);
        unlock_db_file(users_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
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
    if (open_db(BOOKINGS_DB_FILE, (int)sizeof(BookingRecord)) != 0) {
        unlock_db_file(bookings_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (foreach_record(load_booking_record, &load_ctx) != 0) {
        close_db(BOOKINGS_DB_FILE);
        unlock_db_file(bookings_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    int rc = close_db(BOOKINGS_DB_FILE);
    unlock_db_file(bookings_lock_fd);
    int migrate_bookings = load_ctx.legacy_records_migrated;

    int waitlist_lock_fd = -1;
    if (lock_db_file(WAITLIST_DB_FILE, F_RDLCK, &waitlist_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(WAITLIST_DB_FILE, (int)sizeof(WaitlistEntry)) != 0) {
        unlock_db_file(waitlist_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (foreach_record(load_waitlist_record, &load_ctx) != 0) {
        close_db(WAITLIST_DB_FILE);
        unlock_db_file(waitlist_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (close_db(WAITLIST_DB_FILE) != 0) {
        unlock_db_file(waitlist_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    unlock_db_file(waitlist_lock_fd);

    int notifications_lock_fd = -1;
    if (lock_db_file(NOTIFICATIONS_DB_FILE, F_RDLCK, &notifications_lock_fd) != 0) {
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (open_db(NOTIFICATIONS_DB_FILE, (int)sizeof(UserNotification)) != 0) {
        unlock_db_file(notifications_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (foreach_record(load_notification_record, &load_ctx) != 0) {
        close_db(NOTIFICATIONS_DB_FILE);
        unlock_db_file(notifications_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    if (close_db(NOTIFICATIONS_DB_FILE) != 0) {
        unlock_db_file(notifications_lock_fd);
        pthread_mutex_unlock(&g_db_api_lock);
        return -1;
    }
    unlock_db_file(notifications_lock_fd);

    pthread_mutex_unlock(&g_db_api_lock);

    if (migrate_bookings) {
        fprintf(stderr,
                "Migrating legacy flat bookings.db to per-flight layout (booking IDs updated).\n");
        if (recreate_bookings_db_from_state(state) != 0) {
            return -1;
        }
    }

    return rc;
}

void storage_shutdown(ServerState *state) {
    storage_flush_flights(state);
    storage_flush_admins(state);
    storage_flush_users(state);
    storage_flush_bookings(state);
}
