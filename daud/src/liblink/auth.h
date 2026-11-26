/* auth.h -- the SASL step of the link's handshake (EXTERNAL needs none; SCRAM-SHA-256 is added in auth.c). */
#ifndef LK_AUTH_H
#define LK_AUTH_H
struct lk_auth;
void lk_auth_free(struct lk_auth *a);
#endif
