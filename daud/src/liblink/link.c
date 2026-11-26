/* link.c -- the resumable link engine; see link.h.
 *
 * A carrier carries frames: type:u8 length:u32 payload.  Integers are little-endian.
 *
 *   HELLO   dialer -> acceptor, first: "P9L1" flags:u8 sid:16 received:u64 mech:s8 uname:s8 aname:s16
 *             flags bit 0: this is a new session (not a continuation); received: bytes of the session's other direction
 *             the dialer has taken in; mech: the SASL mechanism the dialer will use; uname, aname: what the mount asks for.
 *   AUTH    either way, between HELLO and STATUS: one step of the SASL mechanism (none for EXTERNAL).  With SCRAM-SHA-256 the
 *             order is HELLO, AUTH(client-first) -> AUTH(server-first) <- AUTH(client-final) -> AUTH(server-final), STATUS <-.
 *             The acceptor parses the HELLO but acts on it (new session, replacement, lost) only after the proof checks out;
 *             a failed proof ends the carrier and changes nothing.  The dialer accepts no STATUS before the server's proof.
 *   STATUS  acceptor -> dialer, once: status:u8 received:u64
 *             status 0: a new session began; 1: the session continues; 2: not known here (lost); 3: refused.
 *   DATA    bytes of the 9P connection (at most 16 KB a frame), in order, each byte once per carrier.
 *   ACK     received:u64 -- every byte below this has been taken in (and may be forgotten by the sender).
 *   CLOSE   reason:s8 -- the carrier is about to be dropped for this reason.
 *
 * After STATUS each end sends, from the offset the other said it had received, every byte it still holds: nothing is lost
 * and nothing is delivered twice (a byte's place in the session is its offset, so a repeat is recognised and dropped). */
#define _GNU_SOURCE
#include "link.h"
#include "auth.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

enum { F_HELLO = 1, F_STATUS = 2, F_AUTH = 3, F_DATA = 4, F_ACK = 5, F_CLOSE = 6 };
enum { ST_NEW = 0, ST_RESUMED = 1, ST_LOST = 2, ST_REFUSED = 3 };
#define MAXDATA   16384
#define MAXFRAME  (MAXDATA + 5)
#define HDR       5

struct lk {
    int       role, state, fd;
    size_t    cap;
    uint8_t   sid[LK_SID_LEN];
    int       have_sid, fresh;               /* dialer: the session was never confirmed by the acceptor */
    char      uname[LK_NAME_MAX], aname[LK_PATH_MAX];
    char     *secret;
    struct lk_auth *auth;
    /* to the other end: tx[0 .. tx_len) are the bytes of offsets tx_acked .. tx_total */
    uint8_t  *tx;
    size_t    tx_len;
    uint64_t  tx_acked, tx_total, tx_sent;
    /* from the other end: rx[rx_off .. rx_off + rx_len) wait for lk_read */
    uint8_t  *rx;
    size_t    rx_off, rx_len;
    uint64_t  rx_total, rx_acked_sent;
    uint8_t   in[MAXFRAME + 16];
    size_t    in_len;
    uint8_t   out[MAXFRAME + 4096];            /* handshake frames queue behind one another; data frames only when it is empty */
    size_t    out_len, out_off;
    int       got_hello, close_after_flush;
    int       auth_done;                       /* the SASL exchange on this carrier has succeeded */
    /* acceptor with a secret: the HELLO is parsed but acted on only once the dialer has proved the secret */
    int       hp_pending, hp_fresh;
    uint8_t   hp_sid[LK_SID_LEN];
    uint64_t  hp_rx;
    char      hp_un[LK_NAME_MAX], hp_an[LK_PATH_MAX];
    uint64_t  peer_rx;
    time_t    down_since;
    unsigned  ev;
    char      err[200];
};

static void put16(uint8_t *p, unsigned v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static unsigned get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

lk_t *lk_new(int role, size_t cap)
{
    lk_t *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->role = role;
    l->cap = cap ? cap : 4u << 20;
    l->fd = -1;
    l->state = LK_S_DETACHED;
    l->down_since = time(NULL);
    l->tx = malloc(l->cap);
    l->rx = malloc(l->cap);
    if (!l->tx || !l->rx) {
        lk_free(l);
        return NULL;
    }
    return l;
}

void lk_free(lk_t *l)
{
    if (!l)
        return;
    if (l->fd >= 0)
        close(l->fd);
    lk_auth_free(l->auth);
    free(l->secret);
    free(l->tx);
    free(l->rx);
    free(l);
}

static void seterr(lk_t *l, const char *msg)
{
    snprintf(l->err, sizeof l->err, "%s", msg);
}

int lk_begin(lk_t *l, const char *uname, const char *aname)
{
    if (strlen(uname) >= LK_NAME_MAX || strlen(aname) >= LK_PATH_MAX)
        return -1;
    if (getrandom(l->sid, sizeof l->sid, 0) != (ssize_t)sizeof l->sid)
        return -1;
    l->have_sid = 1;
    l->fresh = 1;
    snprintf(l->uname, sizeof l->uname, "%s", uname);
    snprintf(l->aname, sizeof l->aname, "%s", aname);
    l->tx_len = l->rx_len = l->rx_off = 0;
    l->tx_acked = l->tx_total = l->tx_sent = l->rx_total = l->rx_acked_sent = 0;
    l->state = LK_S_DETACHED;
    l->down_since = time(NULL);
    return 0;
}

int lk_set_secret(lk_t *l, const char *secret)
{
    free(l->secret);
    l->secret = secret ? strdup(secret) : NULL;
    return secret && !l->secret ? -1 : 0;
}

int lk_fd(const lk_t *l) { return l->fd; }
int lk_state(const lk_t *l) { return l->state; }
const char *lk_uname(const lk_t *l) { return l->uname; }
const char *lk_aname(const lk_t *l) { return l->aname; }
const char *lk_error(const lk_t *l) { return l->err; }
void lk_sid(const lk_t *l, uint8_t sid[LK_SID_LEN]) { memcpy(sid, l->sid, LK_SID_LEN); }
size_t lk_space(const lk_t *l) { return l->state == LK_S_LOST ? 0 : l->cap - l->tx_len; }
size_t lk_avail(const lk_t *l) { return l->rx_len; }
int lk_unsent(const lk_t *l) { return l->tx_acked != l->tx_total; }

/* ---- the carrier ---- */

static unsigned drop(lk_t *l, const char *why)
{
    if (l->fd >= 0) {
        close(l->fd);
        l->fd = -1;
    }
    lk_auth_free(l->auth);
    l->auth = NULL;
    l->in_len = l->out_len = l->out_off = 0;
    l->got_hello = l->close_after_flush = 0;
    l->auth_done = l->hp_pending = 0;
    if (l->state != LK_S_LOST)
        l->state = LK_S_DETACHED;
    l->down_since = time(NULL);
    seterr(l, why);
    return LK_EV_DOWN;
}

void lk_detach(lk_t *l)
{
    if (l->fd >= 0)
        l->ev |= drop(l, "the carrier failed");
}

static int queue(lk_t *l, int type, const uint8_t *payload, size_t n)
{
    uint8_t *o;
    if (l->out_off == l->out_len)
        l->out_off = l->out_len = 0;
    if (n > MAXFRAME || l->out_len + HDR + n > sizeof l->out)
        return -1;
    o = l->out + l->out_len;
    o[0] = (uint8_t)type;
    put32(o + 1, (uint32_t)n);
    if (n)
        memcpy(o + HDR, payload, n);
    l->out_len += HDR + n;
    return 0;
}

static size_t put_s8(uint8_t *p, const char *s)
{
    size_t n = strlen(s);
    p[0] = (uint8_t)n;
    memcpy(p + 1, s, n);
    return 1 + n;
}

static int send_hello(lk_t *l)
{
    uint8_t b[512];
    size_t o = 0, n;
    const char *mech = l->secret ? "SCRAM-SHA-256" : "EXTERNAL";
    memcpy(b, "P9L1", 4); o = 4;
    b[o++] = l->fresh ? 1 : 0;
    memcpy(b + o, l->sid, LK_SID_LEN); o += LK_SID_LEN;
    put64(b + o, l->rx_total); o += 8;
    o += put_s8(b + o, mech);
    o += put_s8(b + o, l->uname);
    n = strlen(l->aname);
    put16(b + o, (unsigned)n); o += 2;
    memcpy(b + o, l->aname, n); o += n;
    return queue(l, F_HELLO, b, o);
}

int lk_attach(lk_t *l, int fd)
{
    int fl;
    if (l->state == LK_S_LOST || (l->role == LK_DIALER && !l->have_sid))
        return -1;
    if (l->fd >= 0)
        l->ev |= drop(l, "replaced by a new carrier");
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    l->fd = fd;
    l->state = LK_S_HANDSHAKE;
    l->in_len = l->out_len = l->out_off = 0;
    l->got_hello = l->close_after_flush = 0;
    l->auth_done = l->hp_pending = 0;
    lk_auth_free(l->auth);
    l->auth = NULL;
    if (l->role == LK_DIALER) {
        int bad = send_hello(l) < 0;
        if (!bad && l->secret) {                    /* the first SASL message goes out with the HELLO */
            uint8_t m[256];
            size_t k;
            l->auth = lk_auth_new(l->secret, 0);
            bad = !l->auth || lk_auth_first(l->auth, m, sizeof m, &k) < 0 || queue(l, F_AUTH, m, k) < 0;
        }
        if (bad)
            l->ev |= drop(l, "the handshake could not begin");
    }
    return 0;
}

short lk_events(const lk_t *l)
{
    short ev = 0;
    if (l->fd < 0)
        return 0;
    if (l->in_len < sizeof l->in && l->cap - l->rx_len >= MAXDATA && l->cap - l->rx_len - l->rx_off + l->rx_off >= MAXDATA)
        ev |= POLLIN;
    if (l->out_len != l->out_off)
        ev |= POLLOUT;
    else if (l->state == LK_S_UP && (l->tx_sent < l->tx_total || l->rx_total != l->rx_acked_sent))
        ev |= POLLOUT;
    return ev;
}

/* tx is kept from the first unacknowledged byte: forget what the other end has taken in */
static void ack_to(lk_t *l, uint64_t upto)
{
    uint64_t n;
    if (upto > l->tx_total)
        upto = l->tx_total;
    if (upto <= l->tx_acked)
        return;
    n = upto - l->tx_acked;
    memmove(l->tx, l->tx + n, l->tx_len - (size_t)n);
    l->tx_len -= (size_t)n;
    l->tx_acked = upto;
    if (l->tx_sent < upto)
        l->tx_sent = upto;
}

static void become_up(lk_t *l)
{
    l->state = LK_S_UP;
    l->tx_sent = l->peer_rx;                        /* send again from where the other end says it stopped */
    l->rx_acked_sent = l->rx_total;
    l->ev |= LK_EV_UP;
}

/* act on the parsed HELLO (kept in hp_*): is it a new session, a continuation, one this end does not know, or a replacement?
 * With a secret this runs only after the dialer has proved it, so an unauthenticated peer changes nothing here. */
static int decide_hello(lk_t *l)
{
    uint8_t status, reply[9];
    int same = l->have_sid && memcmp(l->hp_sid, l->sid, LK_SID_LEN) == 0;
    l->hp_pending = 0;
    if (!same && !l->hp_fresh)
        status = ST_LOST;                           /* a continuation of a session this end does not have */
    else if (!same && l->have_sid) {
        l->ev |= LK_EV_RESET;                       /* a new session while the old one lives: the caller starts afresh */
        seterr(l, "a new session replaces the old one");
        queue(l, F_CLOSE, (const uint8_t *)"\5reset", 6);
        l->close_after_flush = 1;
        return 0;
    } else
        status = same ? ST_RESUMED : ST_NEW;
    l->peer_rx = l->hp_rx;
    if (status == ST_NEW) {
        memcpy(l->sid, l->hp_sid, LK_SID_LEN);
        l->have_sid = 1;
        snprintf(l->uname, sizeof l->uname, "%s", l->hp_un);
        snprintf(l->aname, sizeof l->aname, "%s", l->hp_an);
    }
    if (status == ST_NEW || status == ST_RESUMED) {
        if (status == ST_NEW && l->peer_rx != 0)
            status = ST_LOST;
        if (l->peer_rx < l->tx_acked || l->peer_rx > l->tx_total)
            status = ST_LOST;
    }
    reply[0] = status;
    put64(reply + 1, l->rx_total);
    if (queue(l, F_STATUS, reply, sizeof reply) < 0)
        return -1;
    if (status == ST_LOST || status == ST_REFUSED) {
        l->close_after_flush = 1;
        return 0;
    }
    ack_to(l, l->peer_rx);
    become_up(l);
    if (status == ST_NEW)
        l->ev |= LK_EV_NEW;
    return 0;
}

/* a frame arrived */
static int on_frame(lk_t *l, int type, const uint8_t *p, size_t n)
{
    switch (type) {
    case F_HELLO: {
        uint8_t reply[9];
        size_t o = 4, k;
        char mech[LK_NAME_MAX];
        if (l->role != LK_ACCEPTOR || l->got_hello || n < 4 + 1 + LK_SID_LEN + 8 + 1 || memcmp(p, "P9L1", 4) != 0) {
            seterr(l, "bad hello");
            return -1;
        }
        l->hp_fresh = p[o++] & 1;
        memcpy(l->hp_sid, p + o, LK_SID_LEN); o += LK_SID_LEN;
        l->hp_rx = get64(p + o); o += 8;
        k = p[o++];
        if (o + k + 1 > n || k >= sizeof mech) { seterr(l, "bad hello"); return -1; }
        memcpy(mech, p + o, k); mech[k] = '\0'; o += k;
        k = p[o++];
        if (o + k + 2 > n || k >= sizeof l->hp_un) { seterr(l, "bad hello"); return -1; }
        memcpy(l->hp_un, p + o, k); l->hp_un[k] = '\0'; o += k;
        k = get16(p + o); o += 2;
        if (o + k > n || k >= sizeof l->hp_an) { seterr(l, "bad hello"); return -1; }
        memcpy(l->hp_an, p + o, k); l->hp_an[k] = '\0';
        l->got_hello = 1;
        if (strcmp(mech, l->secret ? "SCRAM-SHA-256" : "EXTERNAL") != 0) {
            /* answered at once: it tells the peer nothing about the session and changes nothing */
            seterr(l, "the dialer's authentication mechanism is not the one this end requires");
            reply[0] = ST_REFUSED;
            put64(reply + 1, 0);
            if (queue(l, F_STATUS, reply, sizeof reply) < 0)
                return -1;
            l->close_after_flush = 1;
            return 0;
        }
        if (!l->secret)
            return decide_hello(l);
        lk_auth_free(l->auth);
        if (!(l->auth = lk_auth_new(l->secret, 1))) {
            seterr(l, "out of memory");
            return -1;
        }
        l->hp_pending = 1;                          /* decided when the proof has been checked (F_AUTH) */
        return 0;
    }
    case F_AUTH: {
        uint8_t out[512];
        size_t k;
        int done;
        if (l->state != LK_S_HANDSHAKE || !l->auth || l->auth_done || (l->role == LK_ACCEPTOR && !l->hp_pending)) {
            seterr(l, "unexpected authentication");
            return -1;
        }
        if (lk_auth_step(l->auth, p, n, out, sizeof out, &k, &done) < 0) {
            seterr(l, "authentication failed");
            if (l->role == LK_ACCEPTOR) {           /* nothing about the session changes: just end the carrier */
                l->hp_pending = 0;
                queue(l, F_CLOSE, (const uint8_t *)"\4auth", 5);
                l->close_after_flush = 1;
                return 0;
            }
            return -1;
        }
        if (k && queue(l, F_AUTH, out, k) < 0)
            return -1;
        if (done) {
            l->auth_done = 1;
            if (l->role == LK_ACCEPTOR)
                return decide_hello(l);
        }
        return 0;
    }
    case F_STATUS: {
        if (l->role != LK_DIALER || l->state != LK_S_HANDSHAKE || n != 9) {
            seterr(l, "unexpected status");
            return -1;
        }
        if (l->secret && !l->auth_done) {           /* an end that has not proved the secret decides nothing here */
            seterr(l, "the other end answered without proving the secret");
            return -1;
        }
        if (p[0] == ST_LOST) {
            l->state = LK_S_LOST;
            seterr(l, "the other end does not know this session");
            l->ev |= LK_EV_LOST;
            return -1;
        }
        if (p[0] == ST_REFUSED) {
            seterr(l, "the other end refused this carrier");
            return -1;
        }
        l->peer_rx = get64(p + 1);
        if (l->peer_rx < l->tx_acked || l->peer_rx > l->tx_total) {
            l->state = LK_S_LOST;
            seterr(l, "the other end's count does not fit this session");
            l->ev |= LK_EV_LOST;
            return -1;
        }
        l->fresh = 0;
        ack_to(l, l->peer_rx);
        become_up(l);
        return 0;
    }
    case F_DATA:
        if (l->state != LK_S_UP || n > MAXDATA || n > l->cap - l->rx_len) {
            seterr(l, "unexpected data");
            return -1;
        }
        if (l->rx_off && l->rx_off + l->rx_len + n > l->cap) {
            memmove(l->rx, l->rx + l->rx_off, l->rx_len);
            l->rx_off = 0;
        }
        memcpy(l->rx + l->rx_off + l->rx_len, p, n);
        l->rx_len += n;
        l->rx_total += n;
        return 0;
    case F_ACK:
        if (l->state != LK_S_UP || n != 8) {
            seterr(l, "unexpected ack");
            return -1;
        }
        ack_to(l, get64(p));
        return 0;
    case F_CLOSE:
        seterr(l, "the other end closed the carrier");
        return -1;
    default:
        seterr(l, "unknown frame");
        return -1;
    }
}

/* what to send next, if the output buffer is free */
static void fill_out(lk_t *l)
{
    if (l->out_len != l->out_off || l->fd < 0 || l->state != LK_S_UP)
        return;
    if (l->rx_total != l->rx_acked_sent) {
        uint8_t b[8];
        put64(b, l->rx_total);
        if (queue(l, F_ACK, b, 8) == 0)
            l->rx_acked_sent = l->rx_total;
        return;
    }
    if (l->tx_sent < l->tx_total) {
        size_t off = (size_t)(l->tx_sent - l->tx_acked), n = l->tx_len - off;
        if (n > MAXDATA)
            n = MAXDATA;
        if (queue(l, F_DATA, l->tx + off, n) == 0)
            l->tx_sent += n;
    }
}

static int flush_out(lk_t *l)
{
    while (l->fd >= 0 && l->out_off < l->out_len) {
        ssize_t k = send(l->fd, l->out + l->out_off, l->out_len - l->out_off, MSG_NOSIGNAL);
        if (k < 0 && errno == ENOTSOCK)
            k = write(l->fd, l->out + l->out_off, l->out_len - l->out_off);
        if (k < 0) {
            if (errno == EAGAIN || errno == EINTR)
                return 0;
            return -1;
        }
        l->out_off += (size_t)k;
    }
    if (l->out_off >= l->out_len) {
        l->out_off = l->out_len = 0;
        if (l->close_after_flush)
            return -1;
    }
    return 0;
}

static int process_in(lk_t *l)
{
    size_t used = 0;
    while (l->in_len - used >= HDR) {
        uint32_t n = get32(l->in + used + 1);
        int type = l->in[used];
        if (n > MAXFRAME) {
            seterr(l, "oversize frame");
            return -1;
        }
        if (l->in_len - used < HDR + n)
            break;
        if (type == F_DATA && (n > l->cap - l->rx_len))
            break;                                  /* wait until the reader has made room */
        if (l->close_after_flush)
            break;
        if (on_frame(l, type, l->in + used + HDR, n) < 0)
            return -1;
        used += HDR + n;
    }
    if (used) {
        memmove(l->in, l->in + used, l->in_len - used);
        l->in_len -= used;
    }
    return 0;
}

unsigned lk_pump(lk_t *l, short rev)
{
    unsigned ev;
    int bad = 0;
    if (l->fd >= 0) {
        if (rev & POLLNVAL)
            bad = 1;
        if (!bad && flush_out(l) < 0)
            bad = 1;
        if (!bad && (rev & (POLLIN | POLLHUP | POLLERR)) && l->in_len < sizeof l->in) {
            ssize_t k = read(l->fd, l->in + l->in_len, sizeof l->in - l->in_len);
            if (k > 0)
                l->in_len += (size_t)k;
            else if (k == 0) {
                if (l->in_len == 0 || 1)
                    bad = 2;
            } else if (errno != EAGAIN && errno != EINTR)
                bad = 1;
        }
        if (!bad && process_in(l) < 0)
            bad = 1;
        if (bad == 2) {                             /* end of file: what came before it was processed above */
            if (!l->err[0] || l->state != LK_S_LOST)
                seterr(l, "the carrier ended");
            if (process_in(l) < 0) {}
            l->ev |= drop(l, l->err);
        } else if (bad) {
            l->ev |= drop(l, l->err[0] ? l->err : "the carrier failed");
        } else {
            for (;;) {
                size_t before = l->out_len;
                fill_out(l);
                if (l->out_len == before && l->out_len == l->out_off)
                    break;
                if (flush_out(l) < 0) {
                    l->ev |= drop(l, "the carrier failed");
                    break;
                }
                if (l->out_off < l->out_len)
                    break;
            }
        }
    }
    ev = l->ev;
    l->ev = 0;
    return ev;
}

size_t lk_write(lk_t *l, const void *p, size_t n)
{
    size_t room = lk_space(l);
    if (n > room)
        n = room;
    memcpy(l->tx + l->tx_len, p, n);
    l->tx_len += n;
    l->tx_total += n;
    return n;
}

size_t lk_read(lk_t *l, void *p, size_t n)
{
    if (n > l->rx_len)
        n = l->rx_len;
    memcpy(p, l->rx + l->rx_off, n);
    l->rx_off += n;
    l->rx_len -= n;
    if (l->rx_len == 0)
        l->rx_off = 0;
    return n;
}

unsigned lk_tick(lk_t *l, time_t now, int hold)
{
    if (l->fd < 0 && l->state != LK_S_LOST && l->have_sid && now - l->down_since >= hold) {
        l->state = LK_S_LOST;
        seterr(l, "no carrier within the hold time");
        return LK_EV_LOST;
    }
    return 0;
}
