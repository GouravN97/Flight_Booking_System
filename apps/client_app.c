#include "client.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void read_token(const char *prompt, char *out, size_t out_size) {
    printf("%s", prompt);
    if (scanf("%63s", out) != 1) {
        out[0] = '\0';
    }
    (void)out_size;
}

int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 9090;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    if (client_set_server(host, port) != 0) {
        fprintf(stderr, "Invalid server host/port\n");
        return 1;
    }

    if (client_ping_server() != 0) {
        fprintf(stderr, "Server is not reachable at %s:%d. Start server first.\n", host, port);
        return 1;
    }

    bool authenticated = false;
    char current_user[64] = {0};

    while (1) {
        int choice = 0;
        if (!authenticated) {
            printf("\n=== Flight Booking Client (Auth) ===\n");
            printf("1. Signup\n");
            printf("2. Login\n");
            printf("3. Exit\n");
        } else {
            printf("\n=== Flight Booking Client (%s) ===\n", current_user);
            printf("1. List Flights (source,destination)\n");
            printf("2. Flight Details (flight number)\n");
            printf("3. Book (source,destination,date,seats)\n");
            printf("4. View My Bookings\n");
            printf("5. Cancel Booking\n");
            printf("6. Logout\n");
            printf("7. Exit\n");
        }
        printf("Enter choice: ");

        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "Invalid choice\n");
            return 1;
        }

        if (!authenticated) {
            if (choice == 1) {
                char id[64], password[64];
                read_token("User ID: ", id, sizeof(id));
                read_token("Password: ", password, sizeof(password));
                if (signup(id, password) == 0) {
                    authenticated = true;
                    snprintf(current_user, sizeof(current_user), "%s", id);
                    printf("Signup OK. Logged in as %s\n", current_user);
                } else {
                    printf("Signup failed\n");
                }
            } else if (choice == 2) {
                char id[64], password[64];
                read_token("User ID: ", id, sizeof(id));
                read_token("Password: ", password, sizeof(password));
                if (login(id, password) == 0) {
                    authenticated = true;
                    snprintf(current_user, sizeof(current_user), "%s", id);
                    printf("Login OK. Welcome %s\n", current_user);
                } else {
                    printf("Login failed\n");
                }
            } else if (choice == 3) {
                break;
            } else {
                printf("Unknown choice\n");
            }
        } else {
            if (choice == 1) {
                char source[64], destination[64];
                read_token("Source: ", source, sizeof(source));
                read_token("Destination: ", destination, sizeof(destination));
                if (view_available_flights(source, destination) != 0) {
                    printf("Failed to fetch flights\n");
                }
            } else if (choice == 2) {
                char flight_number[24];
                printf("Flight number: ");
                if (scanf("%23s", flight_number) != 1) {
                    printf("Invalid flight number\n");
                    continue;
                }
                if (view_flight_details(flight_number) != 0) {
                    printf("Failed to fetch flight details\n");
                }
            } else if (choice == 3) {
                char source[64], destination[64];
                int day, month, year, seats;
                read_token("Source: ", source, sizeof(source));
                read_token("Destination: ", destination, sizeof(destination));
                printf("Day Month Year (e.g. 1 5 2026): ");
                if (scanf("%d %d %d", &day, &month, &year) != 3) {
                    printf("Invalid date\n");
                    continue;
                }
                printf("Seats: ");
                if (scanf("%d", &seats) != 1) {
                    printf("Invalid seats\n");
                    continue;
                }
                int booking_result = book_seats(source, destination, day, month, year, seats);
                if (booking_result == 0) {
                    printf("Booking OK\n");
                } else if (booking_result == BOOK_SEATS_WAITLISTED) {
                    printf("Flight is full. You were added to the waitlist and will be booked when a new flight on this route is created.\n");
                } else if (booking_result == BOOK_SEATS_ADMIN_REQUESTED) {
                    printf("Flight unavailable. Creation request sent to admins\n");
                } else {
                    printf("Booking failed\n");
                }
            } else if (choice == 4) {
                if (view_my_bookings() != 0) {
                    printf("Failed to fetch bookings\n");
                }
            } else if (choice == 5) {
                char booking_id[64];
                read_token("Booking ID: ", booking_id, sizeof(booking_id));
                printf(cancel_booking(booking_id) == 0 ? "Cancelled\n" : "Cancel failed\n");
            } else if (choice == 6) {
                authenticated = false;
                current_user[0] = '\0';
                client_logout();
                printf("Logged out\n");
            } else if (choice == 7) {
                break;
            } else {
                printf("Unknown choice\n");
            }
        }
    }

    return 0;
}
