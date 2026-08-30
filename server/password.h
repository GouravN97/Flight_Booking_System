#ifndef PASSWORD_H
#define PASSWORD_H

#include <stddef.h>

#define PASSWORD_HASH_SIZE 128
#define SHA256_DIGEST_LEN 32

int hash_password(const char *password, char *out, size_t out_size);
int verify_password(const char *password, const char *stored);
void crypto_sha256(const unsigned char *data, size_t len, unsigned char out[SHA256_DIGEST_LEN]);
void crypto_hmac_sha256(const unsigned char *key, size_t key_len,
                        const unsigned char *msg, size_t msg_len,
                        unsigned char out[SHA256_DIGEST_LEN]);

#endif
