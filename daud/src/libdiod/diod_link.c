/************************************************************\
 * Part of daud, a fork of the diod 9P server (see AUTHORS).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
\************************************************************/

/* diod_link.c -- an Nptrans over the resumable link.  recv waits through a dropped carrier instead of reporting the end of
 * the connection, so the connection's fids live on; the engine (lk_t) is not thread-safe, so every call into it is made
 * with the transport's mutex held, and one thread (diod_link_run) owns the carrier: it polls the descriptor, pumps the
 * engine, runs the carrier command again when it dies, and wakes the threads that wait for data or room. */
#if HAVE_CONFIG_H
#include "config.h"
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "src/libnpfs/npfs.h"
#include "src/liblink/link.h"
#include "diod_log.h"
#include "diod_link.h"

struct lt {
    lk_t            *lk;
    pthread_mutex_t  m;
    pthread_cond_t   cv;
    int              wake[2];
    int              closed;       /* the session is over: recv reports end of file, send fails */
    int              destroyed;    /* the connection has let go of the transport */
};

static void lt_wake (struct lt *t)
{
    char c = 0;
    if (write (t->wake[1], &c, 1) < 0) { /* the pipe is full: a wake-up is already waiting */ }
}

/* take exactly n bytes; 1 at the end of the session before any byte, -1 in the middle of the bytes */
static int lt_read_exact (struct lt *t, uint8_t *buf, size_t n)
{
    size_t got = 0;
    pthread_mutex_lock (&t->m);
    while (got < n) {
        size_t k = lk_read (t->lk, buf + got, n - got);
        if (k > 0) {
            got += k;
            lt_wake (t);                    /* room again: the engine may take in more */
            continue;
        }
        if (t->closed) {
            pthread_mutex_unlock (&t->m);
            return got ? -1 : 1;
        }
        pthread_cond_wait (&t->cv, &t->m);
    }
    pthread_mutex_unlock (&t->m);
    return 0;
}

static int lt_recv (Npfcall **fcp, u32 msize, void *a)
{
    struct lt *t = a;
    Npfcall *fc;
    u32 size;
    int rc;

    if (!(fc = np_alloc_fcall (msize))) {
        np_uerror (ENOMEM);
        return -1;
    }
    rc = lt_read_exact (t, fc->pkt, 4);
    if (rc == 1) {
        free (fc);
        *fcp = NULL;                        /* end of file */
        return 0;
    }
    if (rc < 0)
        goto eproto;
    size = np_peek_size (fc->pkt, 4);
    if (size < 7 || size > msize)
        goto eproto;
    if (lt_read_exact (t, fc->pkt + 4, size - 4) != 0)
        goto eproto;
    fc->size = size;
    *fcp = fc;
    return 0;
eproto:
    free (fc);
    np_uerror (EPROTO);
    return -1;
}

static int lt_send (Npfcall *fc, void *a)
{
    struct lt *t = a;
    size_t off = 0, size = fc->size;

    pthread_mutex_lock (&t->m);
    while (off < size) {
        size_t k = lk_write (t->lk, fc->pkt + off, size - off);
        if (k > 0) {
            off += k;
            lt_wake (t);
            continue;
        }
        if (t->closed) {
            pthread_mutex_unlock (&t->m);
            np_uerror (EPIPE);
            return -1;
        }
        pthread_cond_wait (&t->cv, &t->m);  /* no room: the other end has not yet taken in what was sent */
    }
    pthread_mutex_unlock (&t->m);
    return (int)size;
}

static void lt_destroy (void *a)
{
    struct lt *t = a;
    pthread_mutex_lock (&t->m);
    t->destroyed = 1;
    pthread_cond_broadcast (&t->cv);
    pthread_mutex_unlock (&t->m);
}

/* the carrier: the command, with its standard input and output on one end of a socket pair; returns our end */
static int spawn_carrier (const char *cmd, pid_t *pid)
{
    int sv[2];
    if (socketpair (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return -1;
    *pid = fork ();
    if (*pid < 0) {
        close (sv[0]);
        close (sv[1]);
        return -1;
    }
    if (*pid == 0) {
        sigset_t none;
        pid_t parent = getppid ();
        /* the carrier does not outlive daud: if daud is killed, ssh is told to end too */
        prctl (PR_SET_PDEATHSIG, SIGTERM);
        if (getppid () != parent)
            _exit (126);
        sigemptyset (&none);
        sigprocmask (SIG_SETMASK, &none, NULL);
        signal (SIGTERM, SIG_DFL);
        signal (SIGHUP, SIG_DFL);
        signal (SIGPIPE, SIG_DFL);
        if (dup2 (sv[1], 0) < 0 || dup2 (sv[1], 1) < 0)
            _exit (126);
        execl ("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit (127);
    }
    close (sv[1]);
    return sv[0];
}

int diod_link_once = 0;

static void run_session (struct lt *t, const char *cmd, int hold,
                         volatile int *shutdown, const sigset_t *mask)
{
    pid_t child = 0;
    time_t next_try = 0;
    int delay = 1, was_up = 0, ever_up = 0;
    time_t began = time (NULL);

    for (;;) {
        struct pollfd p[2];
        struct timespec ts = { 1, 0 };
        time_t now = time (NULL);
        unsigned ev;

        pthread_mutex_lock (&t->m);
        if (*shutdown || lk_state (t->lk) == LK_S_LOST
            || (diod_link_once && !ever_up && now - began >= hold)) {
            pthread_mutex_unlock (&t->m);
            break;
        }
        if (lk_fd (t->lk) < 0 && now >= next_try) {
            int fd = spawn_carrier (cmd, &child);
            if (fd >= 0 && lk_attach (t->lk, fd) == 0) {
                was_up = 0;
            } else {
                if (fd >= 0)
                    close (fd);
                msg ("link: cannot run the carrier command: %s", strerror (errno));
                next_try = now + delay;
            }
        }
        p[0].fd = t->wake[0]; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = lk_fd (t->lk); p[1].events = lk_events (t->lk); p[1].revents = 0;
        pthread_mutex_unlock (&t->m);

        if (ppoll (p, 2, &ts, mask) < 0 && errno != EINTR)
            err_exit ("ppoll");
        if (p[0].revents & POLLIN) {
            char junk[64];
            if (read (t->wake[0], junk, sizeof junk) < 0) { /* empty: nothing to drain */ }
        }
        if (child > 0 && waitpid (child, NULL, WNOHANG) == child)
            child = 0;

        pthread_mutex_lock (&t->m);
        now = time (NULL);
        ev = lk_pump (t->lk, p[1].fd >= 0 ? p[1].revents : 0);
        ev |= lk_tick (t->lk, now, hold);
        if (ev & LK_EV_UP) {
            was_up = ever_up = 1;
            delay = 1;
            msg ("link: up");
        }
        if (ev & LK_EV_DOWN) {
            msg ("link: carrier lost (%s); the connection waits up to %d seconds", lk_error (t->lk), hold);
            if (child > 0)
                kill (child, SIGTERM);
            next_try = now + delay;
            if (!was_up && delay < 30)
                delay *= 2;
        }
        if (ev & LK_EV_LOST)
            msg ("link: the session is over (%s)", lk_error (t->lk));
        pthread_cond_broadcast (&t->cv);
        pthread_mutex_unlock (&t->m);
    }
    if (child > 0) {
        kill (child, SIGTERM);
        waitpid (child, NULL, 0);
    }
}

void diod_link_run (Npsrv *srv, const char *cmd, int hold, const char *uname,
                    const char *aname, const char *secret,
                    volatile int *shutdown, const sigset_t *waitmask)
{
    while (!*shutdown) {
        struct lt *t = calloc (1, sizeof *t);
        Nptrans *trans;
        Npconn *conn;

        if (!t)
            err_exit ("calloc");
        pthread_mutex_init (&t->m, NULL);
        pthread_cond_init (&t->cv, NULL);
        if (pipe2 (t->wake, O_NONBLOCK | O_CLOEXEC) < 0)
            err_exit ("pipe2");
        if (!(t->lk = lk_new (LK_DIALER, 0)) || lk_begin (t->lk, uname, aname) < 0
            || (secret && lk_set_secret (t->lk, secret) < 0))
            msg_exit ("link: cannot start a session");
        if (!(trans = np_trans_create (t, lt_recv, lt_send, lt_destroy)))
            errn_exit (np_rerror (), "np_trans_create");
        if (!(conn = np_conn_create (srv, trans, "link", 0))) {
            errn (np_rerror (), "link: cannot create the connection");
            sleep (1);
            lk_free (t->lk);
            close (t->wake[0]); close (t->wake[1]);
            free (t);
            continue;
        }
        msg ("link: a new session for %s, serving %s", uname, aname);
        run_session (t, cmd, hold, shutdown, waitmask);

        /* the session is over: the connection sees the end of file and lets go of the transport */
        pthread_mutex_lock (&t->m);
        t->closed = 1;
        pthread_cond_broadcast (&t->cv);
        while (!t->destroyed)
            pthread_cond_wait (&t->cv, &t->m);
        pthread_mutex_unlock (&t->m);
        lk_free (t->lk);
        close (t->wake[0]);
        close (t->wake[1]);
        free (t);
        if (diod_link_once)
            *shutdown = 1;
    }
}
