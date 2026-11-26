/* test_link.c -- the link engine alone: two engines joined by socket pairs that are cut again and again while data flows both
 * ways.  Every byte must arrive once and in order, whatever the cuts; and the cases where a session cannot continue. */
#define _GNU_SOURCE
#include "test.h"
#include "../daud/src/liblink/link.h"
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* a byte of a stream, from its offset: both ends know what should arrive */
static uint8_t pat(uint64_t off, unsigned seed) { return (uint8_t)((off * 2654435761u + seed) >> 13); }

struct side {
    lk_t     *l;
    uint64_t  sent, got, bad;
    unsigned  seed;
    uint64_t  want;                         /* bytes to send in all */
};

static void feed(struct side *s)
{
    uint8_t buf[5000];
    while (s->sent < s->want && lk_space(s->l) > 0) {
        size_t n = sizeof buf, i;
        if (n > s->want - s->sent) n = (size_t)(s->want - s->sent);
        if (n > lk_space(s->l)) n = lk_space(s->l);
        for (i = 0; i < n; i++) buf[i] = pat(s->sent + i, s->seed);
        n = lk_write(s->l, buf, n);
        if (!n) break;
        s->sent += n;
    }
}

static void drain(struct side *s, unsigned peer_seed)
{
    uint8_t buf[7000];
    size_t n, i;
    while ((n = lk_read(s->l, buf, sizeof buf)) > 0) {
        for (i = 0; i < n; i++)
            if (buf[i] != pat(s->got + i, peer_seed)) s->bad++;
        s->got += n;
    }
}

/* run both engines until nothing more happens (or LIMIT rounds), cutting the carrier every CUT rounds if CUT */
static void run(struct side *a, struct side *b, int limit, int cut, int *cuts, unsigned *eva, unsigned *evb)
{
    int round;
    for (round = 0; round < limit; round++) {
        struct pollfd p[2];
        unsigned x;
        feed(a); feed(b);
        if (cut && round % cut == cut - 1 && lk_fd(a->l) >= 0) {   /* cut: both ends lose the carrier, a new pair is made */
            int sv[2];
            lk_detach(a->l); lk_detach(b->l);
            CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
            CHECK(lk_attach(b->l, sv[1]) == 0);
            CHECK(lk_attach(a->l, sv[0]) == 0);
            (*cuts)++;
        }
        p[0].fd = lk_fd(a->l); p[0].events = lk_events(a->l);
        p[1].fd = lk_fd(b->l); p[1].events = lk_events(b->l);
        poll(p, 2, 0);
        x = lk_pump(a->l, p[0].fd >= 0 ? p[0].revents : 0); *eva |= x;
        x = lk_pump(b->l, p[1].fd >= 0 ? p[1].revents : 0); *evb |= x;
        drain(a, b->seed); drain(b, a->seed);
        if (a->got == b->want && b->got == a->want && !lk_unsent(a->l) && !lk_unsent(b->l))
            return;
    }
}

static int connect_pair(lk_t *a, lk_t *b)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) return -1;
    if (lk_attach(b, sv[1]) < 0) return -1;
    return lk_attach(a, sv[0]);
}

int main(void)
{
    struct side a = { .seed = 1 }, b = { .seed = 2 };
    unsigned eva = 0, evb = 0;
    int cuts = 0;

    /* 1. a clean carrier: handshake, then two streams of a few megabytes each way (more than the buffers hold) */
    a.l = lk_new(LK_DIALER, 1u << 20); b.l = lk_new(LK_ACCEPTOR, 1u << 20);
    a.want = b.want = 3u << 20;
    CHECK(lk_begin(a.l, "alice", "/srv/projects") == 0);
    CHECK(connect_pair(a.l, b.l) == 0);
    run(&a, &b, 200000, 0, &cuts, &eva, &evb);
    CHECK(a.got == b.want && b.got == a.want && a.bad == 0 && b.bad == 0);
    CHECK(eva & LK_EV_UP);
    CHECK((evb & LK_EV_NEW) && (evb & LK_EV_UP));
    CHECK(lk_state(a.l) == LK_S_UP && lk_state(b.l) == LK_S_UP);
    CHECK(!strcmp(lk_uname(b.l), "alice") && !strcmp(lk_aname(b.l), "/srv/projects"));
    lk_free(a.l); lk_free(b.l);

    /* 2. the carrier is cut every few rounds during the same transfer: nothing lost, nothing twice */
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
    a.seed = 3; b.seed = 4; eva = evb = 0; cuts = 0;
    a.l = lk_new(LK_DIALER, 1u << 18); b.l = lk_new(LK_ACCEPTOR, 1u << 18);
    a.want = 12u << 20; b.want = 6u << 20;
    CHECK(lk_begin(a.l, "alice", "/srv/projects") == 0);
    CHECK(connect_pair(a.l, b.l) == 0);
    run(&a, &b, 2000000, 8, &cuts, &eva, &evb);
    CHECK(cuts > 50);
    CHECK(a.got == b.want && b.got == a.want);
    CHECK(a.bad == 0 && b.bad == 0);
    CHECK(lk_state(a.l) == LK_S_UP && lk_state(b.l) == LK_S_UP);
    lk_free(a.l); lk_free(b.l);

    /* 3. a continuation of a session the acceptor does not have (it was restarted): the dialer learns the session is lost */
    {
        lk_t *d = lk_new(LK_DIALER, 0), *x = lk_new(LK_ACCEPTOR, 0), *y = lk_new(LK_ACCEPTOR, 0);
        struct side sd = { .l = d, .want = 1000 }, sx = { .l = x };
        struct side sy = { .l = y };
        unsigned e1 = 0, e2 = 0, e3 = 0, e4 = 0;
        int c = 0;
        CHECK(lk_begin(d, "alice", "/srv/p") == 0);
        CHECK(connect_pair(d, x) == 0);
        run(&sd, &sx, 100, 0, &c, &e1, &e2);
        CHECK(lk_state(d) == LK_S_UP);
        lk_detach(d); lk_detach(x);
        CHECK(connect_pair(d, y) == 0);                      /* the new acceptor has never heard of the session */
        run(&sd, &sy, 100, 0, &c, &e3, &e4);
        CHECK(e3 & LK_EV_LOST);
        CHECK(lk_state(d) == LK_S_LOST);
        CHECK(lk_write(d, "x", 1) == 0);                     /* a lost session takes no more */
        lk_free(d); lk_free(x); lk_free(y);
    }

    /* 4. a new session while the acceptor has one: reset, so that the caller starts afresh */
    {
        lk_t *d1 = lk_new(LK_DIALER, 0), *d2 = lk_new(LK_DIALER, 0), *x = lk_new(LK_ACCEPTOR, 0);
        struct side s1 = { .l = d1, .want = 100, .seed = 1 }, s2 = { .l = d2, .want = 100, .seed = 1 }, sx = { .l = x, .want = 100, .seed = 2 };
        unsigned e1 = 0, e2 = 0, e3 = 0, e4 = 0;
        int c = 0;
        CHECK(lk_begin(d1, "alice", "/srv/p") == 0 && lk_begin(d2, "alice", "/srv/p") == 0);
        CHECK(connect_pair(d1, x) == 0);
        run(&s1, &sx, 50, 0, &c, &e1, &e2);
        CHECK(e2 & LK_EV_NEW);
        lk_detach(d1); lk_detach(x);
        CHECK(connect_pair(d2, x) == 0);
        run(&s2, &sx, 50, 0, &c, &e3, &e4);
        CHECK(e4 & LK_EV_RESET);
        lk_free(d1); lk_free(d2); lk_free(x);
    }

    /* 5. the hold time: with no carrier for that long the session is lost, and not before */
    {
        lk_t *d = lk_new(LK_DIALER, 0);
        time_t now = time(NULL);
        CHECK(lk_begin(d, "alice", "/srv/p") == 0);
        CHECK(lk_tick(d, now + 5, 10) == 0);
        CHECK((lk_tick(d, now + 11, 10) & LK_EV_LOST) != 0);
        CHECK(lk_state(d) == LK_S_LOST);
        lk_free(d);
    }

    /* 6. the buffer is bounded: a writer is told how much fits */
    {
        lk_t *d = lk_new(LK_DIALER, 4096);
        char big[10000] = { 0 };
        CHECK(lk_begin(d, "alice", "/srv/p") == 0);
        CHECK(lk_write(d, big, sizeof big) == 4096);
        CHECK(lk_space(d) == 0 && lk_write(d, big, 1) == 0);
        lk_free(d);
    }
    DONE();
}
