/* test_scram.c -- SHA-256, HMAC, PBKDF2 and base64 against their published test vectors, and SCRAM-SHA-256 against the example of
 * RFC 7677 (on both the client and the server side, with the nonces and the salt fixed to the RFC's), then with random ones,
 * with a wrong secret, and with a server that does not know the secret. */
#define _GNU_SOURCE
#include "test.h"
#include "../daud/src/liblink/crypto.h"
#include "../daud/src/liblink/auth.h"
#include <string.h>

static void hex(const uint8_t *p, size_t n, char *out)
{
    size_t i;
    for (i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", p[i]);
}

static int run_pair(const char *cs, const char *ss)
{
    struct lk_auth *c = lk_auth_new(cs, 0), *s = lk_auth_new(ss, 1);
    uint8_t m[512], r[512];
    size_t mn, rn;
    int cd = 0, sd = 0, ok = 0;
    if (lk_auth_first(c, m, sizeof m, &mn) == 0 && lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == 0
        && lk_auth_step(c, r, rn, m, sizeof m, &mn, &cd) == 0 && lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == 0 && sd
        && lk_auth_step(c, r, rn, m, sizeof m, &mn, &cd) == 0 && cd)
        ok = 1;
    lk_auth_free(c);
    lk_auth_free(s);
    return ok;
}

int main(void)
{
    char h[200];
    uint8_t d[32], buf[100];
    uint8_t key20[20];
    memset(key20, 0x0b, sizeof key20);

    lk_sha256("abc", 3, d); hex(d, 32, h);
    CHECK(!strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    lk_sha256("", 0, d); hex(d, 32, h);
    CHECK(!strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    {   /* more than one block, and a message that is split across updates */
        struct lk_sha256 c;
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        lk_sha256_init(&c); lk_sha256_update(&c, m, 20); lk_sha256_update(&c, m + 20, strlen(m) - 20); lk_sha256_final(&c, d); hex(d, 32, h);
        CHECK(!strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    }
    lk_hmac_sha256(key20, 20, "Hi There", 8, d); hex(d, 32, h);
    CHECK(!strcmp(h, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"));
    lk_pbkdf2_sha256("password", 8, "salt", 4, 1, d); hex(d, 32, h);
    CHECK(!strcmp(h, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    lk_pbkdf2_sha256("password", 8, "salt", 4, 2, d); hex(d, 32, h);
    CHECK(!strcmp(h, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"));
    {
        char o[32];
        CHECK(lk_b64_encode("foobar", 6, o, sizeof o) == 8 && !strcmp(o, "Zm9vYmFy"));
        CHECK(lk_b64_encode("fo", 2, o, sizeof o) == 4 && !strcmp(o, "Zm8="));
        CHECK(lk_b64_decode("Zm9vYmE=", 8, buf, sizeof buf) == 5 && !memcmp(buf, "fooba", 5));
        CHECK(lk_b64_decode("Zm9v!mE=", 8, buf, sizeof buf) == -1);
        CHECK(lk_b64_decode("Zm9", 3, buf, sizeof buf) == -1);
    }

    /* RFC 7677 section 3: user "user", password "pencil" */
    {
        const char *snonce = "%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
        const char *first = "n,,n=user,r=rOprNGfwEbeRWgbNEkqO";
        const char *sfirst = "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";
        const char *cfinal = "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=";
        const char *sfinal = "v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=";
        uint8_t salt[16], out[512];
        size_t on;
        int done = 0;
        struct lk_auth *c = lk_auth_new("pencil", 0), *s = lk_auth_new("pencil", 1);

        /* the client */
        lk_auth_fix(c, "user", "rOprNGfwEbeRWgbNEkqO", NULL, 0);
        CHECK(lk_auth_first(c, out, sizeof out, &on) == 0 && on == strlen(first) && !memcmp(out, first, on));
        CHECK(lk_auth_step(c, (const uint8_t *)sfirst, strlen(sfirst), out, sizeof out, &on, &done) == 0 && !done);
        CHECK(on == strlen(cfinal) && !memcmp(out, cfinal, on));
        CHECK(lk_auth_step(c, (const uint8_t *)sfinal, strlen(sfinal), out, sizeof out, &on, &done) == 0 && done);
        lk_auth_free(c);

        /* the server */
        CHECK(lk_b64_decode("W22ZaJ0SNY7soEsUEjb6gQ==", 24, salt, sizeof salt) == 16);
        lk_auth_fix(s, NULL, snonce, salt, 16);
        done = 0;
        CHECK(lk_auth_step(s, (const uint8_t *)first, strlen(first), out, sizeof out, &on, &done) == 0 && !done);
        CHECK(on == strlen(sfirst) && !memcmp(out, sfirst, on));
        CHECK(lk_auth_step(s, (const uint8_t *)cfinal, strlen(cfinal), out, sizeof out, &on, &done) == 0 && done);
        CHECK(on == strlen(sfinal) && !memcmp(out, sfinal, on));
        lk_auth_free(s);
    }

    /* random nonces and salt, the same secret: it works; a different secret or a tampered message: it does not */
    CHECK(run_pair("a secret of any length", "a secret of any length"));
    CHECK(!run_pair("a secret of any length", "a different secret"));
    {
        struct lk_auth *c = lk_auth_new("s", 0), *s = lk_auth_new("s", 1);
        uint8_t m[512], r[512];
        size_t mn, rn;
        int cd = 0, sd = 0;
        CHECK(lk_auth_first(c, m, sizeof m, &mn) == 0 && lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == 0);
        CHECK(lk_auth_step(c, r, rn, m, sizeof m, &mn, &cd) == 0);
        m[mn - 3] ^= 1;                                          /* the proof is altered in transit */
        CHECK(lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == -1 && !sd);
        lk_auth_free(c); lk_auth_free(s);
    }
    {   /* a server that does not know the secret cannot make the client's check pass */
        struct lk_auth *c = lk_auth_new("s", 0), *s = lk_auth_new("s", 1);
        uint8_t m[512], r[512];
        size_t mn, rn;
        int cd = 0, sd = 0;
        CHECK(lk_auth_first(c, m, sizeof m, &mn) == 0 && lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == 0);
        CHECK(lk_auth_step(c, r, rn, m, sizeof m, &mn, &cd) == 0);
        CHECK(lk_auth_step(s, m, mn, r, sizeof r, &rn, &sd) == 0 && sd);
        r[rn - 3] ^= 1;
        CHECK(lk_auth_step(c, r, rn, m, sizeof m, &mn, &cd) == -1 && !cd);
        lk_auth_free(c); lk_auth_free(s);
    }
    DONE();
}
