/* auth.h -- the SASL step of the link's handshake: SCRAM-SHA-256 (RFC 5802, RFC 7677) with the secret as the password, no
 * channel binding, and the server proves itself to the client as well (so each end knows the other has the secret).  EXTERNAL
 * needs no steps: the carrier is trusted.  The messages go in AUTH frames. */
#ifndef LK_AUTH_H
#define LK_AUTH_H
#include <stddef.h>
#include <stdint.h>

struct lk_auth;

/* SERVER: this end checks the proof (the acceptor); otherwise it makes it (the dialer) */
struct lk_auth *lk_auth_new(const char *secret, int server);
void lk_auth_free(struct lk_auth *a);

/* the client's first message */
int lk_auth_first(struct lk_auth *a, uint8_t *out, size_t outsz, size_t *outn);
/* one message in, the next (if any) out; *done is set when the exchange has succeeded.  0, or -1 when it failed */
int lk_auth_step(struct lk_auth *a, const uint8_t *in, size_t n, uint8_t *out, size_t outsz, size_t *outn, int *done);

/* for tests only: use these in place of random values (NULL keeps the random one) */
void lk_auth_fix(struct lk_auth *a, const char *user, const char *nonce, const uint8_t *salt, size_t saltlen);

#endif
