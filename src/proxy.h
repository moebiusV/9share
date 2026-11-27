/* proxy.h -- the 9P-aware part of the relay: a state machine with no I/O of its own.
 *
 * The relay sits between the kernel's 9p client and the link to daud.  The kernel sends one read at a time and waits for the answer, so over
 * a link with a round trip of 10 to 25 ms a program that reads 32 KB at a time gets about 1.3 MB/s.  scp gets 33 to 39 MB/s over the same ssh
 * because it keeps up to 64 requests of 32 KB outstanding (OpenSSH's sftp-client.c).  This module does the same for 9P: when it sees a
 * file read sequentially it asks daud for the next blocks before the kernel does, and answers the kernel's next reads from what has already
 * arrived.
 *
 * Bytes go in at one side and come out at the other, in whole messages: px_from_kernel takes what the kernel wrote and fills the queue for the
 * server, px_from_server the other way round.  Everything it does not understand it passes on unchanged; everything it does look at is
 * bounds-checked.  A framing error (a message that cannot be one) makes both calls return -1: the stream cannot be followed any more, and the
 * caller ends the session instead of guessing.
 *
 * What may be stale: data the module read ahead is older than a change made on the exporting machine by the time it sat in the queue (3 s at
 * most, see px_conf.ttl) and only for blocks past the program's position.  A write, a truncate or any change of the tree through this relay
 * drops what was read ahead; a block is used once. */
#ifndef NINESHARE_PROXY_H
#define NINESHARE_PROXY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct px_conf {
    size_t   window_bytes;      /* how far ahead to read, in bytes (scp: 64 x 32 KB = 2 MiB); 0: never read ahead */
    unsigned max_requests;      /* at most this many read-aheads outstanding for one file (scp: 64) */
    size_t   total_bytes;       /* all that is held or asked for, over all files */
    int      ttl;               /* seconds a block read ahead may wait to be used */
};

#define PX_DEFAULT_WINDOW  (2u * 1024 * 1024)
#define PX_DEFAULT_REQUESTS 64u
#define PX_DEFAULT_TOTAL   (16u * 1024 * 1024)
#define PX_DEFAULT_TTL     3

/* a queue of bytes for one side */
struct px_buf {
    uint8_t *p;
    size_t   len, off, cap;     /* the bytes are p[off .. len) */
};

typedef struct px px_t;

px_t *px_new(const struct px_conf *c);
void  px_free(px_t *x);
void  px_reset(px_t *x);                              /* a new session: forget the fids, the tags and everything read ahead */
void  px_set_window(px_t *x, size_t window_bytes);    /* change how far ahead to read, at any time; 0 stops and drops what was read ahead */
void  px_tick(px_t *x, time_t now);                   /* the time, now and then (drops what has waited too long) */

int   px_from_kernel(px_t *x, const uint8_t *p, size_t n);    /* 0, or -1: the stream is broken */
int   px_from_server(px_t *x, const uint8_t *p, size_t n);

struct px_buf *px_to_server(px_t *x);                 /* what is to be sent on to daud */
struct px_buf *px_to_kernel(px_t *x);                 /* what is to be written to the kernel's end */
static inline size_t px_buf_avail(const struct px_buf *b) { return b->len - b->off; }
static inline const uint8_t *px_buf_data(const struct px_buf *b) { return b->p + b->off; }
void  px_buf_consume(struct px_buf *b, size_t n);

/* counters, for tests and the log */
struct px_stats {
    unsigned long hits;         /* kernel reads answered from what was read ahead (parked ones included) */
    unsigned long issued;       /* reads asked for ahead of the kernel */
    unsigned long dropped;      /* blocks thrown away unused */
    unsigned long forwarded;    /* kernel reads sent on to daud as they were */
    size_t        held;         /* bytes held or asked for now */
    unsigned      ahead;        /* blocks asked for or ready now, over all files */
};
void  px_stats(const px_t *x, struct px_stats *s);

#endif
