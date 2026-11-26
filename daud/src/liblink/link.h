/* link.h -- the resumable link: one 9P connection carried over a series of carriers (an ssh session, a socket) that may break.
 *
 * Both ends keep, per direction, a count of the bytes of the session sent and a buffer of those the other end has not yet
 * acknowledged.  When a carrier breaks and another is attached, each end says how many bytes it has received and the other
 * sends the rest, so the 9P connection above, and the fids the server keeps for it, never notice.  The engine does no waiting
 * of its own: the caller owns the file descriptor, polls it, and calls lk_pump.  See the frame layout in link.c.
 *
 * The dialer (the exporting side, daud) names the session and opens each carrier; the acceptor (the receiving side) keeps one
 * session and says whether a carrier continues it. */
#ifndef LINK_H
#define LINK_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define LK_SID_LEN   16
#define LK_NAME_MAX  64
#define LK_PATH_MAX  1024

enum { LK_DIALER = 0, LK_ACCEPTOR = 1 };

/* states */
enum { LK_S_DETACHED, LK_S_HANDSHAKE, LK_S_UP, LK_S_LOST };

/* events, ORed together, returned by lk_pump and lk_tick */
enum {
    LK_EV_UP    = 1,    /* a carrier finished its handshake: bytes flow (a resumed or a new session) */
    LK_EV_NEW   = 2,    /* acceptor: a new session began (lk_uname and lk_aname say what it is for) */
    LK_EV_DOWN  = 4,    /* the carrier was lost (the engine closed it); attach another within the hold time */
    LK_EV_LOST  = 8,    /* the session cannot continue: the hold time passed, or the other end does not know it */
    LK_EV_RESET = 16    /* acceptor: the dialer began a new session while this one lived; the caller starts afresh */
};

typedef struct lk lk_t;

lk_t *lk_new(int role, size_t cap);           /* cap: bytes of unacknowledged data kept each way (default 4 MB if 0) */
void  lk_free(lk_t *l);                       /* closes an attached carrier */

/* dialer: begin a session with a new random id, to be shown to the acceptor as UNAME and ANAME (what the mount asks diod for) */
int   lk_begin(lk_t *l, const char *uname, const char *aname);
/* a secret turns on SCRAM-SHA-256 for the handshake (both ends must have the same); NULL: EXTERNAL, which trusts the carrier */
int   lk_set_secret(lk_t *l, const char *secret);

/* the carrier: attach a connected descriptor (the engine makes it non-blocking and closes it when it is lost); then poll it for
 * lk_events() and call lk_pump with what poll said.  fd -1: nothing attached. */
int   lk_attach(lk_t *l, int fd);
int   lk_fd(const lk_t *l);
short lk_events(const lk_t *l);
unsigned lk_pump(lk_t *l, short revents);
void  lk_detach(lk_t *l);                     /* the caller saw the carrier fail; the engine closes it */

/* the 9P bytes above: lk_write takes up to n bytes for the other end (as many as fit), lk_read gives what has come */
size_t lk_write(lk_t *l, const void *p, size_t n);
size_t lk_read(lk_t *l, void *p, size_t n);
size_t lk_space(const lk_t *l);
size_t lk_avail(const lk_t *l);

/* time: after HOLD seconds with no carrier the session is lost; the caller calls this now and then */
unsigned lk_tick(lk_t *l, time_t now, int hold);

int   lk_state(const lk_t *l);
int   lk_unsent(const lk_t *l);               /* 1 if bytes wait to be sent or acknowledged (the carrier is not yet quiet) */
const char *lk_uname(const lk_t *l);
const char *lk_aname(const lk_t *l);
const char *lk_error(const lk_t *l);          /* why the last carrier or the session failed, for the log */
void  lk_sid(const lk_t *l, uint8_t sid[LK_SID_LEN]);

#endif
