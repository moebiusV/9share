/* test_proxy.c -- the 9P-aware part of the relay (src/proxy.c): a fake kernel on one side, a fake server on the other, and the state machine in
 * between.  No sockets, no link, no root.  What is checked: that whatever is not understood passes unchanged; that tags are mapped both ways and
 * never collide; that a file read in order is read ahead (as deep as scp, 64 requests or 2 MiB) and the kernel's reads are answered from it; that
 * nothing is invented (a failed read-ahead is never shown, a block is used once); that every change through the relay drops what was read ahead;
 * and that broken input is refused or passed on, never followed off the end of a buffer. */
#include "test.h"
#include "../src/proxy.h"
#include <stdlib.h>
#include <string.h>

#define FILEQ     77            /* the qid.path of the file the fake server has */
#define OTHERQ    78
#define MSIZE     1048576u

struct m { uint8_t b[270000]; size_t n; };

static void put8(struct m *m, uint8_t v) { m->b[m->n++] = v; }
static void put16(struct m *m, uint16_t v) { put8(m, v & 255); put8(m, (uint8_t)(v >> 8)); }
static void put32(struct m *m, uint32_t v) { put16(m, v & 0xFFFF); put16(m, (uint16_t)(v >> 16)); }
static void put64(struct m *m, uint64_t v) { put32(m, (uint32_t)v); put32(m, (uint32_t)(v >> 32)); }
static void puts9(struct m *m, const char *s) { size_t l = strlen(s); put16(m, (uint16_t)l); memcpy(m->b + m->n, s, l); m->n += l; }
static void mstart(struct m *m, uint8_t type, uint16_t tag) { m->n = 0; put32(m, 0); put8(m, type); put16(m, tag); }
static void mend(struct m *m) { uint32_t s = (uint32_t)m->n; m->b[0] = s & 255; m->b[1] = (s >> 8) & 255; m->b[2] = (s >> 16) & 255; m->b[3] = (uint8_t)(s >> 24); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }
static uint8_t pat(uint64_t i) { return (uint8_t)((i * 2654435761u) >> 11); }
static void qid(struct m *m, uint8_t type, uint64_t path) { put8(m, type); put32(m, 0); put64(m, path); }

enum { Tversion = 100, Tattach = 104, Twalk = 110, Tlopen = 12, Tlcreate = 14, Tread = 116, Twrite = 118, Tclunk = 120, Tflush = 108,
       Tsetattr = 26, Tfsync = 50, Tunlinkat = 76, Tgetattr = 24, Tremove = 122,
       Rlerror = 7, Rversion = 101, Rattach = 105, Rwalk = 111, Rlopen = 13, Rread = 117, Rwrite = 119, Rclunk = 121, Rflush = 109,
       Rsetattr = 27, Rfsync = 51, Rgetattr = 25, Rlcreate = 15, Runlinkat = 77 };

/* ---- the fake server: what px sent it, kept until the test answers ---- */
struct sreq { uint8_t type; uint16_t tag; uint32_t fid, newfid, count; uint64_t off; uint16_t oldtag, nw; int done; };
static struct sreq srv[8192];
static int nsrv;
static uint64_t filesize = 100u * 1024 * 1024;

static int next_msg(struct px_buf *q, struct m *out)
{
    uint32_t sz;
    if (px_buf_avail(q) < 7)
        return 0;
    sz = g32(px_buf_data(q));
    if (sz < 7 || sz > sizeof out->b || px_buf_avail(q) < sz)
        return 0;
    memcpy(out->b, px_buf_data(q), sz);
    out->n = sz;
    px_buf_consume(q, sz);
    return 1;
}

static void srv_pull(px_t *x)
{
    struct m m;
    while (next_msg(px_to_server(x), &m)) {
        struct sreq *r = &srv[nsrv++];
        memset(r, 0, sizeof *r);
        r->type = m.b[4];
        r->tag = g16(m.b + 5);
        if (r->type == Tread || r->type == Twrite) { r->fid = g32(m.b + 7); r->off = g64(m.b + 11); r->count = g32(m.b + 19); }
        else if (r->type == Twalk) { r->fid = g32(m.b + 7); r->newfid = g32(m.b + 11); r->nw = g16(m.b + 15); }
        else if (r->type == Tflush) r->oldtag = g16(m.b + 7);
        else if (m.n >= 11) r->fid = g32(m.b + 7);
    }
}

static int srv_reply(px_t *x, struct sreq *r, int err)
{
    struct m m;
    unsigned i;
    r->done = 1;
    if (err) { mstart(&m, Rlerror, r->tag); put32(&m, (uint32_t)err); mend(&m); return px_from_server(x, m.b, m.n); }
    switch (r->type) {
    case Tversion: mstart(&m, Rversion, r->tag); put32(&m, MSIZE); puts9(&m, "9P2000.L"); break;
    case Tattach:  mstart(&m, Rattach, r->tag); qid(&m, 0x80, 1); break;
    case Twalk: {                                              /* at most 16 names are walked (MAXWELEM): a longer walk is answered short */
        unsigned nq = r->nw > 16 ? 16 : r->nw;
        mstart(&m, Rwalk, r->tag); put16(&m, (uint16_t)nq);
        for (i = 0; i < nq; i++) qid(&m, 0, r->newfid >= 500 ? OTHERQ : FILEQ);
        break;
    }
    case Tlopen:   mstart(&m, Rlopen, r->tag); qid(&m, 0, FILEQ); put32(&m, 0); break;
    case Tlcreate: mstart(&m, Rlcreate, r->tag); qid(&m, 0, 99); put32(&m, 0); break;
    case Tread: {
        uint64_t n = r->off >= filesize ? 0 : filesize - r->off;
        uint32_t c = n < r->count ? (uint32_t)n : r->count;
        if (c > 250000) c = 250000;                              /* a real server answers no more than a message holds */
        mstart(&m, Rread, r->tag); put32(&m, c);
        for (i = 0; i < c; i++) put8(&m, pat(r->off + i));
        break;
    }
    case Twrite:   mstart(&m, Rwrite, r->tag); put32(&m, r->count); break;
    case Tclunk:   mstart(&m, Rclunk, r->tag); break;
    case Tsetattr: mstart(&m, Rsetattr, r->tag); break;
    case Tfsync:   mstart(&m, Rfsync, r->tag); break;
    case Tflush:   mstart(&m, Rflush, r->tag); break;
    case Tunlinkat: mstart(&m, Runlinkat, r->tag); break;
    default:       mstart(&m, Rlerror, r->tag); put32(&m, 38); break;
    }
    mend(&m);
    return px_from_server(x, m.b, m.n);
}

static void srv_answer_all(px_t *x)
{
    int i;
    srv_pull(x);
    for (i = 0; i < nsrv; i++)
        if (!srv[i].done)
            srv_reply(x, &srv[i], 0);
}

static int srv_pending_reads(void)
{
    int i, n = 0;
    for (i = 0; i < nsrv; i++)
        if (!srv[i].done && srv[i].type == Tread)
            n++;
    return n;
}

static struct sreq *srv_find_read(uint64_t off)
{
    int i;
    for (i = 0; i < nsrv; i++)
        if (srv[i].type == Tread && srv[i].off == off)
            return &srv[i];
    return NULL;
}

static void srv_clear(void) { nsrv = 0; }

/* ---- the fake kernel ---- */
static int ksend(px_t *x, struct m *m) { mend(m); return px_from_kernel(x, m->b, m->n); }

static void k_version(px_t *x) { struct m m; mstart(&m, Tversion, 0xFFFF); put32(&m, MSIZE); puts9(&m, "9P2000.L"); ksend(x, &m); }
static void k_attach(px_t *x, uint16_t tag, uint32_t fid) { struct m m; mstart(&m, Tattach, tag); put32(&m, fid); put32(&m, 0xFFFFFFFFu); puts9(&m, "u"); puts9(&m, "/"); put32(&m, 1000); ksend(x, &m); }
static void k_walk(px_t *x, uint16_t tag, uint32_t fid, uint32_t newfid, int nw) { struct m m; mstart(&m, Twalk, tag); put32(&m, fid); put32(&m, newfid); put16(&m, (uint16_t)nw); for (int i = 0; i < nw; i++) puts9(&m, "f"); ksend(x, &m); }
static void k_lopen(px_t *x, uint16_t tag, uint32_t fid, uint32_t flags) { struct m m; mstart(&m, Tlopen, tag); put32(&m, fid); put32(&m, flags); ksend(x, &m); }
static void k_read(px_t *x, uint16_t tag, uint32_t fid, uint64_t off, uint32_t count) { struct m m; mstart(&m, Tread, tag); put32(&m, fid); put64(&m, off); put32(&m, count); ksend(x, &m); }
static void k_write(px_t *x, uint16_t tag, uint32_t fid, uint64_t off, uint32_t count) { struct m m; mstart(&m, Twrite, tag); put32(&m, fid); put64(&m, off); put32(&m, count); for (uint32_t i = 0; i < count; i++) put8(&m, 'w'); ksend(x, &m); }
static void k_clunk(px_t *x, uint16_t tag, uint32_t fid) { struct m m; mstart(&m, Tclunk, tag); put32(&m, fid); ksend(x, &m); }
static void k_flush(px_t *x, uint16_t tag, uint16_t old) { struct m m; mstart(&m, Tflush, tag); put16(&m, old); ksend(x, &m); }
static void k_setattr(px_t *x, uint16_t tag, uint32_t fid) { struct m m; mstart(&m, Tsetattr, tag); put32(&m, fid); put32(&m, 8); for (int i = 0; i < 52; i++) put8(&m, 0); ksend(x, &m); }

/* what the kernel has been sent, in order */
static struct m kq[48];
static int nkq;
static void kern_pull(px_t *x) { static struct m m; while (next_msg(px_to_kernel(x), &m)) kq[nkq++ % 48] = m; }
static void kern_clear(px_t *x) { kern_pull(x); nkq = 0; }
static struct m *kern_find(uint16_t tag, uint8_t type)
{
    for (int i = 0; i < nkq && i < 48; i++)
        if (kq[i].b[4] == type && g16(kq[i].b + 5) == tag)
            return &kq[i];
    return NULL;
}
static int kern_read_ok(uint16_t tag, uint64_t off, uint32_t count)
{
    struct m *m = kern_find(tag, Rread);
    uint32_t c, i;
    if (!m)
        return 0;
    c = g32(m->b + 7);
    if (c != count || m->n != 11u + c)
        return 0;
    for (i = 0; i < c; i++)
        if (m->b[11 + i] != pat(off + i))
            return 0;
    return 1;
}

static unsigned long rnd_seed;
static uint32_t rnd(void)                       /* a function, so that two calls in one expression do not trip over each other */
{
    rnd_seed = rnd_seed * 6364136223846793005ul + 1442695040888963407ul;
    return (uint32_t)(rnd_seed >> 33);
}

static struct px_conf conf(size_t window)
{
    struct px_conf c = { window, PX_DEFAULT_REQUESTS, PX_DEFAULT_TOTAL, PX_DEFAULT_TTL };
    return c;
}

/* a fresh proxy with the file open as fid F: version, attach (fid 1), walk, lopen */
static px_t *fresh(const struct px_conf *c, uint32_t fid, uint32_t flags)
{
    px_t *x = px_new(c);
    srv_clear();
    nkq = 0;
    k_version(x); srv_answer_all(x);
    k_attach(x, 1, 1); srv_answer_all(x);
    k_walk(x, 2, 1, fid, 1); srv_answer_all(x);
    k_lopen(x, 3, fid, flags); srv_answer_all(x);
    kern_clear(x);
    srv_clear();
    return x;
}

#define COUNT 32768u

/* read blocks [first, first+n) in order, each answered before the next is asked, as the kernel does */
static void read_seq(px_t *x, uint32_t fid, int first, int n, uint16_t tag0)
{
    for (int i = first; i < first + n; i++) {
        k_read(x, (uint16_t)(tag0 + i), fid, (uint64_t)i * COUNT, COUNT);
        srv_answer_all(x);
    }
}

/* the same, but only the kernel's own read is answered each time: what was asked for ahead stays on its way */
static void answer_read_at(px_t *x, uint64_t off)
{
    int i;
    srv_pull(x);
    for (i = 0; i < nsrv; i++)
        if (!srv[i].done && srv[i].type == Tread && srv[i].off == off) { srv_reply(x, &srv[i], 0); return; }
}

static void read_seq_hold(px_t *x, uint32_t fid, int n, uint16_t tag0)
{
    for (int i = 0; i < n; i++) {
        k_read(x, (uint16_t)(tag0 + i), fid, (uint64_t)i * COUNT, COUNT);
        answer_read_at(x, (uint64_t)i * COUNT);
    }
}

int main(void)
{
    struct px_conf c = conf(PX_DEFAULT_WINDOW);
    struct px_stats st;
    px_t *x;
    struct m m;
    int i;

    /* ---- window 0: nothing is read ahead, and the kernel sees exactly its own answers ---- */
    {
        struct px_conf off = conf(0);
        x = fresh(&off, 10, 0);
        read_seq(x, 10, 0, 8, 100);
        kern_pull(x);
        CHECK(srv_pending_reads() == 0);
        { int reads = 0; for (i = 0; i < nsrv; i++) if (srv[i].type == Tread) reads++; CHECK(reads == 8); }
        CHECK(kern_read_ok(100, 0, COUNT) && kern_read_ok(107, 7ull * COUNT, COUNT));
        px_free(x);
    }

    /* ---- a Tversion with an ordinary tag (not every client sends NOTAG) is answered, with its own tag, and sets the size limit ---- */
    x = px_new(&c);
    srv_clear(); nkq = 0;
    mstart(&m, Tversion, 7); put32(&m, 65536); puts9(&m, "9P2000.L"); ksend(x, &m);
    srv_pull(x);
    CHECK(nsrv == 1 && srv[0].type == Tversion);
    { struct m rv; mstart(&rv, Rversion, srv[0].tag); put32(&rv, 65536); puts9(&rv, "9P2000.L"); mend(&rv); CHECK(px_from_server(x, rv.b, rv.n) == 0); }
    kern_pull(x);
    CHECK(kern_find(7, Rversion) != NULL);
    { uint8_t big[7] = { 0, 0, 2, 0, Tread, 1, 0 };                    /* 128 KB: now bigger than the 64 KB that was agreed */
      CHECK(px_from_kernel(x, big, sizeof big) == -1); }
    px_free(x);

    /* ---- tags: the kernel's come back as they were, whatever went upstream ---- */
    x = fresh(&c, 10, 0);
    k_read(x, 0xF000, 10, 0, COUNT);
    k_read(x, 0, 10, 5ull * COUNT, COUNT);
    k_read(x, 0xFFFE, 10, 9ull * COUNT, COUNT);
    srv_pull(x);
    CHECK(nsrv == 3 && srv[0].tag != srv[1].tag && srv[1].tag != srv[2].tag && srv[0].tag != srv[2].tag);
    CHECK(srv[0].tag != 0xFFFF && srv[1].tag != 0xFFFF && srv[2].tag != 0xFFFF);
    srv_answer_all(x);
    kern_pull(x);
    CHECK(kern_read_ok(0xF000, 0, COUNT) && kern_read_ok(0, 5ull * COUNT, COUNT) && kern_read_ok(0xFFFE, 9ull * COUNT, COUNT));
    px_free(x);

    /* ---- read-ahead starts at the third read in order, and as deep as the ramp allows ---- */
    x = fresh(&c, 10, 0);
    read_seq(x, 10, 0, 2, 100);
    CHECK(srv_pending_reads() == 0);                                   /* two in order: not yet */
    px_stats(x, &st); CHECK(st.issued == 0);
    k_read(x, 102, 10, 2ull * COUNT, COUNT);
    srv_pull(x);
    CHECK(srv_pending_reads() == 3);                                   /* the read itself and two ahead of it */
    CHECK(srv_find_read(3ull * COUNT) && srv_find_read(4ull * COUNT));
    px_stats(x, &st); CHECK(st.issued == 2);
    srv_answer_all(x);
    kern_pull(x);
    CHECK(kern_read_ok(102, 2ull * COUNT, COUNT));

    /* ---- a read that was read ahead is answered at once, and the depth grows by one per hit ---- */
    kern_clear(x); srv_clear();
    k_read(x, 103, 10, 3ull * COUNT, COUNT);
    kern_pull(x);
    CHECK(kern_read_ok(103, 3ull * COUNT, COUNT));                     /* no server needed */
    srv_pull(x);
    CHECK(srv_pending_reads() == 2);                                   /* ahead entries kept at 3: one is ready, two asked for */
    CHECK(srv_find_read(5ull * COUNT) && srv_find_read(6ull * COUNT));
    px_stats(x, &st); CHECK(st.hits == 1);

    /* ---- a read that matches one still on its way waits for it, and sends nothing of its own ---- */
    kern_clear(x);
    k_read(x, 104, 10, 4ull * COUNT, COUNT);                           /* ready: hit */
    k_read(x, 105, 10, 5ull * COUNT, COUNT);                           /* asked for, not answered */
    kern_pull(x);
    CHECK(kern_read_ok(104, 4ull * COUNT, COUNT) && !kern_find(105, Rread));
    srv_pull(x);
    { int own = 0; for (i = 0; i < nsrv; i++) if (!srv[i].done && srv[i].type == Tread && srv[i].off == 5ull * COUNT) own++; CHECK(own == 1); }  /* only the read-ahead */
    srv_answer_all(x);
    kern_pull(x);
    CHECK(kern_read_ok(105, 5ull * COUNT, COUNT));

    /* ---- a block is used once: the same range again goes to the server ---- */
    kern_clear(x); srv_clear();
    k_read(x, 106, 10, 3ull * COUNT, COUNT);                           /* seek back: not in order */
    kern_pull(x);
    CHECK(!kern_find(106, Rread));
    srv_pull(x);
    CHECK(srv_find_read(3ull * COUNT) != NULL);
    srv_answer_all(x);
    px_free(x);

    /* ---- random access never reads ahead ---- */
    x = fresh(&c, 10, 0);
    for (i = 0; i < 20; i++) { k_read(x, (uint16_t)(200 + i), 10, (uint64_t)((i * 7919) % 1000) * COUNT, COUNT); srv_answer_all(x); }
    px_stats(x, &st); CHECK(st.issued == 0);
    px_free(x);

    /* ---- a file opened for writing is never read ahead ---- */
    x = fresh(&c, 10, 2 /* O_RDWR */);
    read_seq(x, 10, 0, 10, 100);
    px_stats(x, &st); CHECK(st.issued == 0);
    px_free(x);

    /* ---- the end of the file: a short answer stops the read-ahead, and the kernel gets what the server gave ---- */
    filesize = 6ull * COUNT + 100;
    x = fresh(&c, 10, 0);
    read_seq(x, 10, 0, 3, 100);                                        /* ahead: blocks 3 and 4 */
    for (i = 3; i < 7; i++) { k_read(x, (uint16_t)(100 + i), 10, (uint64_t)i * COUNT, COUNT); srv_answer_all(x); }
    kern_pull(x);
    CHECK(kern_read_ok(106, 6ull * COUNT, 100));                       /* the short last block */
    k_read(x, 107, 10, 6ull * COUNT + 100, COUNT); srv_answer_all(x); kern_pull(x);
    { struct m *r = kern_find(107, Rread); CHECK(r && g32(r->b + 7) == 0); }   /* at the end: nothing */
    srv_clear();
    k_read(x, 108, 10, 6ull * COUNT + 100, COUNT); srv_pull(x);
    CHECK(srv_pending_reads() == 1);                                   /* and nothing is read beyond it */
    px_free(x);
    filesize = 100u * 1024 * 1024;

    /* ---- a read-ahead that fails is not shown to the kernel; the kernel's own read is made afresh ---- */
    x = fresh(&c, 10, 0);
    read_seq_hold(x, 10, 3, 100);
    srv_pull(x);                                                       /* blocks 3 and 4 asked for; answer 3 with an error */
    { struct sreq *r = srv_find_read(3ull * COUNT); CHECK(r != NULL && !r->done); if (r) srv_reply(x, r, 5); }
    kern_clear(x);
    k_read(x, 103, 10, 3ull * COUNT, COUNT);
    kern_pull(x);
    CHECK(nkq == 0);                                                   /* no Rlerror, no Rread yet */
    srv_clear(); srv_pull(x);
    CHECK(srv_find_read(3ull * COUNT) != NULL);                        /* made afresh */
    srv_answer_all(x); kern_pull(x);
    CHECK(kern_read_ok(103, 3ull * COUNT, COUNT));
    px_free(x);

    /* ---- every kind of change drops what was read ahead: the next read in order is not answered from it ---- */
    {
        enum { WRITE_SAME_FID, WRITE_OTHER_FID, SETATTR, UNLINKAT, LCREATE, TTL, NCASES };
        static const char *what[] = { "write, same fid", "write, other fid of the same file", "setattr", "unlinkat", "lcreate", "ttl" };
        for (int cs = 0; cs < NCASES; cs++) {
            x = fresh(&c, 10, 0);
            k_walk(x, 4, 1, 11, 1); srv_answer_all(x);                  /* a second fid for the same file (same qid.path) */
            k_lopen(x, 5, 11, 2); srv_answer_all(x);
            read_seq(x, 10, 0, 4, 100);                                /* blocks 4 and on are ahead */
            srv_answer_all(x);
            px_stats(x, &st);
            CHECK(st.issued >= 2);
            kern_clear(x); srv_clear();
            switch (cs) {
            case WRITE_SAME_FID: k_write(x, 300, 10, 0, 10); break;
            case WRITE_OTHER_FID: k_write(x, 300, 11, 0, 10); break;
            case SETATTR: k_setattr(x, 300, 11); break;
            case UNLINKAT: mstart(&m, Tunlinkat, 300); put32(&m, 1); puts9(&m, "f"); put32(&m, 0); ksend(x, &m); break;
            case LCREATE: mstart(&m, Tlcreate, 300); put32(&m, 1); puts9(&m, "g"); put32(&m, 0); put32(&m, 0644); put32(&m, 0); ksend(x, &m); break;
            case TTL: px_tick(x, time(NULL) + 60); break;
            }
            srv_answer_all(x);
            kern_clear(x); srv_clear();
            k_read(x, 500, 10, 4ull * COUNT, COUNT);                   /* the next one in order */
            kern_pull(x);
            CHECK(!kern_find(500, Rread));
            if (kern_find(500, Rread)) fprintf(stderr, "   (%s: answered from stale read-ahead)\n", what[cs]);
            px_free(x);
        }

        /* a clunk drops the fid's blocks (nothing is held for it afterwards), and so does a new session */
        x = fresh(&c, 10, 0);
        read_seq(x, 10, 0, 4, 100); srv_answer_all(x);
        k_clunk(x, 300, 10); srv_answer_all(x); kern_clear(x);
        px_stats(x, &st); CHECK(st.held == 0);
        px_free(x);
        x = fresh(&c, 10, 0);
        read_seq(x, 10, 0, 4, 100);                                    /* some asked for and not answered, some ready */
        px_reset(x);
        px_stats(x, &st); CHECK(st.held == 0);
        srv_clear(); srv_pull(x); kern_clear(x);
        px_free(x);
    }

    /* ---- a flush of a read that is waiting for a read-ahead answers the flush and sends nothing ---- */
    x = fresh(&c, 10, 0);
    read_seq_hold(x, 10, 3, 100);
    srv_pull(x);                                                       /* 3 and 4 are asked for */
    kern_clear(x);
    k_read(x, 700, 10, 3ull * COUNT, COUNT);                           /* parked on the block that is not answered yet */
    k_flush(x, 701, 700);
    kern_pull(x);
    CHECK(kern_find(701, Rflush) != NULL);
    srv_answer_all(x); kern_pull(x);
    CHECK(!kern_find(700, Rread));                                     /* the flushed read is not answered afterwards */
    px_free(x);

    /* ---- a flush of a read that went to the server is sent on, with the server's tag for it, and the answer comes back to the kernel's tag ---- */
    x = fresh(&c, 10, 0);
    k_read(x, 710, 10, 50ull * COUNT, COUNT);
    srv_pull(x);
    { uint16_t up = srv[nsrv - 1].tag; int before = nsrv;
      k_flush(x, 711, 710);
      srv_pull(x);
      CHECK(nsrv == before + 1 && srv[nsrv - 1].type == Tflush && srv[nsrv - 1].oldtag == up);
      srv_reply(x, &srv[nsrv - 1], 0);
      kern_pull(x);
      CHECK(kern_find(711, Rflush) != NULL); }
    px_free(x);

    /* ---- the byte budget: no more is held or asked for than total_bytes ---- */
    {
        struct px_conf small = conf(PX_DEFAULT_WINDOW);
        small.total_bytes = 3 * COUNT;
        x = fresh(&small, 10, 0);
        for (i = 0; i < 40; i++) { k_read(x, (uint16_t)(100 + i), 10, (uint64_t)i * COUNT, COUNT); srv_pull(x); px_stats(x, &st); CHECK(st.held <= 3u * COUNT); srv_answer_all(x); }
        px_free(x);
    }

    /* ---- the ramp reaches 64 requests, and not more, for 32 KB reads ---- */
    x = fresh(&c, 10, 0);
    { int maxdepth = 0;
      for (i = 0; i < 200; i++) {
          k_read(x, (uint16_t)(100 + (i % 1000)), 10, (uint64_t)i * COUNT, COUNT);
          srv_pull(x);
          px_stats(x, &st);
          if ((int)st.ahead > maxdepth) maxdepth = (int)st.ahead;
          srv_answer_all(x);
      }
      CHECK(maxdepth >= 60 && maxdepth <= 65); }
    px_free(x);

    /* ---- with 200000-byte reads the 2 MiB window allows ten ahead, not 64 ---- */
    x = fresh(&c, 10, 0);
    { int maxdepth = 0;
      for (i = 0; i < 60; i++) {
          k_read(x, (uint16_t)(100 + i), 10, (uint64_t)i * 200000u, 200000u);
          srv_pull(x);
          px_stats(x, &st);
          if ((int)st.ahead > maxdepth) maxdepth = (int)st.ahead;
          srv_answer_all(x);
      }
      CHECK(maxdepth >= 9 && maxdepth <= 11); }
    px_free(x);

    /* ---- unknown and truncated messages go on unchanged and change nothing ---- */
    x = fresh(&c, 10, 0);
    mstart(&m, 200, 55); put32(&m, 1234); mend(&m);                    /* a type nobody has */
    CHECK(px_from_kernel(x, m.b, m.n) == 0);
    srv_pull(x);
    CHECK(nsrv == 1 && srv[0].type == 200);
    mstart(&m, Tread, 56); put32(&m, 10); mend(&m);                    /* a Tread with no room for its offset and count */
    CHECK(px_from_kernel(x, m.b, m.n) == 0);
    srv_pull(x);
    CHECK(nsrv == 2 && srv[1].type == Tread);
    px_stats(x, &st); CHECK(st.issued == 0);
    px_free(x);

    /* ---- a stream that cannot be followed is refused ---- */
    x = fresh(&c, 10, 0);
    { uint8_t bad[7] = { 3, 0, 0, 0, Tread, 1, 0 };                    /* size 3: smaller than its own header */
      CHECK(px_from_kernel(x, bad, sizeof bad) == -1); }
    px_free(x);
    x = fresh(&c, 10, 0);
    { uint8_t bad[7] = { 0xFF, 0xFF, 0xFF, 0x7F, Tread, 1, 0 };        /* 2 GB */
      CHECK(px_from_server(x, bad, sizeof bad) == -1); }
    px_free(x);

    /* ---- in pieces: a byte at a time gives the same bytes as all at once ---- */
    {
        px_t *pa = px_new(&c), *pb = px_new(&c);
        struct m all;
        mstart(&all, Tversion, 0xFFFF); put32(&all, MSIZE); puts9(&all, "9P2000.L"); mend(&all);   /* NOTAG: not rewritten, so equal */
        CHECK(px_from_kernel(pa, all.b, all.n) == 0);
        for (size_t j = 0; j < all.n; j++)
            CHECK(px_from_kernel(pb, all.b + j, 1) == 0);
        CHECK(px_buf_avail(px_to_server(pa)) == all.n && px_buf_avail(px_to_server(pb)) == all.n);
        CHECK(!memcmp(px_buf_data(px_to_server(pa)), px_buf_data(px_to_server(pb)), all.n));
        CHECK(!memcmp(px_buf_data(px_to_server(pa)), all.b, all.n));                           /* and it is what was sent */
        px_free(pa); px_free(pb);
    }

    /* ---- noise: nothing may crash or read outside what it was given (run under ASAN and UBSAN for the real test) ---- */
    {
        const char *fr = getenv("PX_FUZZ_ROUNDS"), *fs = getenv("PX_FUZZ_SEED");
        int rounds = fr ? atoi(fr) : 60;
        rnd_seed = fs ? strtoul(fs, NULL, 10) : 12345;
        #define RND() rnd()
        for (int round = 0; round < rounds; round++) {
            x = fresh(&c, 10, round % 3);
            for (i = 0; i < 400; i++) {
                struct m r;
                uint32_t kind = RND() % 6;
                if (kind == 0) {                                       /* pure noise, both ways */
                    size_t l = RND() % 200;
                    for (size_t j = 0; j < l; j++) r.b[j] = (uint8_t)RND();
                    r.n = l;
                    if (RND() & 1) px_from_kernel(x, r.b, r.n); else px_from_server(x, r.b, r.n);
                } else {                                               /* well framed, random type and body */
                    uint8_t type = (kind == 1) ? (uint8_t)(RND() % 128) : (uint8_t[]){ Tread, Twrite, Twalk, Tlopen, Tclunk, Tflush, Tsetattr }[RND() % 7];
                    mstart(&r, type, (uint16_t)(RND() % 16));
                    for (uint32_t j = RND() % 40; j > 0; j--) put8(&r, (uint8_t)(RND() % 4 ? RND() % 8 : RND()));
                    mend(&r);
                    if (px_from_kernel(x, r.b, r.n) < 0) break;
                }
                srv_pull(x);
                { int k = RND() % (nsrv ? nsrv : 1);                   /* answer one of them, sometimes with garbage that is framed */
                  if (nsrv && !srv[k].done) { if (RND() % 5 == 0) srv_reply(x, &srv[k], 5); else srv_reply(x, &srv[k], 0); }
                  else { mstart(&r, (uint8_t)(RND() % 128), (uint16_t)(RND() % 16)); for (uint32_t j = RND() % 30; j > 0; j--) put8(&r, (uint8_t)RND()); mend(&r); if (px_from_server(x, r.b, r.n) < 0) break; } }
                if (nsrv > 6000) srv_clear();
                kern_clear(x);
            }
            px_free(x);
            srv_clear();
        }
        CHECK(1);                                                      /* it got here */
    }

    DONE();
}
