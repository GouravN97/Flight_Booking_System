#include "client.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

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

    bool admin_authenticated = false;
    char current_admin[64] = {0};
    char current_admin_password[64] = {0};

    while (1) {
        int choice = 0;
        if (!admin_authenticated) {
            printf("\n=== Admin Console (Auth) ===\n");
            printf("1. Admin Login\n");
            printf("2. Exit\n");
        } else {
            printf("\n=== Admin Console (%s) ===\n", current_admin);
            printf("1. Create Admin User\n");
            printf("2. Create Flight\n");
            printf("3. Change Flight Timing\n");
            printf("4. Update Flight Price\n");
            printf("5. List All Flights\n");
            printf("6. List All Admins\n");
            printf("7. Write Shared-Memory Admin Message\n");
            printf("8. Read Shared-Memory Admin Feed\n");
            printf("9. Logout\n");
            printf("10. Exit\n");
        }
        printf("Enter choice: ");
        if (scanf("%d", &choice) != 1) {
            fprintf(stderr, "Invalid choice\n");
            return 1;
        }

        if (!admin_authenticated) {
            if (choice == 1) {
                char admin_id[64], password[64];
                read_token("Admin ID: ", admin_id, sizeof(admin_id));
                read_token("Admin password: ", password, sizeof(password));
                if (admin_login(admin_id, password) == 0) {
                    admin_authenticated = true;
                    snprintf(current_admin, sizeof(current_admin), "%s", admin_id);
                    snprintf(current_admin_password, sizeof(current_admin_password), "%s", password);
                    printf("Admin login OK\n");
                } else {
                    printf("Admin login failed\n");
                }
            } else if (choice == 2) {
                break;
            } else {
                printf("Unknown choice\n");
            }
            continue;
        }

        if (choice == 1) {
            char admin_id[64], password[64];
            read_token("New admin ID: ", admin_id, sizeof(admin_id));
            read_token("New admin password: ", password, sizeof(password));
            printf(admin_create_user(current_admin, current_admin_password, admin_id, password) == 0
                       ? "Admin created\n"
                       : "Admin create failed\n");
        } else if (choice == 2) {
            char flight_number[24], source[64], destination[64];
            int day, month, year, seats;
            double price;
            printf("Flight number: ");
            if (scanf("%23s", flight_number) != 1) {
                printf("Invalid flight number\n");
                continue;
            }
            read_token("Source: ", source, sizeof(source));
            read_token("Destination: ", destination, sizeof(destination));
            printf("Day Month Year: ");
            if (scanf("%d %d %d", &day, &month, &year) != 3) {
                printf("Invalid date\n");
                continue;
            }
            printf("Seats: ");
            if (scanf("%d", &seats) != 1) {
                printf("Invalid seats\n");
                continue;
            }
            printf("Price: ");
            if (scanf("%lf", &price) != 1) {
                printf("Invalid price\n");
                continue;
            }
            printf(admin_create_flight(flight_number,
                                       source,
                                       destination,
                                       day,
                                       month,
                                       year,
                                       seats,
                                       price,
                                       current_admin,
                                       current_admin_password) == 0
                       ? "Flight created\n"
                       : "Create flight failed\n");
        } else if (choice == 3) {
            char flight_number[24];
            int day, month, year;
            printf("Flight number: ");
            if (scanf("%23s", flight_number) != 1) {
                printf("Invalid flight number\n");
                continue;
            }
            printf("New Day Month Year: ");
            if (scanf("%d %d %d", &day, &month, &year) != 3) {
                printf("Invalid date\n");
                continue;
            }
            printf(admin_change_flight_timing(flight_number, day, month, year, current_admin, current_admin_password) == 0
                       ? "Timing updated\n"
                       : "Timing update failed\n");
        } else if (choice == 4) {
            char flight_number[24];
            double price;
            printf("Flight number: ");
            if (scanf("%23s", flight_number) != 1) {
                printf("Invalid flight number\n");
                continue;
            }
            printf("New price: ");
            if (scanf("%lf", &price) != 1) {
                printf("Invalid price\n");
                continue;
            }
            printf(admin_update_flight_price(flight_number, price, current_admin, current_admin_password) == 0
                       ? "Price updated\n"
                       : "Price update failed\n");
        } else if (choice == 5) {
            if (admin_list_all_flights(current_admin, current_admin_password) != 0) {
                printf("Failed to list flights\n");
            }
        } else if (choice == 6) {
            if (admin_list_all_admins(current_admin, current_admin_password) != 0) {
                printf("Failed to list admins\n");
            }
        } else if (choice == 7) {
            char message[256];
            printf("Admin message (single token, no spaces): ");
            if (scanf("%255s", message) != 1) {
                printf("Invalid message\n");
                continue;
            }
            printf(send_admin_message(message) == 0 ? "Admin message published\n" : "Publish failed\n");
        } else if (choice == 8) {
            if (read_admin_shared_message() != 0) {
                printf("Failed to read shared memory message\n");
            }
        } else if (choice == 9) {
            admin_authenticated = false;
            current_admin[0] = '\0';
            current_admin_password[0] = '\0';
            printf("Logged out\n");
        } else if (choice == 10) {
            break;
        } else {
            printf("Unknown choice\n");
        }
    }

    return 0;
}
