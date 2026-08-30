#ifndef TOKEN_H
#define TOKEN_H

#include <stddef.h>

#define AUTH_TOKEN_SIZE 192
#define AUTH_TOKEN_TTL_SEC 86400
#define AUTH_SECRET_LEN 32
#define AUTH_SECRET_FILE "auth.secret"
#define AUTH_ROLE_USER 'u'
#define AUTH_ROLE_ADMIN 'a'

int load_or_create_auth_secret(unsigned char *secret, size_t secret_len);
int issue_auth_token(const unsigned char *secret,
                     size_t secret_len,
                     char role,
                     const char *subject,
                     char *out,
                     size_t out_size);
int verify_auth_token(const unsigned char *secret,
                      size_t secret_len,
                      const char *token,
                      char required_role,
                      char *subject,
                      size_t subject_size,
                      char *role_out);

#endif
