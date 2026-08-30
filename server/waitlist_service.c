#include "waitlist_service.h"
#include "booking_service.h"
#include "storage_service.h"
#include <stdio.h>
#include <string.h>

static int add_notification(ServerState *state, const char *user_id, const char *message) {
    int slot = -1;
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!state->notifications[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return -1;
    }

    state->notifications[slot].in_use = 1;
    snprintf(state->notifications[slot].user_id, sizeof(state->notifications[slot].user_id), "%s", user_id);
    snprintf(state->notifications[slot].message, sizeof(state->notifications[slot].message), "%s", message);
    return storage_write_notification_entry(state, slot);
}

int enqueue_waitlist(ServerState *state,
                     const char *user_id,
                     const char *source,
                     const char *destination,
                     int day,
                     int month,
                     int year,
                     int seat_count) {
    if (state == NULL || user_id == NULL || source == NULL || destination == NULL || seat_count <= 0) {
        return -1;
    }

    pthread_mutex_lock(&state->waitlist_lock);

    int existing = -1;
    for (int i = 0; i < MAX_WAITLIST; i++) {
        if (!state->waitlist[i].in_use) {
            continue;
        }
        if (strncmp(state->waitlist[i].user_id, user_id, sizeof(state->waitlist[i].user_id)) == 0 &&
            strncmp(state->waitlist[i].source, source, sizeof(state->waitlist[i].source)) == 0 &&
            strncmp(state->waitlist[i].destination, destination, sizeof(state->waitlist[i].destination)) == 0) {
            existing = i;
            break;
        }
    }

    int slot = existing;
    if (slot < 0) {
        for (int i = 0; i < MAX_WAITLIST; i++) {
            int idx = (state->next_waitlist_slot + i) % MAX_WAITLIST;
            if (!state->waitlist[idx].in_use) {
                slot = idx;
                state->next_waitlist_slot = (idx + 1) % MAX_WAITLIST;
                break;
            }
        }
    }

    if (slot < 0) {
        pthread_mutex_unlock(&state->waitlist_lock);
        return -1;
    }

    if (existing < 0) {
        memset(&state->waitlist[slot], 0, sizeof(state->waitlist[slot]));
        state->waitlist[slot].seq = state->next_waitlist_seq++;
        snprintf(state->waitlist[slot].user_id, sizeof(state->waitlist[slot].user_id), "%s", user_id);
        snprintf(state->waitlist[slot].source, sizeof(state->waitlist[slot].source), "%s", source);
        snprintf(state->waitlist[slot].destination, sizeof(state->waitlist[slot].destination), "%s", destination);
    }

    state->waitlist[slot].in_use = 1;
    state->waitlist[slot].day = day;
    state->waitlist[slot].month = month;
    state->waitlist[slot].year = year;
    state->waitlist[slot].seat_count = seat_count;

    int write_rc = storage_write_waitlist_entry(state, slot);
    pthread_mutex_unlock(&state->waitlist_lock);
    return write_rc;
}

int fulfill_waitlist_for_flight(ServerState *state, int flight_idx) {
    if (state == NULL || flight_idx < 0 || flight_idx >= MAX_FLIGHTS) {
        return -1;
    }

    Flight *flight = &state->flights[flight_idx];
    if (!flight->in_use) {
        return -1;
    }

    int ordered[MAX_WAITLIST];
    int ordered_count = 0;

    pthread_mutex_lock(&state->waitlist_lock);
    for (int i = 0; i < MAX_WAITLIST; i++) {
        if (!state->waitlist[i].in_use) {
            continue;
        }
        if (strncmp(state->waitlist[i].source, flight->source, sizeof(state->waitlist[i].source)) != 0 ||
            strncmp(state->waitlist[i].destination, flight->destination, sizeof(state->waitlist[i].destination)) != 0) {
            continue;
        }
        ordered[ordered_count++] = i;
    }

    for (int i = 1; i < ordered_count; i++) {
        int key = ordered[i];
        int j = i;
        while (j > 0 && state->waitlist[ordered[j - 1]].seq > state->waitlist[key].seq) {
            ordered[j] = ordered[j - 1];
            j--;
        }
        ordered[j] = key;
    }
    pthread_mutex_unlock(&state->waitlist_lock);

    int booked_count = 0;
    for (int n = 0; n < ordered_count; n++) {
        int idx = ordered[n];
        WaitlistEntry entry;
        pthread_mutex_lock(&state->waitlist_lock);
        if (!state->waitlist[idx].in_use) {
            pthread_mutex_unlock(&state->waitlist_lock);
            continue;
        }
        entry = state->waitlist[idx];
        pthread_mutex_unlock(&state->waitlist_lock);

        int available = 0;
        sem_getvalue(&flight->seats_sem, &available);
        if (available < entry.seat_count) {
            continue;
        }

        char booking_id[64];
        int rc = book_seats_on_flight(state,
                                      flight_idx,
                                      entry.user_id,
                                      entry.seat_count,
                                      booking_id,
                                      sizeof(booking_id),
                                      1,
                                      0);
        if (rc != 0) {
            continue;
        }

        pthread_mutex_lock(&state->waitlist_lock);
        state->waitlist[idx].in_use = 0;
        pthread_mutex_unlock(&state->waitlist_lock);
        (void)storage_delete_waitlist_entry(idx);

        char message[512];
        snprintf(message,
                 sizeof(message),
                 "Waitlist booking confirmed: %s %s->%s date:%02d/%02d/%04d booking:%s seats:%d",
                 flight->flight_number,
                 flight->source,
                 flight->destination,
                 flight->day,
                 flight->month,
                 flight->year,
                 booking_id,
                 entry.seat_count);
        pthread_mutex_lock(&state->waitlist_lock);
        (void)add_notification(state, entry.user_id, message);
        pthread_mutex_unlock(&state->waitlist_lock);
        booked_count++;
    }

    return booked_count;
}

int format_and_consume_notifications(ServerState *state,
                                     const char *user_id,
                                     char *out,
                                     size_t out_size) {
    if (state == NULL || user_id == NULL || out == NULL || out_size == 0) {
        return -1;
    }

    int pending[MAX_NOTIFICATIONS];
    int pending_count = 0;

    pthread_mutex_lock(&state->waitlist_lock);
    for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
        if (!state->notifications[i].in_use) {
            continue;
        }
        if (strncmp(state->notifications[i].user_id, user_id, sizeof(state->notifications[i].user_id)) != 0) {
            continue;
        }
        pending[pending_count++] = i;
    }

    if (pending_count == 0) {
        pthread_mutex_unlock(&state->waitlist_lock);
        if (out_size > 0) {
            out[0] = '\0';
        }
        return 0;
    }

    size_t off = (size_t)snprintf(out, out_size, "NOTIFICATIONS:\n");
    int written = 0;
    for (int i = 0; i < pending_count && off < out_size - 1; i++) {
        int n = snprintf(out + off,
                         out_size - off,
                         "%s\n",
                         state->notifications[pending[i]].message);
        if (n < 0 || (size_t)n >= out_size - off) {
            break;
        }
        off += (size_t)n;
        written++;
    }

    for (int i = 0; i < written; i++) {
        int idx = pending[i];
        state->notifications[idx].in_use = 0;
        (void)storage_delete_notification_entry(idx);
    }
    pthread_mutex_unlock(&state->waitlist_lock);
    return 0;
}
