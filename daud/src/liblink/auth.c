/* auth.c -- SCRAM-SHA-256 for the link's handshake; see auth.h.
 *
 *   client-first   n,,n=USER,r=CNONCE
 *   server-first   r=CNONCE+SNONCE,s=BASE64(SALT),i=ITERATIONS
 *   client-final   c=biws,r=CNONCE+SNONCE,p=BASE64(PROOF)
 *   server-final   v=BASE64(SERVER-SIGNATURE)
 * with  SaltedPassword = PBKDF2(secret, SALT, i), ClientKey = HMAC(SaltedPassword, "Client Key"), StoredKey = H(ClientKey),
 * AuthMessage = client-first-bare "," server-first "," client-final-without-proof, ClientSignature = HMAC(StoredKey, AuthMessage),
 * ClientProof = ClientKey xor ClientSignature, ServerSignature = HMAC(HMAC(SaltedPassword, "Server Key"), AuthMessage). */
#define _GNU_SOURCE
#include "auth.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

#define ITER 4096
#define MAXITER 1000000
#define MSG 512

struct lk_auth {
    int      server, state;
    char    *secret;
    char     user[64];
    char     fixed_nonce[64];
    uint8_t  fixed_salt[32];
    size_t   fixed_saltlen;
    char     cnonce[64], nonce[160];
    char     first_bare[MSG], server_first[MSG];
    uint8_t  stored[LK_SHA256_LEN], client_key[LK_SHA256_LEN], server_sig[LK_SHA256_LEN], server_key[LK_SHA256_LEN];
};

struct lk_auth *lk_auth_new(const char *secret, int server)
{
    struct lk_auth *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->server = server;
    a->secret = strdup(secret);
    snprintf(a->user, sizeof a->user, "9share");
    if (!a->secret) {
        free(a);
        return NULL;
    }
    return a;
}

void lk_auth_free(struct lk_auth *a)
{
    if (!a)
        return;
    memset(a->secret, 0, strlen(a->secret));
    free(a->secret);
    memset(a, 0, sizeof *a);
    free(a);
}

void lk_auth_fix(struct lk_auth *a, const char *user, const char *nonce, const uint8_t *salt, size_t saltlen)
{
    if (user)
        snprintf(a->user, sizeof a->user, "%s", user);
    if (nonce)
        snprintf(a->fixed_nonce, sizeof a->fixed_nonce, "%s", nonce);
    if (salt && saltlen <= sizeof a->fixed_salt) {
        memcpy(a->fixed_salt, salt, saltlen);
        a->fixed_saltlen = saltlen;
    }
}

static int random_nonce(struct lk_auth *a, char *out, size_t outsz)
{
    uint8_t r[18];
    if (a->fixed_nonce[0]) {
        snprintf(out, outsz, "%s", a->fixed_nonce);
        return 0;
    }
    if (getrandom(r, sizeof r, 0) != (ssize_t)sizeof r)
        return -1;
    return lk_b64_encode(r, sizeof r, out, outsz) ? 0 : -1;
}

/* the value of attribute KEY ("r", "s", "i", "p", "v", "c") in a comma-separated message; 0 if it is not there */
static int attr(const char *msg, char key, char *out, size_t outsz)
{
    const char *p = msg;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n >= 2 && p[0] == key && p[1] == '=') {
            if (n - 2 >= outsz)
                return 0;
            memcpy(out, p + 2, n - 2);
            out[n - 2] = '\0';
            return 1;
        }
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

/* the keys and signatures from the password and the three messages */
static void derive(struct lk_auth *a, const uint8_t *salt, size_t slen, unsigned iter)
{
    uint8_t salted[LK_SHA256_LEN];
    lk_pbkdf2_sha256(a->secret, strlen(a->secret), salt, slen, iter, salted);
    lk_hmac_sha256(salted, sizeof salted, "Client Key", 10, a->client_key);
    lk_sha256(a->client_key, sizeof a->client_key, a->stored);
    lk_hmac_sha256(salted, sizeof salted, "Server Key", 10, a->server_key);
    memset(salted, 0, sizeof salted);
}

int lk_auth_first(struct lk_auth *a, uint8_t *out, size_t outsz, size_t *outn)
{
    int n;
    if (a->server || a->state != 0 || random_nonce(a, a->cnonce, sizeof a->cnonce) < 0)
        return -1;
    snprintf(a->first_bare, sizeof a->first_bare, "n=%s,r=%s", a->user, a->cnonce);
    n = snprintf((char *)out, outsz, "n,,%s", a->first_bare);
    if (n < 0 || (size_t)n >= outsz)
        return -1;
    *outn = (size_t)n;
    a->state = 1;
    return 0;
}

int lk_auth_step(struct lk_auth *a, const uint8_t *in, size_t n, uint8_t *out, size_t outsz, size_t *outn, int *done)
{
    char msg[MSG], r[160], s[128], i[16], p[128], v[128], c[16];
    uint8_t salt[64], proof[LK_SHA256_LEN], sig[LK_SHA256_LEN];
    char cfwp[MSG], authmsg[3 * MSG], b64[96];
    long slen;
    unsigned iter;
    int len;

    *outn = 0;
    *done = 0;
    if (n == 0 || n >= sizeof msg)
        return -1;
    memcpy(msg, in, n);
    msg[n] = '\0';

    if (!a->server && a->state == 1) {                              /* client: the server-first message */
        size_t k, j;
        if (!attr(msg, 'r', r, sizeof r) || !attr(msg, 's', s, sizeof s) || !attr(msg, 'i', i, sizeof i))
            return -1;
        if (strncmp(r, a->cnonce, strlen(a->cnonce)) != 0 || strlen(r) <= strlen(a->cnonce))
            return -1;                                              /* the server's nonce must extend ours */
        iter = (unsigned)strtoul(i, NULL, 10);
        if (iter < 1 || iter > MAXITER)
            return -1;
        if ((slen = lk_b64_decode(s, strlen(s), salt, sizeof salt)) < 1)
            return -1;
        snprintf(a->server_first, sizeof a->server_first, "%s", msg);
        derive(a, salt, (size_t)slen, iter);
        snprintf(cfwp, sizeof cfwp, "c=biws,r=%s", r);
        len = snprintf(authmsg, sizeof authmsg, "%s,%s,%s", a->first_bare, a->server_first, cfwp);
        if (len < 0 || (size_t)len >= sizeof authmsg)
            return -1;
        lk_hmac_sha256(a->stored, sizeof a->stored, authmsg, (size_t)len, sig);
        for (k = 0; k < LK_SHA256_LEN; k++)
            proof[k] = a->client_key[k] ^ sig[k];
        lk_hmac_sha256(a->server_key, sizeof a->server_key, authmsg, (size_t)len, a->server_sig);
        if (!lk_b64_encode(proof, sizeof proof, b64, sizeof b64))
            return -1;
        j = (size_t)snprintf((char *)out, outsz, "%s,p=%s", cfwp, b64);
        if (j >= outsz)
            return -1;
        *outn = j;
        a->state = 2;
        return 0;
    }
    if (!a->server && a->state == 2) {                              /* client: the server-final message */
        uint8_t got[LK_SHA256_LEN];
        if (!attr(msg, 'v', v, sizeof v) || lk_b64_decode(v, strlen(v), got, sizeof got) != LK_SHA256_LEN)
            return -1;
        if (!lk_ct_equal(got, a->server_sig, LK_SHA256_LEN))
            return -1;
        a->state = 3;
        *done = 1;
        return 0;
    }
    if (a->server && a->state == 0) {                               /* server: the client-first message */
        char cn[64], sn[64];
        uint8_t rnd[16];
        if (strncmp(msg, "n,,", 3) != 0)
            return -1;
        snprintf(a->first_bare, sizeof a->first_bare, "%s", msg + 3);
        if (!attr(a->first_bare, 'r', cn, sizeof cn) || strlen(cn) < 8)
            return -1;
        if (random_nonce(a, sn, sizeof sn) < 0)
            return -1;
        if (a->fixed_nonce[0])                                      /* a test fixed the server's nonce: it is the whole addition */
            snprintf(sn, sizeof sn, "%s", a->fixed_nonce);
        snprintf(a->nonce, sizeof a->nonce, "%s%s", cn, sn);
        if (a->fixed_saltlen) {
            memcpy(salt, a->fixed_salt, a->fixed_saltlen);
            slen = (long)a->fixed_saltlen;
        } else {
            if (getrandom(rnd, sizeof rnd, 0) != (ssize_t)sizeof rnd)
                return -1;
            memcpy(salt, rnd, sizeof rnd);
            slen = sizeof rnd;
        }
        if (!lk_b64_encode(salt, (size_t)slen, b64, sizeof b64))
            return -1;
        len = snprintf(a->server_first, sizeof a->server_first, "r=%s,s=%s,i=%d", a->nonce, b64, ITER);
        if (len < 0 || (size_t)len >= outsz)
            return -1;
        memcpy(out, a->server_first, (size_t)len);
        *outn = (size_t)len;
        derive(a, salt, (size_t)slen, ITER);
        a->state = 1;
        return 0;
    }
    if (a->server && a->state == 1) {                               /* server: the client-final message */
        uint8_t pr[LK_SHA256_LEN], ck[LK_SHA256_LEN], sk[LK_SHA256_LEN];
        const char *pp;
        size_t k;
        if (!attr(msg, 'c', c, sizeof c) || strcmp(c, "biws") != 0 || !attr(msg, 'r', r, sizeof r) || strcmp(r, a->nonce) != 0
            || !attr(msg, 'p', p, sizeof p) || lk_b64_decode(p, strlen(p), pr, sizeof pr) != LK_SHA256_LEN)
            return -1;
        if (!(pp = strstr(msg, ",p=")))
            return -1;
        snprintf(cfwp, sizeof cfwp, "%.*s", (int)(pp - msg), msg);
        len = snprintf(authmsg, sizeof authmsg, "%s,%s,%s", a->first_bare, a->server_first, cfwp);
        if (len < 0 || (size_t)len >= sizeof authmsg)
            return -1;
        lk_hmac_sha256(a->stored, sizeof a->stored, authmsg, (size_t)len, sig);
        for (k = 0; k < LK_SHA256_LEN; k++)
            ck[k] = pr[k] ^ sig[k];
        lk_sha256(ck, sizeof ck, sk);
        if (!lk_ct_equal(sk, a->stored, LK_SHA256_LEN))
            return -1;                                              /* the proof does not fit the secret */
        lk_hmac_sha256(a->server_key, sizeof a->server_key, authmsg, (size_t)len, sig);
        if (!lk_b64_encode(sig, sizeof sig, b64, sizeof b64))
            return -1;
        len = snprintf((char *)out, outsz, "v=%s", b64);
        if (len < 0 || (size_t)len >= outsz)
            return -1;
        *outn = (size_t)len;
        a->state = 3;
        *done = 1;
        return 0;
    }
    return -1;
}
