/* p9client.h -- a small 9P2000.L client, enough to test a server: messages are built by hand, one request at a time. */
#ifndef NINESHARE_P9CLIENT_H
#define NINESHARE_P9CLIENT_H
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define NOFID 0xFFFFFFFFu
#define NONUNAME 0xFFFFFFFFu

struct msg { uint8_t b[8300]; size_t n; };

static void put8(struct msg *m, uint8_t v) { m->b[m->n++] = v; }
static void put16(struct msg *m, uint16_t v) { put8(m, v & 255); put8(m, v >> 8); }
static void put32(struct msg *m, uint32_t v) { put16(m, v & 0xFFFF); put16(m, v >> 16); }
static void put64(struct msg *m, uint64_t v) { put32(m, (uint32_t)v); put32(m, (uint32_t)(v >> 32)); }
static void puts9(struct msg *m, const char *s) { size_t l = strlen(s); put16(m, (uint16_t)l); memcpy(m->b + m->n, s, l); m->n += l; }

static int readn(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t k = read(fd, (char *)buf + got, n - got);
        if (k <= 0)
            return -1;
        got += (size_t)k;
    }
    return 0;
}

/* send a request; returns the reply type, with the body (after the tag) in r; -1 on a broken connection */
static int rpc(int fd, uint8_t type, const struct msg *body, struct msg *r)
{
    struct msg q;
    uint8_t h[4];
    uint32_t size;
    q.n = 0;
    put32(&q, 0);
    put8(&q, type);
    put16(&q, 1);
    memcpy(q.b + q.n, body->b, body->n);
    q.n += body->n;
    q.b[0] = q.n & 255; q.b[1] = (q.n >> 8) & 255; q.b[2] = (q.n >> 16) & 255; q.b[3] = (q.n >> 24) & 255;
    if (write(fd, q.b, q.n) != (ssize_t)q.n || readn(fd, h, 4) < 0)
        return -1;
    size = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
    if (size < 7 || size > sizeof r->b + 4 || readn(fd, r->b, size - 4) < 0)
        return -1;
    r->n = size - 4;
    {
        int t = r->b[0];
        memmove(r->b, r->b + 3, r->n - 3);
        r->n -= 3;
        return t;
    }
}

static int attach(int fd, uint32_t n_uname, const char *aname)
{
    struct msg q = { .n = 0 }, r;
    put32(&q, 1); put32(&q, NOFID); puts9(&q, "nobody"); puts9(&q, aname); put32(&q, n_uname);
    return rpc(fd, 104, &q, &r);
}

static int walk(int fd, uint32_t fid, uint32_t newfid, const char *name)
{
    struct msg q = { .n = 0 }, r;
    put32(&q, fid); put32(&q, newfid); put16(&q, name ? 1 : 0);
    if (name)
        puts9(&q, name);
    return rpc(fd, 110, &q, &r);
}

#endif
