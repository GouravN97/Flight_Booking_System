#include "client.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char g_server_host[64] = "127.0.0.1";
static int g_server_port = 9090;

int client_set_server(const char *host, int port) {
    if (host == NULL || port <= 0 || port > 65535) {
        return -1;
    }
    snprintf(g_server_host, sizeof(g_server_host), "%s", host);
    g_server_port = port;
    return 0;
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

    ssize_t n = recv(fd, response, response_size - 1, 0);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    response[n] = '\0';
    return 0;
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
    char response[256];
    snprintf(request, sizeof(request), "SIGNUP %s %s\n", id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int login(char *id, char *password) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "LOGIN %s %s\n", id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int view_available_flights(char *source, char *destination) {
    char request[256];
    char response[1024];
    snprintf(request, sizeof(request), "LIST %s %s\n", source, destination);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int book_seats(char *user_id,
               char *source,
               char *destination,
               int day,
               int month,
               int year,
               int num_seats) {
    char request[256];
    char response[256];
    snprintf(request,
             sizeof(request),
             "BOOK %s %s %s %d %d %d %d\n",
             user_id,
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
    if (strncmp(response, "REQUESTED ", 10) == 0) {
        return BOOK_SEATS_ADMIN_REQUESTED;
    }
    return -1;
}

int cancel_booking(char *booking_id) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "CANCEL %s\n", booking_id);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int view_flight_details(char *flight_number) {
    char request[256];
    char response[1024];
    snprintf(request, sizeof(request), "DETAIL %s\n", flight_number);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int view_my_bookings(char *user_id) {
    char request[256];
    char response[2048];
    snprintf(request, sizeof(request), "MYBOOKINGS %s\n", user_id);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int send_admin_message(char *message) {
    char request[512];
    char response[256];
    snprintf(request, sizeof(request), "ADMINMSG %s\n", message);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int read_admin_shared_message(void) {
    char response[1024];
    if (request_server("READADMIN\n", response, sizeof(response)) != 0) {
        return -1;
    }
    printf("Shared memory message: %s\n", response);
    return 0;
}

int admin_login(char *admin_id, char *password) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "ADMIN_LOGIN %s %s\n", admin_id, password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_create_user(char *actor_admin_id, char *actor_password, char *new_admin_id, char *new_admin_password) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "ADMIN_CREATE %s %s %s %s\n",
             actor_admin_id, actor_password, new_admin_id, new_admin_password);
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
                        double price,
                        char *actor_admin_id,
                        char *actor_password) {
    char request[512];
    char response[256];
    snprintf(request,
             sizeof(request),
             "ADMIN_CREATE_FLIGHT %s %s %s %s %s %d %d %d %d %.2f\n",
             actor_admin_id,
             actor_password,
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

int admin_change_flight_timing(char *flight_number, int day, int month, int year, char *actor_admin_id, char *actor_password) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "ADMIN_CHANGE_TIMING %s %s %s %d %d %d\n",
             actor_admin_id, actor_password, flight_number, day, month, year);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_update_flight_price(char *flight_number, double price, char *actor_admin_id, char *actor_password) {
    char request[256];
    char response[256];
    snprintf(request, sizeof(request), "ADMIN_UPDATE_PRICE %s %s %s %.2f\n",
             actor_admin_id, actor_password, flight_number, price);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    return strncmp(response, "OK", 2) == 0 ? 0 : -1;
}

int admin_list_all_flights(char *actor_admin_id, char *actor_password) {
    char response[4096];
    char request[256];
    snprintf(request, sizeof(request), "ADMIN_LIST_FLIGHTS %s %s\n", actor_admin_id, actor_password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}

int admin_list_all_admins(char *actor_admin_id, char *actor_password) {
    char response[4096];
    char request[256];
    snprintf(request, sizeof(request), "ADMIN_LIST_ADMINS %s %s\n", actor_admin_id, actor_password);
    if (request_server(request, response, sizeof(response)) != 0) {
        return -1;
    }
    printf("%s", response);
    return 0;
}
