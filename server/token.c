#include "token.h"
#include "password.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void bytes_to_hex(const unsigned char *bytes, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int hex_to_bytes(const char *hex, size_t hex_len, unsigned char *out, size_t out_len) {
    if (hex_len != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int constant_time_equal(const unsigned char *a, const unsigned char *b, size_t n) {
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

int load_or_create_auth_secret(unsigned char *secret, size_t secret_len) {
    if (secret == NULL || secret_len == 0) {
        return -1;
    }

    int fd = open(AUTH_SECRET_FILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, secret, secret_len);
        close(fd);
        if (n == (ssize_t)secret_len) {
            return 0;
        }
    }

    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t got = 0;
    while (got < secret_len) {
        ssize_t n = read(fd, secret + got, secret_len - got);
        if (n <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)n;
    }
    close(fd);

    fd = open(AUTH_SECRET_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    ssize_t w = write(fd, secret, secret_len);
    close(fd);
    return w == (ssize_t)secret_len ? 0 : -1;
}

int issue_auth_token(const unsigned char *secret,
                     size_t secret_len,
                     char role,
                     const char *subject,
                     char *out,
                     size_t out_size) {
    if (secret == NULL || subject == NULL || subject[0] == '\0' || out == NULL) {
        return -1;
    }
    if (role != AUTH_ROLE_USER && role != AUTH_ROLE_ADMIN) {
        return -1;
    }

    long long exp = (long long)time(NULL) + AUTH_TOKEN_TTL_SEC;
    char body[128];
    int n = snprintf(body, sizeof(body), "%c.%s.%lld", role, subject, exp);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        return -1;
    }

    unsigned char mac[SHA256_DIGEST_LEN];
    crypto_hmac_sha256(secret, secret_len, (const unsigned char *)body, strlen(body), mac);

    char mac_hex[SHA256_DIGEST_LEN * 2 + 1];
    bytes_to_hex(mac, SHA256_DIGEST_LEN, mac_hex);

    n = snprintf(out, out_size, "%s.%s", body, mac_hex);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return 0;
}

int verify_auth_token(const unsigned char *secret,
                      size_t secret_len,
                      const char *token,
                      char required_role,
                      char *subject,
                      size_t subject_size,
                      char *role_out) {
    if (secret == NULL || token == NULL || token[0] == '\0' || subject == NULL || subject_size == 0) {
        return -1;
    }

    const char *last_dot = strrchr(token, '.');
    if (last_dot == NULL || last_dot == token) {
        return -1;
    }
    const char *second = last_dot - 1;
    while (second > token && *second != '.') {
        second--;
    }
    if (second <= token || *second != '.') {
        return -1;
    }

    size_t body_len = (size_t)(last_dot - token);
    const char *mac_hex = last_dot + 1;
    if (strlen(mac_hex) != (size_t)SHA256_DIGEST_LEN * 2) {
        return -1;
    }

    unsigned char expected[SHA256_DIGEST_LEN];
    unsigned char actual[SHA256_DIGEST_LEN];
    if (hex_to_bytes(mac_hex, SHA256_DIGEST_LEN * 2, expected, sizeof(expected)) != 0) {
        return -1;
    }
    crypto_hmac_sha256(secret, secret_len, (const unsigned char *)token, body_len, actual);
    if (!constant_time_equal(expected, actual, SHA256_DIGEST_LEN)) {
        return -1;
    }

    if (token[1] != '.') {
        return -1;
    }
    char role = token[0];
    if (role != AUTH_ROLE_USER && role != AUTH_ROLE_ADMIN) {
        return -1;
    }
    if (required_role != 0 && role != required_role) {
        return -1;
    }

    const char *exp_str = second + 1;
    char *end = NULL;
    long long exp = strtoll(exp_str, &end, 10);
    if (end != last_dot || exp <= 0) {
        return -1;
    }
    if ((long long)time(NULL) >= exp) {
        return -1;
    }

    const char *id = token + 2;
    size_t id_len = (size_t)(second - id);
    if (id_len == 0 || id_len >= subject_size) {
        return -1;
    }
    memcpy(subject, id, id_len);
    subject[id_len] = '\0';
    if (role_out != NULL) {
        *role_out = role;
    }
    return 0;
}
