#include "client.h"
#include "token.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char g_server_host[64] = "127.0.0.1";
static int g_server_port = 9090;
static char g_auth_token[AUTH_TOKEN_SIZE];

int client_set_server(const char *host, int port) {
    if (host == NULL || port <= 0 || port > 65535) {
        return -1;
    }
    snprintf(g_server_host, sizeof(g_server_host), "%s", host);
    g_server_port = port;
    return 0;
}

void client_logout(void) {
    g_auth_token[0] = '\0';
}

static int request_server(const char *request, char *response, size_t response_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)g_server_port);

    if (inet_pton(AF_INET, g_server_host, &addr.sin_addr) != 1) {
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (send(fd, request, strlen(request), 0) < 0) {
        close(fd);
        return -1;
    }

    size_t total = 0;
    while (total + 1 < response_size) {
        ssize_t n = recv(fd, response + total, response_size - 1 - total, 0);
        if (n < 0) {
            close(fd);
            return -1;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }
    close(fd);
    if (total == 0) {
        return -1;
    }
    response[total] = '\0';
    return 0;
}

static int store_token_from_ok(const char *response) {
    if (strncmp(response, "OK ", 3) != 0) {
        return -1;
    }
    char token[AUTH_TOKEN_SIZE];
    if (sscanf(response + 3, "%191s", token) != 1 || token[0] == '\0') {
        return -1;
    }
    snprintf(g_auth_token, sizeof(g_auth_token), "%s", token);
    return 0;
}

static void print_login_notifications(const char *response) {
    char *newline = strchr(response, '\n');
    if (newline == NULL || newline[1] == '\0') {
        return;
    }
    if (strncmp(newline + 1, "NOTIFICATIONS:\n", 15) == 0) {
        printf("\n--- Notifications ---\n%s", newline + 1 + 15);
    } else {
        printf("%s", newline + 1);
    }
}

int client_ping_server(void) {
    char response[128];
    if (request_server("PING\n", response, sizeof(response)) != 0) {
        return -1;
    }
    return (strncmp(response, "ONLINE", 6) == 0 || strncmp(response, "PONG", 4) == 0) ? 0 : -1;
}

int signup(char *id, char *password) {
    char request[256];
    char response[4096];
    snprintf(request, sizeof(request), "SIGNUP %s %s\n", id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    if (store_token_from_ok(response) != 0) {
        return -1;
    }
    print_login_notifications(response);
    return 0;
}

int login(char *id, char *password) {
    char request[256];
    char response[4096];
    snprintf(request, sizeof(request), "LOGIN %s %s\n", id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    if (store_token_from_ok(response) != 0) {
        return -1;
    }
    print_login_notifications(response);
    return 0;
}

int view_available_flights(char *source, char *destination) {
    char request[512];
    char response[4096];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "LIST %s %s %s\n", g_auth_token, source, destination);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int book_seats(char *source,
               char *destination,
               int day,
               int month,
               int year,
               int num_seats) {
    char request[512];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request,
             sizeof(request),
             "BOOK %s %s %s %d %d %d %d\n",
             g_auth_token,
             source,
             destination,
             day,
             month,
             year,
             num_seats);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    if (strncmp(response, "OK ", 3) == 0) {
        printf("Booking confirmed: %s", response + 3);
        return 0;
    }
    if (strncmp(response, "WAITLISTED ", 11) == 0) {
        return BOOK_SEATS_WAITLISTED;
    }
    if (strncmp(response, "REQUESTED ", 10) == 0) {
        return BOOK_SEATS_ADMIN_REQUESTED;
    }
    return -1;
}

int cancel_booking(char *booking_id) {
    char request[512];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "CANCEL %s %s\n", g_auth_token, booking_id);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int view_flight_details(char *flight_number) {
    char request[512];
    char response[1024];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "DETAIL %s %s\n", g_auth_token, flight_number);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int view_my_bookings(void) {
    char request[512];
    char response[2048];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "MYBOOKINGS %s\n", g_auth_token);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int send_admin_message(char *message) {
    char request[768];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMINMSG %s %s\n", g_auth_token, message);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int read_admin_shared_message(void) {
    char request[512];
    char response[1024];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "READADMIN %s\n", g_auth_token);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("Shared memory message: %s\n", response);
    return 0;
}

int admin_login(char *admin_id, char *password) {
    char request[256];
    char response[512];
    snprintf(request, sizeof(request), "ADMIN_LOGIN %s %s\n", admin_id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return store_token_from_ok(response);
}

int admin_create_user(char *new_admin_id, char *new_admin_password) {
    char request[512];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMIN_CREATE %s %s %s\n",
             g_auth_token, new_admin_id, new_admin_password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_create_flight(char *flight_number,
                        char *source,
                        char *destination,
                        int day,
                        int month,
                        int year,
                        int num_seats,
                        double price) {
    char request[768];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request,
             sizeof(request),
             "ADMIN_CREATE_FLIGHT %s %s %s %s %d %d %d %d %.2f\n",
             g_auth_token,
             flight_number,
             source,
             destination,
             day,
             month,
             year,
             num_seats,
             price);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_change_flight_timing(char *flight_number, int day, int month, int year) {
    char request[512];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMIN_CHANGE_TIMING %s %s %d %d %d\n",
             g_auth_token, flight_number, day, month, year);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_update_flight_price(char *flight_number, double price) {
    char request[512];
    char response[256];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMIN_UPDATE_PRICE %s %s %.2f\n",
             g_auth_token, flight_number, price);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_list_all_flights(void) {
    char response[4096];
    char request[512];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMIN_LIST_FLIGHTS %s\n", g_auth_token);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int admin_list_all_admins(void) {
    char response[4096];
    char request[512];
    if (g_auth_token[0] == '\0') {
        return -1;
    }
    snprintf(request, sizeof(request), "ADMIN_LIST_ADMINS %s\n", g_auth_token);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}
