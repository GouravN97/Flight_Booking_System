#include "password.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHA256_BLOCK 64
#define SHA256_DIGEST 32
#define PASSWORD_SALT_LEN 16

static const uint32_t k_sha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
}

static void sha256_transform(uint32_t state[8], const uint8_t block[SHA256_BLOCK]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + k_sha256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void crypto_sha256(const unsigned char *data, size_t len, unsigned char out[SHA256_DIGEST_LEN]) {
    uint32_t state[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    uint8_t block[SHA256_BLOCK];
    size_t offset = 0;
    uint64_t bit_len = (uint64_t)len * 8u;

    while (len - offset >= SHA256_BLOCK) {
        memcpy(block, data + offset, SHA256_BLOCK);
        sha256_transform(state, block);
        offset += SHA256_BLOCK;
    }

    size_t rem = len - offset;
    memcpy(block, data + offset, rem);
    block[rem] = 0x80;
    if (rem + 1 > 56) {
        memset(block + rem + 1, 0, SHA256_BLOCK - rem - 1);
        sha256_transform(state, block);
        memset(block, 0, SHA256_BLOCK);
    } else {
        memset(block + rem + 1, 0, 56 - rem - 1);
    }

    for (int i = 0; i < 8; i++) {
        block[56 + i] = (uint8_t)(bit_len >> (56 - 8 * i));
    }
    sha256_transform(state, block);

    for (int i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)state[i];
    }
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

static void bytes_to_hex(const uint8_t *bytes, size_t n, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int hex_to_bytes(const char *hex, size_t hex_len, uint8_t *out, size_t out_len) {
    if (hex_len != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int fill_random(uint8_t *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) {
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

static int constant_time_equal(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static int sha256_salted(const uint8_t *salt, size_t salt_len, const char *password, uint8_t digest[SHA256_DIGEST]) {
    size_t pw_len = strlen(password);
    uint8_t *combined = malloc(salt_len + pw_len);
    if (combined == NULL) {
        return -1;
    }
    memcpy(combined, salt, salt_len);
    memcpy(combined + salt_len, password, pw_len);
    crypto_sha256(combined, salt_len + pw_len, digest);
    memset(combined, 0, salt_len + pw_len);
    free(combined);
    return 0;
}

int hash_password(const char *password, char *out, size_t out_size) {
    if (password == NULL || password[0] == '\0' || out == NULL || out_size < PASSWORD_HASH_SIZE) {
        return -1;
    }

    uint8_t salt[PASSWORD_SALT_LEN];
    uint8_t digest[SHA256_DIGEST];
    if (fill_random(salt, sizeof(salt)) != 0) {
        return -1;
    }
    if (sha256_salted(salt, sizeof(salt), password, digest) != 0) {
        return -1;
    }

    char salt_hex[PASSWORD_SALT_LEN * 2 + 1];
    char hash_hex[SHA256_DIGEST * 2 + 1];
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    bytes_to_hex(digest, sizeof(digest), hash_hex);

    int n = snprintf(out, out_size, "sha256$%s$%s", salt_hex, hash_hex);
    if (n < 0 || (size_t)n >= out_size) {
        return -1;
    }
    return 0;
}

int verify_password(const char *password, const char *stored) {
    if (password == NULL || stored == NULL || stored[0] == '\0') {
        return -1;
    }

    const char prefix[] = "sha256$";
    if (strncmp(stored, prefix, 7) != 0) {
        return -1;
    }

    const char *salt_hex = stored + 7;
    const char *dollar = strchr(salt_hex, '$');
    if (dollar == NULL) {
        return -1;
    }
    size_t salt_hex_len = (size_t)(dollar - salt_hex);
    const char *hash_hex = dollar + 1;
    size_t hash_hex_len = strlen(hash_hex);
    if (hash_hex_len > 0 && hash_hex[hash_hex_len - 1] == '\n') {
        hash_hex_len--;
    }

    uint8_t salt[PASSWORD_SALT_LEN];
    uint8_t expected[SHA256_DIGEST];
    uint8_t actual[SHA256_DIGEST];
    if (hex_to_bytes(salt_hex, salt_hex_len, salt, sizeof(salt)) != 0) {
        return -1;
    }
    if (hex_to_bytes(hash_hex, hash_hex_len, expected, sizeof(expected)) != 0) {
        return -1;
    }

    if (sha256_salted(salt, sizeof(salt), password, actual) != 0) {
        return -1;
    }
    return constant_time_equal(expected, actual, SHA256_DIGEST) ? 0 : -1;
}

void crypto_hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *msg, size_t msg_len,
                        unsigned char out[SHA256_DIGEST_LEN]) {
    unsigned char keyblock[SHA256_BLOCK];
    unsigned char k_ipad[SHA256_BLOCK];
    unsigned char k_opad[SHA256_BLOCK];
    unsigned char inner_hash[SHA256_DIGEST];
    unsigned char *inner;

    memset(keyblock, 0, sizeof(keyblock));
    if (key_len > SHA256_BLOCK) {
        crypto_sha256(key, key_len, keyblock);
    } else {
        memcpy(keyblock, key, key_len);
    }

    for (int i = 0; i < SHA256_BLOCK; i++) {
        k_ipad[i] = (unsigned char)(keyblock[i] ^ 0x36);
        k_opad[i] = (unsigned char)(keyblock[i] ^ 0x5c);
    }

    inner = malloc(SHA256_BLOCK + msg_len);
    if (inner == NULL) {
        memset(out, 0, SHA256_DIGEST_LEN);
        return;
    }
    memcpy(inner, k_ipad, SHA256_BLOCK);
    if (msg_len > 0 && msg != NULL) {
        memcpy(inner + SHA256_BLOCK, msg, msg_len);
    }
    crypto_sha256(inner, SHA256_BLOCK + msg_len, inner_hash);
    memset(inner, 0, SHA256_BLOCK + msg_len);
    free(inner);

    unsigned char outer[SHA256_BLOCK + SHA256_DIGEST];
    memcpy(outer, k_opad, SHA256_BLOCK);
    memcpy(outer + SHA256_BLOCK, inner_hash, SHA256_DIGEST);
    crypto_sha256(outer, sizeof(outer), out);
}
