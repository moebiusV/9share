/* proxy.c -- see proxy.h.  Self-contained: libc only, no I/O, so that the test and a fuzzer can use it as it is.
 *
 * The 9P2000.L messages it looks at (all numbers little-endian, size[4] type[1] tag[2] then):
 *   Tversion msize[4] version[s]          Rversion msize[4] version[s]
 *   Tattach  fid[4] afid[4] uname[s] aname[s] n_uname[4]        Rattach qid[13]
 *   Twalk    fid[4] newfid[4] nwname[2] wname[s]...             Rwalk nwqid[2] qid[13]...
 *   Tlopen   fid[4] flags[4]              Rlopen qid[13] iounit[4]
 *   Tlcreate fid[4] name[s] flags[4] mode[4] gid[4]             Rlcreate qid[13] iounit[4]
 *   Tread    fid[4] offset[8] count[4]    Rread count[4] data[count]
 *   Twrite   fid[4] offset[8] count[4] data                      Tclunk/Tremove fid[4]       Tsetattr fid[4] ...
 *   Tflush   oldtag[2]                    Rlerror ecode[4]
 * and qid is type[1] version[4] path[8]. */
#define _GNU_SOURCE
#include "proxy.h"
#include <stdlib.h>
#include <string.h>

enum {
    Rlerror = 7, Tlopen = 12, Rlopen, Tlcreate = 14, Rlcreate, Tsymlink = 16, Tmknod = 18, Trename = 20, Tsetattr = 26,
    Txattrcreate = 32, Tlink = 70, Tmkdir = 72, Trenameat = 74, Tunlinkat = 76,
    Tversion = 100, Rversion, Tattach = 104, Rattach, Tflush = 108, Rflush, Twalk = 110, Rwalk,
    Tread = 116, Rread, Twrite = 118, Rwrite, Tclunk = 120, Rclunk, Tremove = 122, Rremove
};

#define NOTAG     0xFFFFu
#define HDR       7u
#define QIDSZ     13u
#define IOHDRSZ   24u
#define MAXTAGS   65535u            /* upstream tags 0 .. 65534 (65535 is NOTAG) */
#define MAXFIDS   8192u
#define HASH      1024u
#define MAXMSG    (16u * 1024 * 1024)

static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t g16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t g64(const uint8_t *p) { return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32; }
static void p16(uint8_t *p, uint16_t v) { p[0] = v & 255; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *p, uint32_t v) { p16(p, v & 0xFFFF); p16(p + 2, (uint16_t)(v >> 16)); }
static void p64(uint8_t *p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }

struct px_entry;

struct px_fid {
    struct px_fid  *hnext;
    uint32_t        fid;
    uint64_t        qpath;
    uint8_t         have_qid, opened, ro;
    int             have_next;
    uint64_t        next;               /* the offset a reader going in order would ask for next */
    uint64_t        pf_next;            /* the next offset to read ahead */
    uint32_t        last_count;
    int             run;                /* reads in a row that went on from the one before */
    unsigned        target;             /* how many blocks to keep ahead; grows by one per hit, like scp's max_req */
    int             eof, nofetch;
    struct px_entry *ents, **tail;      /* oldest first */
    unsigned        nents;
};

enum { E_FLIGHT, E_READY };

struct px_entry {
    struct px_entry *next;
    struct px_fid   *fid;
    uint64_t         off;
    uint32_t         count, got;
    uint8_t         *data;
    uint16_t         utag;
    uint8_t          state;
    int              parked;            /* a kernel read waits for this block ... */
    uint16_t         park_ktag;         /* ... under this tag */
    time_t           t;
};

enum { K_FREE, K_FORWARD, K_AHEAD, K_FLUSH };

struct px_req {                         /* by upstream tag */
    uint8_t          kind, type;
    uint16_t         ktag;
    uint32_t         fid, newfid, flags, gen;
    uint16_t         nwname;
    struct px_entry *e;                 /* K_AHEAD: the block (NULL when it was dropped while on its way) */
    uint16_t         old_utag;          /* K_FLUSH: the request that was flushed ... */
    uint32_t         old_gen;           /* ... and which use of that tag */
};

struct px {
    struct px_conf   c;
    time_t           now;
    uint32_t         msize, maxmsg;
    struct px_buf    in[2], out[2];     /* [0] from the kernel, to the server; [1] from the server, to the kernel */
    struct px_fid   *fids[HASH];
    unsigned         nfids;
    struct px_req   *req;               /* MAXTAGS */
    uint16_t        *freetag;
    unsigned         nfree;
    int32_t         *k2u;               /* kernel tag -> upstream tag, -1 none (65536) */
    struct px_entry **parked;           /* kernel tag -> the block its read waits for (65536) */
    uint32_t         gen;
    struct px_stats  st;
};

/* ---- byte queues ---- */
static int buf_reserve(struct px_buf *b, size_t n)
{
    size_t used, ncap;
    uint8_t *np;
    if (b->off == b->len)
        b->off = b->len = 0;
    if (b->cap - b->len >= n)
        return 0;
    if (b->off > 0) {                                /* close up the front */
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->cap - b->len >= n)
            return 0;
    }
    used = b->len;
    ncap = b->cap ? b->cap : 4096;
    while (ncap - used < n)
        ncap *= 2;
    if (!(np = realloc(b->p, ncap)))
        return -1;
    b->p = np;
    b->cap = ncap;
    return 0;
}

static uint8_t *buf_add(struct px_buf *b, const void *p, size_t n)
{
    uint8_t *d;
    if (buf_reserve(b, n) < 0)
        return NULL;
    d = b->p + b->len;
    if (p)
        memcpy(d, p, n);
    b->len += n;
    return d;
}

void px_buf_consume(struct px_buf *b, size_t n)
{
    b->off += n < px_buf_avail(b) ? n : px_buf_avail(b);
    if (b->off == b->len)
        b->off = b->len = 0;
}

/* ---- tags ---- */
static void tags_init(px_t *x)
{
    unsigned i;
    for (i = 0; i < MAXTAGS; i++)
        x->freetag[i] = (uint16_t)(MAXTAGS - 1 - i);          /* pop gives 0, 1, 2 ... */
    x->nfree = MAXTAGS;
    memset(x->req, 0, MAXTAGS * sizeof *x->req);
    for (i = 0; i < 65536; i++)
        x->k2u[i] = -1;
    memset(x->parked, 0, 65536 * sizeof *x->parked);
}

static int tag_alloc(px_t *x, uint8_t kind)
{
    uint16_t u;
    if (!x->nfree)
        return -1;
    u = x->freetag[--x->nfree];
    memset(&x->req[u], 0, sizeof x->req[u]);
    x->req[u].kind = kind;
    x->req[u].gen = ++x->gen;
    return u;
}

static void tag_free(px_t *x, unsigned u)
{
    if (u < MAXTAGS && x->req[u].kind != K_FREE) {
        x->req[u].kind = K_FREE;
        x->freetag[x->nfree++] = (uint16_t)u;
    }
}

/* ---- fids ---- */
static struct px_fid *fid_find(px_t *x, uint32_t fid)
{
    struct px_fid *f;
    for (f = x->fids[fid % HASH]; f; f = f->hnext)
        if (f->fid == fid)
            return f;
    return NULL;
}

static void entry_free(px_t *x, struct px_entry *e, int count_dropped);
static void entry_drop(px_t *x, struct px_entry *e);

static void fid_drop_entries(px_t *x, struct px_fid *f)
{
    while (f->ents)
        entry_drop(x, f->ents);
}

static void fid_remove(px_t *x, uint32_t fid)
{
    struct px_fid **pp, *f;
    for (pp = &x->fids[fid % HASH]; (f = *pp); pp = &f->hnext)
        if (f->fid == fid) {
            fid_drop_entries(x, f);
            *pp = f->hnext;
            free(f);
            x->nfids--;
            return;
        }
}

static struct px_fid *fid_new(px_t *x, uint32_t fid)
{
    struct px_fid *f = fid_find(x, fid);
    if (f)
        fid_drop_entries(x, f);
    else {
        if (x->nfids >= MAXFIDS || !(f = calloc(1, sizeof *f)))
            return NULL;
        f->fid = fid;
        f->hnext = x->fids[fid % HASH];
        x->fids[fid % HASH] = f;
        x->nfids++;
    }
    f->have_qid = f->opened = f->ro = 0;
    f->have_next = 0;
    f->run = 0;
    f->eof = f->nofetch = 0;
    f->target = 2;
    f->last_count = 0;
    f->tail = &f->ents;
    f->ents = NULL;
    f->nents = 0;
    return f;
}

/* ---- sending ---- */
static int send_server(px_t *x, const uint8_t *msg, size_t n) { return buf_add(&x->out[0], msg, n) ? 0 : -1; }
static int send_kernel(px_t *x, const uint8_t *msg, size_t n) { return buf_add(&x->out[1], msg, n) ? 0 : -1; }

static int send_rread(px_t *x, uint16_t ktag, const uint8_t *data, uint32_t n)
{
    uint8_t h[11];
    p32(h, 11 + n); h[4] = Rread; p16(h + 5, ktag); p32(h + 7, n);
    if (!buf_add(&x->out[1], h, 11) || (n && !buf_add(&x->out[1], data, n)))
        return -1;
    return 0;
}

static int send_rflush(px_t *x, uint16_t ktag)
{
    uint8_t h[7];
    p32(h, 7); h[4] = Rflush; p16(h + 5, ktag);
    return send_kernel(x, h, 7);
}

/* a kernel read that goes on to the server as it is (or again, when what it waited for is gone) */
static int forward_read(px_t *x, uint16_t ktag, uint32_t fid, uint64_t off, uint32_t count)
{
    uint8_t m[23];
    int u = tag_alloc(x, K_FORWARD);
    if (u < 0)
        return -1;
    x->req[u].type = Tread;
    x->req[u].ktag = ktag;
    x->req[u].fid = fid;
    x->k2u[ktag] = u;
    p32(m, 23); m[4] = Tread; p16(m + 5, (uint16_t)u); p32(m + 7, fid); p64(m + 11, off); p32(m + 19, count);
    x->st.forwarded++;
    return send_server(x, m, sizeof m);
}

/* ---- blocks read ahead ---- */
static void entry_unlink(struct px_entry *e)
{
    struct px_fid *f = e->fid;
    struct px_entry **pp;
    for (pp = &f->ents; *pp; pp = &(*pp)->next)
        if (*pp == e) {
            *pp = e->next;
            break;
        }
    f->tail = &f->ents;
    for (pp = &f->ents; *pp; pp = &(*pp)->next)
        f->tail = &(*pp)->next;
    f->nents--;
}

static void entry_free(px_t *x, struct px_entry *e, int count_dropped)
{
    x->st.held -= e->state == E_READY ? e->got : e->count;
    x->st.ahead--;
    if (count_dropped)
        x->st.dropped++;
    if (e->state == E_FLIGHT && e->utag < MAXTAGS && x->req[e->utag].kind == K_AHEAD)
        x->req[e->utag].e = NULL;                   /* its answer, when it comes, is thrown away */
    free(e->data);
    free(e);
}

/* throw a block away; a kernel read that waited for it goes to the server instead, so that it is not lost */
static void entry_drop(px_t *x, struct px_entry *e)
{
    int parked = e->parked;
    uint16_t kt = e->park_ktag;
    uint32_t fid = e->fid->fid;
    uint64_t off = e->off;
    uint32_t count = e->count;
    entry_unlink(e);
    if (parked)
        x->parked[kt] = NULL;
    entry_free(x, e, 1);
    if (parked)
        forward_read(x, kt, fid, off, count);
}

static void drop_all_entries(px_t *x)
{
    unsigned i;
    for (i = 0; i < HASH; i++) {
        struct px_fid *f;
        for (f = x->fids[i]; f; f = f->hnext)
            fid_drop_entries(x, f);
    }
}

static void drop_file_entries(px_t *x, uint32_t fid)
{
    struct px_fid *f = fid_find(x, fid);
    unsigned i;
    if (!f || !f->have_qid) {                       /* what file it is, is not known: no one's blocks are safe */
        drop_all_entries(x);
        return;
    }
    for (i = 0; i < HASH; i++) {
        struct px_fid *g;
        for (g = x->fids[i]; g; g = g->hnext)
            if (g->have_qid && g->qpath == f->qpath)
                fid_drop_entries(x, g);
    }
}

static unsigned fid_cap(const px_t *x, const struct px_fid *f)
{
    size_t c = f->last_count ? x->c.window_bytes / f->last_count : 0;
    if (c > x->c.max_requests)
        c = x->c.max_requests;
    return c ? (unsigned)c : 1;
}

/* keep f->target blocks asked for or ready ahead of the reader */
static void top_up(px_t *x, struct px_fid *f)
{
    unsigned cap = fid_cap(x, f), want = f->target < cap ? f->target : cap;
    if (!x->c.window_bytes)
        return;
    while (f->nents < want && !f->eof && !f->nofetch) {
        struct px_entry *e;
        uint8_t m[23];
        int u;
        if (f->last_count > x->msize - IOHDRSZ || x->st.held + f->last_count > x->c.total_bytes)
            return;
        if (f->pf_next < f->next)
            f->pf_next = f->next;
        if ((u = tag_alloc(x, K_AHEAD)) < 0)
            return;
        if (!(e = calloc(1, sizeof *e))) {
            tag_free(x, (unsigned)u);
            return;
        }
        e->fid = f;
        e->off = f->pf_next;
        e->count = f->last_count;
        e->utag = (uint16_t)u;
        e->state = E_FLIGHT;
        e->t = x->now;
        x->req[u].e = e;
        *f->tail = e;
        f->tail = &e->next;
        f->nents++;
        x->st.held += e->count;
        x->st.ahead++;
        x->st.issued++;
        f->pf_next += e->count;
        p32(m, 23); m[4] = Tread; p16(m + 5, (uint16_t)u); p32(m + 7, f->fid); p64(m + 11, e->off); p32(m + 19, e->count);
        if (send_server(x, m, sizeof m) < 0)
            return;
    }
}

/* what the kernel asks for with Tread: answered from what was read ahead, or sent on.  Returns -1 only when memory or tags run out. */
static int on_tread(px_t *x, uint16_t ktag, uint32_t fid, uint64_t off, uint32_t count)
{
    struct px_fid *f = fid_find(x, fid);          /* followed whether or not reading ahead is on: it can be switched on at any time */
    struct px_entry *e;
    int seq;
    if (!f || !f->opened || !f->ro || count == 0)
        return forward_read(x, ktag, fid, off, count);
    seq = f->have_next && off == f->next;
    f->run = seq ? f->run + 1 : 0;
    f->have_next = 1;
    f->next = off + count;
    if (!seq || f->last_count != count) {           /* not in order, or reads of another size: what was ahead is no use */
        fid_drop_entries(x, f);
        f->pf_next = f->next;
        f->eof = f->nofetch = 0;
        f->target = 2;
        if (f->last_count != count)
            f->run = seq ? 1 : 0;
    }
    f->last_count = count;
    for (e = f->ents; e; e = e->next)
        if (e->off == off && e->count == count)
            break;
    if (e && e->state == E_READY && x->now - e->t > x->c.ttl) {
        entry_drop(x, e);                           /* too old to trust */
        e = NULL;
    }
    if (e) {
        unsigned cap = fid_cap(x, f);
        x->st.hits++;
        if (f->target < cap)
            f->target++;
        if (e->state == E_READY) {
            int rc = send_rread(x, ktag, e->data, e->got);
            entry_unlink(e);
            entry_free(x, e, 0);
            if (rc < 0)
                return -1;
        } else {
            e->parked = 1;
            e->park_ktag = ktag;
            x->parked[ktag] = e;
        }
    } else if (forward_read(x, ktag, fid, off, count) < 0)
        return -1;
    if (f->run >= 2)
        top_up(x, f);
    return 0;
}

/* the answer to a read that was asked for ahead */
static int on_ahead_reply(px_t *x, unsigned u, const uint8_t *msg, uint32_t size)
{
    struct px_entry *e = x->req[u].e;
    struct px_fid *f;
    int ok;
    uint32_t got = 0;
    tag_free(x, u);
    if (!e)
        return 0;                                    /* dropped while it was on its way */
    f = e->fid;
    ok = msg[4] == Rread && size >= 11 && (got = g32(msg + 7)) <= e->count && size == 11 + got;
    if (!ok) {                                       /* an error, or nonsense: not shown to anyone; the kernel's own read is made afresh */
        f->nofetch = 1;
        entry_drop(x, e);
        return 0;
    }
    if (got) {
        if (!(e->data = malloc(got))) {
            f->nofetch = 1;
            entry_drop(x, e);
            return 0;
        }
        memcpy(e->data, msg + 11, got);
    }
    x->st.held -= e->count - got;
    e->got = got;
    e->state = E_READY;
    if (got < e->count)
        f->eof = 1;
    if (e->parked) {
        uint16_t kt = e->park_ktag;
        int rc;
        x->parked[kt] = NULL;
        rc = send_rread(x, kt, e->data, e->got);
        entry_unlink(e);
        entry_free(x, e, 0);
        if (rc < 0)
            return -1;
        top_up(x, f);
    }
    return 0;
}

/* ---- the kernel's side ---- */
static int forward_t(px_t *x, uint8_t *msg, uint32_t size, struct px_req **out)
{
    uint16_t ktag = g16(msg + 5);
    int u = tag_alloc(x, K_FORWARD);
    uint8_t *d;
    if (u < 0)
        return -1;
    x->req[u].ktag = ktag;
    x->req[u].type = msg[4];
    x->k2u[ktag] = u;
    if (!(d = buf_add(&x->out[0], msg, size)))
        return -1;
    p16(d + 5, (uint16_t)u);
    *out = &x->req[u];
    return 0;
}

static int on_t(px_t *x, uint8_t *msg, uint32_t size)
{
    uint8_t type = msg[4];
    uint16_t ktag = g16(msg + 5);
    struct px_req *r;
    if (type == Tversion) {                          /* a new session of 9P: nothing of the old one counts */
        px_reset(x);
        if (ktag == NOTAG)                           /* what a kernel sends: the answer has NOTAG too and needs no table */
            return send_server(x, msg, size);
        /* any other tag is an ordinary request from here on (other clients do send it so): it is mapped like the rest */
    }
    if (type == Tflush && size >= HDR + 2) {
        uint16_t old = g16(msg + 7);
        uint8_t m[9];
        int u;
        if (x->parked[old]) {                        /* the read waits for a block: nothing went to the server for it */
            x->parked[old]->parked = 0;
            x->parked[old] = NULL;
            return send_rflush(x, ktag);
        }
        if (x->k2u[old] < 0)                         /* answered already */
            return send_rflush(x, ktag);
        if ((u = tag_alloc(x, K_FLUSH)) < 0)
            return -1;
        x->req[u].ktag = ktag;
        x->req[u].type = Tflush;
        x->req[u].old_utag = (uint16_t)x->k2u[old];
        x->req[u].old_gen = x->req[x->k2u[old]].gen;
        x->k2u[ktag] = u;
        p32(m, 9); m[4] = Tflush; p16(m + 5, (uint16_t)u); p16(m + 7, (uint16_t)x->k2u[old]);
        return send_server(x, m, sizeof m);
    }
    switch (type) {
    case Tread:
        if (size >= 23)
            return on_tread(x, ktag, g32(msg + 7), g64(msg + 11), g32(msg + 19));
        break;
    case Twrite:
    case Tsetattr:
        if (size >= 11)
            drop_file_entries(x, g32(msg + 7));
        else
            drop_all_entries(x);
        break;
    case Tclunk:
    case Tremove:
        if (size >= 11) {
            if (type == Tremove)
                drop_all_entries(x);
            fid_remove(x, g32(msg + 7));
        }
        break;
    case Tlcreate: case Tsymlink: case Tmknod: case Trename: case Tlink: case Tmkdir: case Trenameat: case Tunlinkat: case Txattrcreate:
        drop_all_entries(x);                         /* the tree changes: it is not worth working out whose blocks it touches */
        break;
    default:
        break;
    }
    if (forward_t(x, msg, size, &r) < 0)
        return -1;
    switch (type) {
    case Twalk:
        if (size >= 17) { r->fid = g32(msg + 7); r->newfid = g32(msg + 11); r->nwname = g16(msg + 15); }
        break;
    case Tlopen:
        if (size >= 15) { r->fid = g32(msg + 7); r->flags = g32(msg + 11); }
        break;
    case Tattach: case Tlcreate:
        if (size >= 11)
            r->fid = g32(msg + 7);
        break;
    default:
        break;
    }
    return 0;
}

/* ---- the server's side ---- */
static int on_r(px_t *x, uint8_t *msg, uint32_t size)
{
    uint8_t type = msg[4];
    uint16_t utag = g16(msg + 5);
    struct px_req *r;
    uint16_t ktag;
    struct px_fid *f;
    uint8_t *d;
    if (utag == NOTAG) {                             /* Rversion */
        if (type == Rversion && size >= 11) {
            uint32_t ms = g32(msg + 7);
            if (ms >= 4096 && ms <= MAXMSG) {
                x->msize = ms;
                x->maxmsg = ms;
            }
        }
        return send_kernel(x, msg, size);
    }
    if (utag >= MAXTAGS || x->req[utag].kind == K_FREE)
        return 0;                                    /* an answer to nothing we asked: after a flush, say; it goes nowhere */
    r = &x->req[utag];
    if (r->kind == K_AHEAD)
        return on_ahead_reply(x, utag, msg, size);
    ktag = r->ktag;
    if (r->kind == K_FLUSH) {                        /* the flushed request is over, if it is still the one we mean */
        if (r->old_utag < MAXTAGS && x->req[r->old_utag].kind != K_FREE && x->req[r->old_utag].gen == r->old_gen) {
            uint16_t ok = x->req[r->old_utag].ktag;
            if (x->k2u[ok] == r->old_utag)
                x->k2u[ok] = -1;
            tag_free(x, r->old_utag);
        }
        if (x->k2u[ktag] == (int32_t)utag)
            x->k2u[ktag] = -1;
        tag_free(x, utag);
        return send_rflush(x, ktag);
    }
    switch (r->type) {
    case Tversion:
        if (type == Rversion && size >= 11) {
            uint32_t ms = g32(msg + 7);
            if (ms >= 4096 && ms <= MAXMSG) {
                x->msize = ms;
                x->maxmsg = ms;
            }
        }
        break;
    case Twalk:
        if (type == Rwalk && size >= HDR + 2) {
            uint16_t nq = g16(msg + 7);
            if (nq == r->nwname && size >= HDR + 2 + (uint32_t)nq * QIDSZ) {
                struct px_fid *src = r->nwname == 0 ? fid_find(x, r->fid) : NULL;
                uint64_t path = 0;
                uint8_t have = 0;
                if (nq) { path = g64(msg + 9 + (nq - 1) * QIDSZ + 5); have = 1; }
                else if (src && src->have_qid) { path = src->qpath; have = 1; }
                if ((f = fid_new(x, r->newfid))) {
                    f->qpath = path;
                    f->have_qid = have;
                }
            }
        }
        break;
    case Tattach:
        if (type == Rattach && size >= HDR + QIDSZ && (f = fid_new(x, r->fid))) {
            f->qpath = g64(msg + 12);
            f->have_qid = 1;
        }
        break;
    case Tlopen:
        if (type == Rlopen && size >= HDR + QIDSZ && (f = fid_find(x, r->fid))) {
            f->qpath = g64(msg + 12);
            f->have_qid = 1;
            f->opened = 1;
            f->ro = (r->flags & 3) == 0 && msg[7] == 0;     /* read-only, and a plain file */
            f->have_next = 0;
        }
        break;
    case Tlcreate:
        if (type == Rlcreate && size >= HDR + QIDSZ && (f = fid_find(x, r->fid))) {
            f->qpath = g64(msg + 12);
            f->have_qid = 1;
            f->opened = 1;
            f->ro = 0;
        }
        break;
    default:
        break;
    }
    if (x->k2u[ktag] == (int32_t)utag)
        x->k2u[ktag] = -1;
    tag_free(x, utag);
    if (!(d = buf_add(&x->out[1], msg, size)))
        return -1;
    p16(d + 5, ktag);
    return 0;
}

/* ---- framing ---- */
static int pump(px_t *x, int side, const uint8_t *p, size_t n)
{
    struct px_buf *b = &x->in[side];
    int rc = 0;
    if (n && !buf_add(b, p, n))
        return -1;
    while (px_buf_avail(b) >= HDR) {
        uint8_t *m = b->p + b->off;
        uint32_t size = g32(m);
        if (size < HDR || size > x->maxmsg)
            return -1;                               /* this is not 9P, or not the 9P that was agreed on: it cannot be followed */
        if (px_buf_avail(b) < size)
            break;
        rc = side == 0 ? on_t(x, m, size) : on_r(x, m, size);
        if (rc < 0)
            return -1;
        px_buf_consume(b, size);
    }
    return 0;
}

int px_from_kernel(px_t *x, const uint8_t *p, size_t n) { return pump(x, 0, p, n); }
int px_from_server(px_t *x, const uint8_t *p, size_t n) { return pump(x, 1, p, n); }
struct px_buf *px_to_server(px_t *x) { return &x->out[0]; }
struct px_buf *px_to_kernel(px_t *x) { return &x->out[1]; }

/* ---- the whole ---- */
static void clear_state(px_t *x)
{
    unsigned i;
    for (i = 0; i < HASH; i++) {
        struct px_fid *f = x->fids[i], *nx;
        for (; f; f = nx) {
            nx = f->hnext;
            while (f->ents) {                        /* no read waits for anything any more: the session it belonged to is over */
                struct px_entry *e = f->ents;
                f->ents = e->next;
                free(e->data);
                free(e);
            }
            free(f);
        }
        x->fids[i] = NULL;
    }
    x->nfids = 0;
    x->st.held = 0;
    x->st.ahead = 0;
    tags_init(x);
}

void px_reset(px_t *x)
{
    clear_state(x);
    x->msize = 8192;
    x->maxmsg = MAXMSG;
}

px_t *px_new(const struct px_conf *c)
{
    px_t *x = calloc(1, sizeof *x);
    if (!x)
        return NULL;
    x->c = *c;
    if (!x->c.max_requests)
        x->c.max_requests = 1;
    x->now = time(NULL);
    x->req = calloc(MAXTAGS, sizeof *x->req);
    x->freetag = calloc(MAXTAGS, sizeof *x->freetag);
    x->k2u = calloc(65536, sizeof *x->k2u);
    x->parked = calloc(65536, sizeof *x->parked);
    if (!x->req || !x->freetag || !x->k2u || !x->parked) {
        px_free(x);
        return NULL;
    }
    tags_init(x);
    x->msize = 8192;
    x->maxmsg = MAXMSG;
    return x;
}

void px_free(px_t *x)
{
    if (!x)
        return;
    if (x->req && x->freetag && x->k2u && x->parked)
        clear_state(x);
    free(x->req); free(x->freetag); free(x->k2u); free(x->parked);
    free(x->in[0].p); free(x->in[1].p); free(x->out[0].p); free(x->out[1].p);
    free(x);
}

void px_set_window(px_t *x, size_t window_bytes)
{
    unsigned i;
    x->c.window_bytes = window_bytes;
    if (window_bytes)
        return;                                       /* on: the next reads in order start it */
    drop_all_entries(x);                              /* off: what was read ahead goes, and a read that waited for it goes to the server */
    for (i = 0; i < HASH; i++) {
        struct px_fid *f;
        for (f = x->fids[i]; f; f = f->hnext) {
            f->run = 0;
            f->target = 2;
            f->eof = f->nofetch = 0;
        }
    }
}

void px_tick(px_t *x, time_t now)
{
    unsigned i;
    x->now = now;
    for (i = 0; i < HASH; i++) {
        struct px_fid *f;
        for (f = x->fids[i]; f; f = f->hnext) {
            struct px_entry *e = f->ents, *nx;
            for (; e; e = nx) {
                nx = e->next;
                if (!e->parked && now - e->t > x->c.ttl)
                    entry_drop(x, e);
            }
        }
    }
}

void px_stats(const px_t *x, struct px_stats *s) { *s = x->st; }
