/*
 * Concurrent booking integration tests for the flight booking server.
 *
 * Each test case uses hard-coded protocol commands and expected response
 * patterns. Concurrent launches are synchronized with pthread barriers so
 * all clients hit the server at the same time.
 *
 * Run via: tests/run_concurrency_tests.sh
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RESP_SIZE 4096
#define MAX_CONCURRENT 64
#define MAX_SETUP 32

typedef struct {
    const char *host;
    int port;
    const char *command;
    char command_buf[768];
    char response[RESP_SIZE];
    int status; /* 0=unset, 1=ok, 2=err, 3=other */
    int thread_rc;
    pthread_barrier_t *barrier;
} ConcurrentJob;

typedef struct {
    const char *name;
    const char *description;
    const char *setup[MAX_SETUP];
    const char *commands[MAX_CONCURRENT];
    int command_count;
    int expected_ok;
    int expected_reject; /* ERR, REQUESTED, or WAITLISTED under contention */
    const char *verify_command;
    const char *verify_substr;
    const char *verify_command_b;
    const char *verify_substr_b;
    const char *required_ok_substrings[8];
    int verify_unique_booking_ids;
    const char *expected_id_prefix;
} TestCase;

static int tests_run = 0;
static int tests_passed = 0;

#define TOKEN_SIZE 192
#define MAX_CACHED_TOKENS 80

static char g_admin_token[TOKEN_SIZE];
static struct {
    char id[64];
    char token[TOKEN_SIZE];
} g_user_tokens[MAX_CACHED_TOKENS];
static int g_user_token_count;

static void reset_auth_cache(void) {
    g_admin_token[0] = '\0';
    g_user_token_count = 0;
    memset(g_user_tokens, 0, sizeof(g_user_tokens));
}

static int parse_ok_token(const char *response, char *out, size_t out_size) {
    if (response == NULL || strncmp(response, "OK ", 3) != 0) {
        return -1;
    }
    char token[TOKEN_SIZE];
    if (sscanf(response + 3, "%191s", token) != 1 || token[0] == '\0') {
        return -1;
    }
    snprintf(out, out_size, "%s", token);
    return 0;
}

static int cache_user_token(const char *id, const char *token) {
    for (int i = 0; i < g_user_token_count; i++) {
        if (strcmp(g_user_tokens[i].id, id) == 0) {
            snprintf(g_user_tokens[i].token, sizeof(g_user_tokens[i].token), "%s", token);
            return 0;
        }
    }
    if (g_user_token_count >= MAX_CACHED_TOKENS) {
        return -1;
    }
    snprintf(g_user_tokens[g_user_token_count].id, sizeof(g_user_tokens[g_user_token_count].id), "%s", id);
    snprintf(g_user_tokens[g_user_token_count].token, sizeof(g_user_tokens[g_user_token_count].token), "%s", token);
    g_user_token_count++;
    return 0;
}

static const char *lookup_user_token(const char *id) {
    for (int i = 0; i < g_user_token_count; i++) {
        if (strcmp(g_user_tokens[i].id, id) == 0) {
            return g_user_tokens[i].token;
        }
    }
    return NULL;
}

static int connect_server(const char *host, int port) {
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    char port_str[16];

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

static int send_command(const char *host, int port, const char *command, char *response, size_t response_size) {
    if (response == NULL || response_size == 0) {
        return -1;
    }
    response[0] = '\0';

    int fd = connect_server(host, port);
    if (fd < 0) {
        return -1;
    }

    char payload[512];
    snprintf(payload, sizeof(payload), "%s\n", command);

    size_t total = 0;
    while (total < strlen(payload)) {
        ssize_t n = send(fd, payload + total, strlen(payload) - total, 0);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        total += (size_t)n;
    }

    size_t off = 0;
    while (off + 1 < response_size) {
        ssize_t n = recv(fd, response + off, response_size - off - 1, 0);
        if (n <= 0) {
            break;
        }
        off += (size_t)n;
        if (memchr(response, '\n', off) != NULL) {
            break;
        }
    }

    response[off] = '\0';
    close(fd);
    return off > 0 ? 0 : -1;
}

static int ensure_admin_token(const char *host, int port) {
    if (g_admin_token[0] != '\0') {
        return 0;
    }
    char response[RESP_SIZE];
    if (send_command(host, port, "ADMIN_LOGIN admin admin123", response, sizeof(response)) != 0) {
        return -1;
    }
    return parse_ok_token(response, g_admin_token, sizeof(g_admin_token));
}

static int rewrite_auth_command(const char *in, char *out, size_t out_size) {
    char cmd[64];
    if (in == NULL || sscanf(in, "%63s", cmd) != 1) {
        return -1;
    }

    if (strcmp(cmd, "ADMIN_CREATE_FLIGHT") == 0) {
        const char *rest = strstr(in, "admin123");
        if (rest == NULL || g_admin_token[0] == '\0') {
            return -1;
        }
        rest += strlen("admin123");
        while (*rest == ' ') {
            rest++;
        }
        snprintf(out, out_size, "ADMIN_CREATE_FLIGHT %s %s", g_admin_token, rest);
        return 0;
    }

    if (strcmp(cmd, "BOOK") == 0) {
        char user[64];
        if (sscanf(in, "BOOK %63s", user) != 1) {
            return -1;
        }
        const char *token = lookup_user_token(user);
        if (token == NULL) {
            return -1;
        }
        const char *rest = in + 4;
        while (*rest == ' ') {
            rest++;
        }
        while (*rest != '\0' && *rest != ' ') {
            rest++;
        }
        if (*rest == ' ') {
            rest++;
        }
        snprintf(out, out_size, "BOOK %s %s", token, rest);
        return 0;
    }

    if (strcmp(cmd, "DETAIL") == 0) {
        char flight[24];
        if (sscanf(in, "DETAIL %23s", flight) != 1 || g_admin_token[0] == '\0') {
            return -1;
        }
        snprintf(out, out_size, "DETAIL %s %s", g_admin_token, flight);
        return 0;
    }

    snprintf(out, out_size, "%s", in);
    return 0;
}

static int classify_response(const char *response) {
    if (response == NULL) {
        return 3;
    }
    if (strncmp(response, "OK", 2) == 0) {
        return 1;
    }
    if (strncmp(response, "ERR", 3) == 0) {
        return 2;
    }
    if (strncmp(response, "REQUESTED", 9) == 0) {
        return 4;
    }
    if (strncmp(response, "WAITLISTED", 10) == 0) {
        return 5;
    }
    return 3;
}

static int is_rejection_status(int status) {
    return status == 2 || status == 4 || status == 5;
}

static void *concurrent_worker(void *arg) {
    ConcurrentJob *job = (ConcurrentJob *)arg;
    job->thread_rc = 0;
    job->status = 0;
    job->response[0] = '\0';

    if (pthread_barrier_wait(job->barrier) == PTHREAD_BARRIER_SERIAL_THREAD) {
        /* allowed */
    }

    if (send_command(job->host, job->port, job->command, job->response, sizeof(job->response)) != 0) {
        job->thread_rc = -1;
        return NULL;
    }

    job->status = classify_response(job->response);
    return NULL;
}

static int run_setup(const char *host, int port, const char *const *setup) {
    char response[RESP_SIZE];
    if (ensure_admin_token(host, port) != 0) {
        fprintf(stderr, "  admin login for setup failed\n");
        return -1;
    }
    for (int i = 0; setup[i] != NULL; i++) {
        char rewritten[768];
        if (rewrite_auth_command(setup[i], rewritten, sizeof(rewritten)) != 0) {
            fprintf(stderr, "  setup rewrite failed: %s\n", setup[i]);
            return -1;
        }
        if (send_command(host, port, rewritten, response, sizeof(response)) != 0) {
            fprintf(stderr, "  setup send failed: %s\n", setup[i]);
            return -1;
        }
        if (strncmp(setup[i], "SIGNUP ", 7) == 0) {
            char user[64], password[64], token[TOKEN_SIZE];
            if (sscanf(setup[i], "SIGNUP %63s %63s", user, password) != 2) {
                fprintf(stderr, "  invalid SIGNUP setup: %s\n", setup[i]);
                return -1;
            }
            if (strncmp(response, "OK ", 3) == 0) {
                if (parse_ok_token(response, token, sizeof(token)) != 0 || cache_user_token(user, token) != 0) {
                    fprintf(stderr, "  failed to cache token for %s\n", user);
                    return -1;
                }
                continue;
            }
            if (strncmp(response, "ERROR:", 6) == 0) {
                char login_cmd[160];
                snprintf(login_cmd, sizeof(login_cmd), "LOGIN %s %s", user, password);
                if (send_command(host, port, login_cmd, response, sizeof(response)) != 0 ||
                    parse_ok_token(response, token, sizeof(token)) != 0 ||
                    cache_user_token(user, token) != 0) {
                    fprintf(stderr, "  login after existing signup failed for %s: %s", user, response);
                    return -1;
                }
                continue;
            }
            fprintf(stderr, "  setup unexpected SIGNUP response for `%s`:\n    got: %s", setup[i], response);
            return -1;
        }
        if (strncmp(response, "OK", 2) != 0) {
            fprintf(stderr, "  setup unexpected response for `%s`:\n    got: %s", setup[i], response);
            return -1;
        }
    }
    return 0;
}

static int response_contains_booking_id(const char *response, const char *prefix, char ids[][64], int *id_count, int id_cap) {
    if (strncmp(response, "OK ", 3) != 0) {
        return 0;
    }
    const char *id = response + 3;
    while (*id == ' ') {
        id++;
    }
    char trimmed[64];
    snprintf(trimmed, sizeof(trimmed), "%s", id);
    char *nl = strchr(trimmed, '\n');
    if (nl != NULL) {
        *nl = '\0';
    }

    if (prefix != NULL && strncmp(trimmed, prefix, strlen(prefix)) != 0) {
        return -1;
    }

    for (int i = 0; i < *id_count; i++) {
        if (strcmp(ids[i], trimmed) == 0) {
            return -2;
        }
    }

    if (*id_count >= id_cap) {
        return -3;
    }

    snprintf(ids[*id_count], 64, "%s", trimmed);
    (*id_count)++;
    return 1;
}

static int run_test_case(const char *host, int port, const TestCase *tc) {
    printf("\n=== TEST: %s ===\n", tc->name);
    printf("%s\n", tc->description);

    reset_auth_cache();
    if (run_setup(host, port, tc->setup) != 0) {
        printf("RESULT: FAIL (setup)\n");
        return 0;
    }

    if (tc->command_count <= 0 || tc->command_count > MAX_CONCURRENT) {
        printf("RESULT: FAIL (invalid command count)\n");
        return 0;
    }

    pthread_barrier_t barrier;
    if (pthread_barrier_init(&barrier, NULL, (unsigned)tc->command_count) != 0) {
        printf("RESULT: FAIL (barrier init)\n");
        return 0;
    }

    ConcurrentJob jobs[MAX_CONCURRENT];
    pthread_t threads[MAX_CONCURRENT];
    memset(jobs, 0, sizeof(jobs));

    for (int i = 0; i < tc->command_count; i++) {
        jobs[i].host = host;
        jobs[i].port = port;
        if (rewrite_auth_command(tc->commands[i], jobs[i].command_buf, sizeof(jobs[i].command_buf)) != 0) {
            printf("RESULT: FAIL (rewrite %s)\n", tc->commands[i]);
            pthread_barrier_destroy(&barrier);
            return 0;
        }
        jobs[i].command = jobs[i].command_buf;
        jobs[i].barrier = &barrier;
        if (pthread_create(&threads[i], NULL, concurrent_worker, &jobs[i]) != 0) {
            printf("RESULT: FAIL (pthread_create)\n");
            pthread_barrier_destroy(&barrier);
            return 0;
        }
    }

    int ok_count = 0;
    int reject_count = 0;
    int other_count = 0;
    char booking_ids[MAX_CONCURRENT][64];
    int booking_id_count = 0;

    for (int i = 0; i < tc->command_count; i++) {
        pthread_join(threads[i], NULL);
        if (jobs[i].thread_rc != 0) {
            printf("  worker %d failed to communicate: cmd=`%s`\n", i, tc->commands[i]);
            other_count++;
            continue;
        }

        printf("  [%02d] cmd: %-55s => %s", i, tc->commands[i], jobs[i].response);
        if (jobs[i].status == 1) {
            ok_count++;
            if (tc->verify_unique_booking_ids) {
                int id_rc = response_contains_booking_id(jobs[i].response,
                                                           tc->expected_id_prefix,
                                                           booking_ids,
                                                           &booking_id_count,
                                                           MAX_CONCURRENT);
                if (id_rc < 0) {
                    printf("  booking id check failed (rc=%d) for response above\n", id_rc);
                    other_count++;
                }
            }
        } else if (is_rejection_status(jobs[i].status)) {
            reject_count++;
        } else {
            other_count++;
        }
    }

    pthread_barrier_destroy(&barrier);

    int pass = 1;
    if (ok_count != tc->expected_ok) {
        printf("  expected OK count %d, got %d\n", tc->expected_ok, ok_count);
        pass = 0;
    }
    if (reject_count != tc->expected_reject) {
        printf("  expected reject count %d (ERR, REQUESTED, or WAITLISTED), got %d\n", tc->expected_reject, reject_count);
        pass = 0;
    }
    if (other_count != 0) {
        printf("  unexpected non-OK/ERR responses: %d\n", other_count);
        pass = 0;
    }

    if (tc->verify_unique_booking_ids && pass) {
        if (booking_id_count != tc->expected_ok) {
            printf("  expected %d unique booking ids, collected %d\n", tc->expected_ok, booking_id_count);
            pass = 0;
        } else {
            printf("  unique booking ids verified (%d)\n", booking_id_count);
        }
    }

    if (tc->verify_command != NULL && tc->verify_substr != NULL) {
        char verify_resp[RESP_SIZE];
        char verify_cmd[768];
        if (rewrite_auth_command(tc->verify_command, verify_cmd, sizeof(verify_cmd)) != 0) {
            printf("  verify rewrite failed: %s\n", tc->verify_command);
            pass = 0;
        } else if (send_command(host, port, verify_cmd, verify_resp, sizeof(verify_resp)) != 0) {
            printf("  verify command failed: %s\n", tc->verify_command);
            pass = 0;
        } else {
            printf("  verify `%s` => %s", tc->verify_command, verify_resp);
            if (strstr(verify_resp, tc->verify_substr) == NULL) {
                printf("  expected substring missing: `%s`\n", tc->verify_substr);
                pass = 0;
            }
        }
    }

    if (pass && tc->verify_command_b != NULL && tc->verify_substr_b != NULL) {
        char verify_resp[RESP_SIZE];
        char verify_cmd[768];
        if (rewrite_auth_command(tc->verify_command_b, verify_cmd, sizeof(verify_cmd)) != 0) {
            printf("  verify rewrite failed: %s\n", tc->verify_command_b);
            pass = 0;
        } else if (send_command(host, port, verify_cmd, verify_resp, sizeof(verify_resp)) != 0) {
            printf("  verify command failed: %s\n", tc->verify_command_b);
            pass = 0;
        } else {
            printf("  verify `%s` => %s", tc->verify_command_b, verify_resp);
            if (strstr(verify_resp, tc->verify_substr_b) == NULL) {
                printf("  expected substring missing: `%s`\n", tc->verify_substr_b);
                pass = 0;
            }
        }
    }

    if (pass && tc->required_ok_substrings[0] != NULL) {
        for (int req = 0; tc->required_ok_substrings[req] != NULL; req++) {
            int found = 0;
            for (int i = 0; i < tc->command_count; i++) {
                if (jobs[i].status == 1 && strstr(jobs[i].response, tc->required_ok_substrings[req]) != NULL) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                printf("  required OK substring not found: `%s`\n", tc->required_ok_substrings[req]);
                pass = 0;
            }
        }
    }

    if (pass) {
        printf("RESULT: PASS\n");
        return 1;
    }

    printf("RESULT: FAIL\n");
    return 0;
}


int main(int argc, char **argv) {
    const char *host = "127.0.0.1";
    int port = 19090;

    if (argc >= 2) {
        host = argv[1];
    }
    if (argc >= 3) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
    }

    char ping_resp[RESP_SIZE];
    if (send_command(host, port, "PING", ping_resp, sizeof(ping_resp)) != 0 ||
        strstr(ping_resp, "ONLINE") == NULL) {
        fprintf(stderr, "Server not reachable at %s:%d (PING failed)\n", host, port);
        return 1;
    }

    TestCase cases[] = {
        {
            .name = "01_single_seat_race_exact_capacity",
            .description =
                "10 clients each book 1 seat on CF101 (capacity 5) at the same instant.\n"
                "Expect exactly 5 successes and 5 failures with no overbooking.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF101 BLR DEL 15 8 2026 5 1000.00",
                "SIGNUP user01 pass01",
                "SIGNUP user02 pass02",
                "SIGNUP user03 pass03",
                "SIGNUP user04 pass04",
                "SIGNUP user05 pass05",
                "SIGNUP user06 pass06",
                "SIGNUP user07 pass07",
                "SIGNUP user08 pass08",
                "SIGNUP user09 pass09",
                "SIGNUP user10 pass10",
                NULL,
            },
            .commands = {
                "BOOK user01 BLR DEL 15 8 2026 1",
                "BOOK user02 BLR DEL 15 8 2026 1",
                "BOOK user03 BLR DEL 15 8 2026 1",
                "BOOK user04 BLR DEL 15 8 2026 1",
                "BOOK user05 BLR DEL 15 8 2026 1",
                "BOOK user06 BLR DEL 15 8 2026 1",
                "BOOK user07 BLR DEL 15 8 2026 1",
                "BOOK user08 BLR DEL 15 8 2026 1",
                "BOOK user09 BLR DEL 15 8 2026 1",
                "BOOK user10 BLR DEL 15 8 2026 1",
            },
            .command_count = 10,
            .expected_ok = 5,
            .expected_reject = 5,
            .verify_command = "DETAIL CF101",
            .verify_substr = "seats:0/5",
            .verify_unique_booking_ids = 1,
            .expected_id_prefix = "CF101-BK",
        },
        {
            .name = "02_parallel_different_flights_no_interference",
            .description =
                "5 clients book CF102 while 5 book CF103 concurrently.\n"
                "Different flights should not block each other; all 10 succeed.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF102 BLR MUM 16 8 2026 5 2000.00",
                "ADMIN_CREATE_FLIGHT admin admin123 CF103 BLR HYD 16 8 2026 5 3000.00",
                "SIGNUP user11 pass11",
                "SIGNUP user12 pass12",
                "SIGNUP user13 pass13",
                "SIGNUP user14 pass14",
                "SIGNUP user15 pass15",
                "SIGNUP user16 pass16",
                "SIGNUP user17 pass17",
                "SIGNUP user18 pass18",
                "SIGNUP user19 pass19",
                "SIGNUP user20 pass20",
                NULL,
            },
            .commands = {
                "BOOK user11 BLR MUM 16 8 2026 1",
                "BOOK user12 BLR MUM 16 8 2026 1",
                "BOOK user13 BLR MUM 16 8 2026 1",
                "BOOK user14 BLR MUM 16 8 2026 1",
                "BOOK user15 BLR MUM 16 8 2026 1",
                "BOOK user16 BLR HYD 16 8 2026 1",
                "BOOK user17 BLR HYD 16 8 2026 1",
                "BOOK user18 BLR HYD 16 8 2026 1",
                "BOOK user19 BLR HYD 16 8 2026 1",
                "BOOK user20 BLR HYD 16 8 2026 1",
            },
            .command_count = 10,
            .expected_ok = 10,
            .expected_reject = 0,
            .verify_command = "DETAIL CF102",
            .verify_substr = "seats:0/5",
            .verify_command_b = "DETAIL CF103",
            .verify_substr_b = "seats:0/5",
            .verify_unique_booking_ids = 0,
            .expected_id_prefix = NULL,
        },
        {
            .name = "03_multi_seat_race_fills_flight",
            .description =
                "5 clients each request 2 seats on CF104 (capacity 10) concurrently.\n"
                "Expect exactly 5 OK (10 seats total) and no ERR.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF104 COK TRV 17 8 2026 10 1500.00",
                "SIGNUP user01 pass01",
                "SIGNUP user02 pass02",
                "SIGNUP user03 pass03",
                "SIGNUP user04 pass04",
                "SIGNUP user05 pass05",
                NULL,
            },
            .commands = {
                "BOOK user01 COK TRV 17 8 2026 2",
                "BOOK user02 COK TRV 17 8 2026 2",
                "BOOK user03 COK TRV 17 8 2026 2",
                "BOOK user04 COK TRV 17 8 2026 2",
                "BOOK user05 COK TRV 17 8 2026 2",
            },
            .command_count = 5,
            .expected_ok = 5,
            .expected_reject = 0,
            .verify_command = "DETAIL CF104",
            .verify_substr = "seats:0/10",
            .verify_unique_booking_ids = 1,
            .expected_id_prefix = "CF104-BK",
        },
        {
            .name = "04_multi_seat_race_with_rejections",
            .description =
                "10 clients each request 2 seats on CF105 (capacity 10) concurrently.\n"
                "Only 5 can succeed; the other 5 must fail.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF105 IXC GAU 18 8 2026 10 1500.00",
                "SIGNUP user06 pass06",
                "SIGNUP user07 pass07",
                "SIGNUP user08 pass08",
                "SIGNUP user09 pass09",
                "SIGNUP user10 pass10",
                "SIGNUP user11 pass11",
                "SIGNUP user12 pass12",
                "SIGNUP user13 pass13",
                "SIGNUP user14 pass14",
                "SIGNUP user15 pass15",
                NULL,
            },
            .commands = {
                "BOOK user06 IXC GAU 18 8 2026 2",
                "BOOK user07 IXC GAU 18 8 2026 2",
                "BOOK user08 IXC GAU 18 8 2026 2",
                "BOOK user09 IXC GAU 18 8 2026 2",
                "BOOK user10 IXC GAU 18 8 2026 2",
                "BOOK user11 IXC GAU 18 8 2026 2",
                "BOOK user12 IXC GAU 18 8 2026 2",
                "BOOK user13 IXC GAU 18 8 2026 2",
                "BOOK user14 IXC GAU 18 8 2026 2",
                "BOOK user15 IXC GAU 18 8 2026 2",
            },
            .command_count = 10,
            .expected_ok = 5,
            .expected_reject = 5,
            .verify_command = "DETAIL CF105",
            .verify_substr = "seats:0/10",
            .verify_unique_booking_ids = 1,
            .expected_id_prefix = "CF105-BK",
        },
        {
            .name = "05_last_seat_many_contenders",
            .description =
                "8 clients race for 1 remaining-style scenario on CF106 (capacity 1).\n"
                "Expect 1 OK and 7 ERR booking failed.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF106 JAI UDR 19 8 2026 1 500.00",
                "SIGNUP user01 pass01",
                "SIGNUP user02 pass02",
                "SIGNUP user03 pass03",
                "SIGNUP user04 pass04",
                "SIGNUP user05 pass05",
                "SIGNUP user06 pass06",
                "SIGNUP user07 pass07",
                "SIGNUP user08 pass08",
                NULL,
            },
            .commands = {
                "BOOK user01 JAI UDR 19 8 2026 1",
                "BOOK user02 JAI UDR 19 8 2026 1",
                "BOOK user03 JAI UDR 19 8 2026 1",
                "BOOK user04 JAI UDR 19 8 2026 1",
                "BOOK user05 JAI UDR 19 8 2026 1",
                "BOOK user06 JAI UDR 19 8 2026 1",
                "BOOK user07 JAI UDR 19 8 2026 1",
                "BOOK user08 JAI UDR 19 8 2026 1",
            },
            .command_count = 8,
            .expected_ok = 1,
            .expected_reject = 7,
            .verify_command = "DETAIL CF106",
            .verify_substr = "seats:0/1",
            .verify_unique_booking_ids = 1,
            .expected_id_prefix = "CF106-BK",
        },
        {
            .name = "06_per_flight_booking_id_namespaces",
            .description =
                "Two flights booked concurrently should each assign BK0001 independently.\n"
                "One client books CF107, another books CF108 at the same time.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF107 PAT RPR 20 8 2026 3 900.00",
                "ADMIN_CREATE_FLIGHT admin admin123 CF108 BLR MUM 20 8 2026 3 900.00",
                "SIGNUP user01 pass01",
                "SIGNUP user02 pass02",
                NULL,
            },
            .commands = {
                "BOOK user01 PAT RPR 20 8 2026 1",
                "BOOK user02 BLR MUM 20 8 2026 1",
            },
            .command_count = 2,
            .expected_ok = 2,
            .expected_reject = 0,
            .verify_command = "DETAIL CF107",
            .verify_substr = "seats:2/3",
            .verify_command_b = "DETAIL CF108",
            .verify_substr_b = "seats:2/3",
            .required_ok_substrings = {"CF107-BK0001", "CF108-BK0001", NULL},
            .verify_unique_booking_ids = 0,
            .expected_id_prefix = NULL,
        },
        {
            .name = "07_high_contention_max_seats",
            .description =
                "50 clients race for CF109 with 25 seats (max per flight).\n"
                "Expect exactly 25 OK and 25 ERR — strong overbooking guard.",
            .setup = {
                "ADMIN_CREATE_FLIGHT admin admin123 CF109 AMD PNQ 21 8 2026 25 750.00",
                "SIGNUP user01 pass01",
                "SIGNUP user02 pass02",
                "SIGNUP user03 pass03",
                "SIGNUP user04 pass04",
                "SIGNUP user05 pass05",
                "SIGNUP user06 pass06",
                "SIGNUP user07 pass07",
                "SIGNUP user08 pass08",
                "SIGNUP user09 pass09",
                "SIGNUP user10 pass10",
                "SIGNUP user11 pass11",
                "SIGNUP user12 pass12",
                "SIGNUP user13 pass13",
                "SIGNUP user14 pass14",
                "SIGNUP user15 pass15",
                "SIGNUP user16 pass16",
                "SIGNUP user17 pass17",
                "SIGNUP user18 pass18",
                "SIGNUP user19 pass19",
                "SIGNUP user20 pass20",
                NULL,
            },
            .commands = {
                "BOOK user01 AMD PNQ 21 8 2026 1",
                "BOOK user02 AMD PNQ 21 8 2026 1",
                "BOOK user03 AMD PNQ 21 8 2026 1",
                "BOOK user04 AMD PNQ 21 8 2026 1",
                "BOOK user05 AMD PNQ 21 8 2026 1",
                "BOOK user06 AMD PNQ 21 8 2026 1",
                "BOOK user07 AMD PNQ 21 8 2026 1",
                "BOOK user08 AMD PNQ 21 8 2026 1",
                "BOOK user09 AMD PNQ 21 8 2026 1",
                "BOOK user10 AMD PNQ 21 8 2026 1",
                "BOOK user11 AMD PNQ 21 8 2026 1",
                "BOOK user12 AMD PNQ 21 8 2026 1",
                "BOOK user13 AMD PNQ 21 8 2026 1",
                "BOOK user14 AMD PNQ 21 8 2026 1",
                "BOOK user15 AMD PNQ 21 8 2026 1",
                "BOOK user16 AMD PNQ 21 8 2026 1",
                "BOOK user17 AMD PNQ 21 8 2026 1",
                "BOOK user18 AMD PNQ 21 8 2026 1",
                "BOOK user19 AMD PNQ 21 8 2026 1",
                "BOOK user20 AMD PNQ 21 8 2026 1",
                "BOOK user01 AMD PNQ 21 8 2026 1",
                "BOOK user02 AMD PNQ 21 8 2026 1",
                "BOOK user03 AMD PNQ 21 8 2026 1",
                "BOOK user04 AMD PNQ 21 8 2026 1",
                "BOOK user05 AMD PNQ 21 8 2026 1",
                "BOOK user06 AMD PNQ 21 8 2026 1",
                "BOOK user07 AMD PNQ 21 8 2026 1",
                "BOOK user08 AMD PNQ 21 8 2026 1",
                "BOOK user09 AMD PNQ 21 8 2026 1",
                "BOOK user10 AMD PNQ 21 8 2026 1",
                "BOOK user11 AMD PNQ 21 8 2026 1",
                "BOOK user12 AMD PNQ 21 8 2026 1",
                "BOOK user13 AMD PNQ 21 8 2026 1",
                "BOOK user14 AMD PNQ 21 8 2026 1",
                "BOOK user15 AMD PNQ 21 8 2026 1",
                "BOOK user16 AMD PNQ 21 8 2026 1",
                "BOOK user17 AMD PNQ 21 8 2026 1",
                "BOOK user18 AMD PNQ 21 8 2026 1",
                "BOOK user19 AMD PNQ 21 8 2026 1",
                "BOOK user20 AMD PNQ 21 8 2026 1",
                "BOOK user01 AMD PNQ 21 8 2026 1",
                "BOOK user02 AMD PNQ 21 8 2026 1",
                "BOOK user03 AMD PNQ 21 8 2026 1",
                "BOOK user04 AMD PNQ 21 8 2026 1",
                "BOOK user05 AMD PNQ 21 8 2026 1",
                "BOOK user06 AMD PNQ 21 8 2026 1",
                "BOOK user07 AMD PNQ 21 8 2026 1",
                "BOOK user08 AMD PNQ 21 8 2026 1",
                "BOOK user09 AMD PNQ 21 8 2026 1",
                "BOOK user10 AMD PNQ 21 8 2026 1",
            },
            .command_count = 50,
            .expected_ok = 25,
            .expected_reject = 25,
            .verify_command = "DETAIL CF109",
            .verify_substr = "seats:0/25",
            .verify_unique_booking_ids = 0,
            .expected_id_prefix = NULL,
        },
    };

    size_t case_count = sizeof(cases) / sizeof(cases[0]);
    for (size_t i = 0; i < case_count; i++) {
        tests_run++;
        if (run_test_case(host, port, &cases[i])) {
            tests_passed++;
        }
    }

    printf("\n========================================\n");
    printf("Concurrent booking tests: %d/%d passed\n", tests_passed, tests_run);
    printf("========================================\n");

    return tests_passed == tests_run ? 0 : 1;
}
