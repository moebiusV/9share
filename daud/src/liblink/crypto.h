/* crypto.h -- the little that SCRAM-SHA-256 needs: SHA-256, HMAC, PBKDF2 and base64 (checked against the published vectors in
 * tests/test_scram.c).  Not for general use. */
#ifndef LK_CRYPTO_H
#define LK_CRYPTO_H
#include <stddef.h>
#include <stdint.h>

#define LK_SHA256_LEN 32

struct lk_sha256 { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t len; };

void lk_sha256_init(struct lk_sha256 *c);
void lk_sha256_update(struct lk_sha256 *c, const void *data, size_t n);
void lk_sha256_final(struct lk_sha256 *c, uint8_t out[LK_SHA256_LEN]);
void lk_sha256(const void *data, size_t n, uint8_t out[LK_SHA256_LEN]);
void lk_hmac_sha256(const void *key, size_t klen, const void *data, size_t n, uint8_t out[LK_SHA256_LEN]);
void lk_pbkdf2_sha256(const void *pw, size_t pwlen, const void *salt, size_t slen, unsigned iter, uint8_t out[LK_SHA256_LEN]);
size_t lk_b64_encode(const void *in, size_t n, char *out, size_t outsz);    /* NUL-terminated; returns the length, 0 if it does not fit */
long   lk_b64_decode(const char *in, size_t n, uint8_t *out, size_t outsz); /* the length, or -1 */
int    lk_ct_equal(const void *a, const void *b, size_t n);                 /* 1 if equal, in constant time */
#endif
