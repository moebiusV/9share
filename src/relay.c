/* relay.c -- the receiving end of a link, two small programs the root daemon and ssh run:
 *
 *   9share relay NAME HOLD KFD CTLFD   (run as the mount's user by the root daemon, one per mount)
 *       keeps the session: listens on ~/.9share/NAME.link, takes each carrier the exporter's ssh brings (through `9share link`),
 *       and joins the link to KFD, the end of a socket pair whose other end the kernel's 9p client was given.  When the carrier
 *       breaks the kernel is not told: its requests wait, and the next carrier carries on.  What it learns goes to CTLFD, a line
 *       at a time: "NEW uname aname" (a session began: the daemon mounts now), "RESET", "LOST" or "KCLOSED" (it ends), "LOG text".
 *   9share link NAME                   (run by ssh on the exporter's behalf)
 *       joins its standard input and output to ~/.9share/NAME.link: it is only a pipe, the link's rules are in the relay.
 *
 * The other direction (an import: this machine shows a directory of another, and its ssh dials out) has three more, on the machine
 * that has the files, started there by ssh:
 *   9share offer NAME DIR [--hold N]   listens on ~/.9share/NAME.offer, starts a daud there if none is running (daud --dial --once), takes
 *       the one connection that daud's carrier makes and joins ssh's standard input and output to it.  Only a pipe, as `link` is.
 *   9share carry NAME                   daud's carrier command: joins its standard input and output to ~/.9share/NAME.offer, waiting a
 *       while for it to appear. */
#include "9share.h"
#include "../daud/src/liblink/link.h"
#include "proxy.h"
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static void on_term(int sig) { (void)sig; stopping = 1; }

static int link_path(const char *name, struct sockaddr_un *a)
{
    const char *home = getenv("HOME");
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (!home || !nineshare_valid_name(name))
        return -1;
    if (snprintf(a->sun_path, sizeof a->sun_path, "%s/.9share/%s.link", home, name) >= (int)sizeof a->sun_path)
        return -1;
    return 0;
}

static void say(int ctl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int ctl, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n++] = '\n';
    if (write(ctl, buf, (size_t)n) < 0) { /* the daemon is gone: we are about to be told so by the signal */ }
}

/* The setting of read-ahead (/etc/9share/prefetch-kb) may change while the mount is in use: look at the file about once a second and apply what is there.
 * Called with PX NULL once at the start, to take note of what the file is. */
static void watch_prefetch(px_t *px, int *pkb, int ctl)
{
    static struct stat last;
    static int have_last, started;
    static time_t checked;
    char path[P9_LINE + 32];
    struct stat st;
    int have, kb, rc;
    time_t now = time(NULL);
    if (started && now == checked)
        return;
    checked = now;
    snprintf(path, sizeof path, "%s/prefetch-kb", p9_sysconf_dir());
    have = stat(path, &st) == 0;
    if (started && have == have_last && (!have || (st.st_mtime == last.st_mtime && st.st_size == last.st_size && st.st_ino == last.st_ino)))
        return;
    have_last = have;
    if (have)
        last = st;
    if (!started) {
        started = 1;
        return;
    }
    rc = p9_prefetch_kb(&kb);
    if (rc < 0) {
        say(ctl, "LOG %s is not usable: read-ahead stays at %d KB", path, *pkb);
        return;
    }
    if (kb != *pkb) {
        *pkb = kb;
        px_set_window(px, (size_t)kb * 1024);
        say(ctl, "LOG read-ahead now %d KB", kb);
    }
}

int p9_relay(int argc, char **argv)
{
    struct sockaddr_un a;
    struct sigaction sa;
    char dir[P9_LINE], *slash;
    int name_ok, hold, kfd, ctl, lsock;
    lk_t *lk;
    uint8_t kout[65536], kin[16384];
    size_t kout_len = 0, kout_off = 0;
    const char *why = NULL;
    uid_t me = getuid();
    px_t *px = NULL;                                    /* the 9P-aware part, unless read-ahead is off (prefetch-kb 0): then bytes only */
    int pkb = 0;
#define PXQ_SERVER (2u * 1024 * 1024)                   /* what may wait to go to the link before the kernel is not read any more */
#define PXQ_KERNEL (4u * 1024 * 1024)                   /* ... and what may wait to go to the kernel before the link is not read any more */

    if (argc != 4)
        return 2;
    hold = atoi(argv[1]);
    kfd = atoi(argv[2]);
    ctl = atoi(argv[3]);
    name_ok = link_path(argv[0], &a) == 0;
    if (!name_ok || hold < 1)
        return 2;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    snprintf(dir, sizeof dir, "%s", a.sun_path);
    if ((slash = strrchr(dir, '/')))
        *slash = '\0';
    p9_mkdir_p(dir, 0700);
    unlink(a.sun_path);
    umask(077);
    if ((lsock = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) < 0
        || bind(lsock, (struct sockaddr *)&a, sizeof a) < 0 || listen(lsock, 4) < 0) {
        say(ctl, "LOG cannot listen on %s: %s", a.sun_path, strerror(errno));
        return 1;
    }
    fcntl(kfd, F_SETFL, fcntl(kfd, F_GETFL) | O_NONBLOCK);
    if (!(lk = lk_new(LK_ACCEPTOR, 0)))
        return 1;
    if (p9_prefetch_kb(&pkb) < 0)
        say(ctl, "LOG %s/prefetch-kb is not usable: %d KB is used", p9_sysconf_dir(), pkb);
    {   /* the proxy is always there, even at 0 KB, so that read-ahead can be switched on and off while files are open */
        struct px_conf pc = { (size_t)pkb * 1024, PX_DEFAULT_REQUESTS, PX_DEFAULT_TOTAL, PX_DEFAULT_TTL };
        if (!(px = px_new(&pc)))
            say(ctl, "LOG out of memory for the proxy: the bytes are carried without looking at them, and nothing is read ahead");
    }
    say(ctl, "LOG %s", !px ? "not reading ahead" : pkb ? "reading ahead (prefetch-kb)" : "not reading ahead (prefetch-kb is 0)");
    watch_prefetch(NULL, NULL, 0);                      /* what the file is now: later changes are noticed against it */

    while (!stopping && !why) {
        struct pollfd p[3];
        unsigned ev;
        int n;
        short kev = 0;

        if (px) {
            struct px_buf *tk = px_to_kernel(px), *ts = px_to_server(px);
            px_tick(px, time(NULL));
            watch_prefetch(px, &pkb, ctl);
            /* what the link has for the kernel goes through the proxy, which may answer some of the kernel's reads by itself */
            while (lk_avail(lk) > 0 && px_buf_avail(tk) < PXQ_KERNEL) {
                uint8_t tmp[65536];
                size_t got = lk_read(lk, tmp, sizeof tmp);
                if (got == 0)
                    break;
                if (px_from_server(px, tmp, got) < 0) {
                    say(ctl, "LOG the 9P stream from daud cannot be followed: the session ends");
                    why = "RESET";
                    break;
                }
            }
            if (why)
                break;
            if (px_buf_avail(tk) > 0) {
                ssize_t k = write(kfd, px_buf_data(tk), px_buf_avail(tk));
                if (k > 0)
                    px_buf_consume(tk, (size_t)k);
                else if (k < 0 && errno != EAGAIN && errno != EINTR) {
                    why = "KCLOSED";
                    break;
                }
            }
            if (px_buf_avail(tk) > 0)
                kev |= POLLOUT;
            /* and what is for daud goes to the link as it has room */
            if (px_buf_avail(ts) > 0 && lk_space(lk) > 0)
                px_buf_consume(ts, lk_write(lk, px_buf_data(ts), px_buf_avail(ts)));
            if (px_buf_avail(ts) < PXQ_SERVER)
                kev |= POLLIN;
        } else {
            /* what the link has for the kernel */
            while (kout_off == kout_len && lk_avail(lk) > 0) {
                kout_len = lk_read(lk, kout, sizeof kout);
                kout_off = 0;
            }
            if (kout_off < kout_len) {
                ssize_t k = write(kfd, kout + kout_off, kout_len - kout_off);
                if (k > 0) {
                    kout_off += (size_t)k;
                    if (kout_off == kout_len)
                        kout_off = kout_len = 0;
                } else if (k < 0 && errno != EAGAIN && errno != EINTR) {
                    why = "KCLOSED";
                    break;
                }
            }
            if (kout_off < kout_len)
                kev |= POLLOUT;
            if (lk_space(lk) > 0)
                kev |= POLLIN;
        }
        p[0].fd = lsock; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = lk_fd(lk); p[1].events = lk_events(lk); p[1].revents = 0;
        p[2].fd = kfd; p[2].events = kev; p[2].revents = 0;
        n = poll(p, 3, 1000);
        if (n < 0 && errno != EINTR)
            break;

        if (p[0].revents & POLLIN) {                    /* a carrier: ssh, through `9share link`, as this user and no other */
            int c = accept4(lsock, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (c >= 0) {
                struct ucred cr;
                socklen_t l = sizeof cr;
                if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0 || cr.uid != me) {
                    say(ctl, "LOG a carrier from another user was refused");
                    close(c);
                } else {
                    /* the secret is read for each carrier, so that `9share pair` takes effect without a restart; a file that
                     * is there but unusable refuses the carrier (never a fall back to trusting it) */
                    char secret[256], spath[P9_LINE], swhy[P9_LINE + 64];
                    int sr = p9_secret_load(getenv("HOME"), argv[0], secret, sizeof secret, spath, sizeof spath, swhy, sizeof swhy);
                    if (sr < 0) {
                        say(ctl, "LOG a carrier was refused: %s", swhy);
                        close(c);
                    } else if (lk_set_secret(lk, sr == 0 ? secret : NULL) < 0 || lk_attach(lk, c) < 0) {
                        say(ctl, "LOG a carrier could not be taken");
                        close(c);
                    }
                    memset(secret, 0, sizeof secret);
                }
            }
        }
        if (p[2].revents & POLLIN) {
            size_t room = px ? PXQ_SERVER : lk_space(lk);
            ssize_t k = read(kfd, kin, room < sizeof kin ? room : sizeof kin);
            if (k > 0 && px) {
                if (px_from_kernel(px, kin, (size_t)k) < 0) {
                    say(ctl, "LOG the 9P stream from the kernel cannot be followed: the session ends");
                    why = "RESET";
                }
            } else if (k > 0)
                lk_write(lk, kin, (size_t)k);
            else if (k == 0 || (errno != EAGAIN && errno != EINTR))
                why = "KCLOSED";
        } else if (p[2].revents & (POLLHUP | POLLERR))
            why = "KCLOSED";
        ev = lk_pump(lk, p[1].fd >= 0 ? p[1].revents : 0);
        ev |= lk_tick(lk, time(NULL), hold);
        if (ev & LK_EV_NEW)                             /* (the proxy is not reset here: this relay has one session, it ends at RESET and LOST, and the kernel
                                                         * may have sent its first messages before the link was up: they are in flight and their answers are due) */
            say(ctl, "NEW %s %s", lk_uname(lk), lk_aname(lk));
        if (ev & LK_EV_UP)
            say(ctl, "LOG link up");
        if (ev & LK_EV_DOWN)
            say(ctl, "LOG carrier lost (%s); waiting up to %d seconds", lk_error(lk), hold);
        if (ev & LK_EV_RESET)
            why = "RESET";
        if (ev & LK_EV_LOST)
            why = "LOST";
    }
    if (px) {
        struct px_stats st;
        px_stats(px, &st);
        say(ctl, "LOG read-ahead: used=%lu issued=%lu dropped=%lu forwarded=%lu", st.hits, st.issued, st.dropped, st.forwarded);
    }
    if (why)
        say(ctl, "%s", why);
    lk_free(lk);
    px_free(px);
    unlink(a.sun_path);
    return 0;
}

static int pump(int fd);

int p9_link(int argc, char **argv)
{
    struct sockaddr_un a;
    int fd;

    if (argc != 1 || link_path(argv[0], &a) < 0) {
        fprintf(stderr, "usage: 9share link NAME\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        fprintf(stderr, "9share: no mount named '%s' is waiting on this machine (%s)\n", argv[0], strerror(errno));
        return 1;
    }
    return pump(fd);
}

/* join standard input and output to FD until either side ends */
static int pump(int fd)
{
    uint8_t buf[16384];
    size_t in_len = 0, in_off = 0, out_len = 0, out_off = 0;
    uint8_t obuf[16384];
    int in_open = 1;

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    fcntl(1, F_SETFL, fcntl(1, F_GETFL) | O_NONBLOCK);
    for (;;) {
        struct pollfd p[4];
        int np = 0, i_in = -1, i_sock = -1, i_out = -1;
        if (in_open && in_off == in_len) {
            p[np].fd = 0; p[np].events = POLLIN; i_in = np++;
        }
        p[np].fd = fd; p[np].events = (out_len == out_off ? POLLIN : 0) | (in_off < in_len ? POLLOUT : 0); i_sock = np++;
        if (out_off < out_len) {
            p[np].fd = 1; p[np].events = POLLOUT; i_out = np++;
        }
        if (poll(p, (nfds_t)np, -1) < 0 && errno != EINTR)
            return 1;
        if (i_in >= 0 && (p[i_in].revents & (POLLIN | POLLHUP))) {
            ssize_t k = read(0, buf, sizeof buf);
            if (k > 0) { in_len = (size_t)k; in_off = 0; }
            else if (k == 0) in_open = 0;
            else if (errno != EAGAIN && errno != EINTR) return 1;
        }
        if (in_off < in_len) {
            ssize_t k = write(fd, buf + in_off, in_len - in_off);
            if (k > 0) in_off += (size_t)k;
            else if (k < 0 && errno != EAGAIN && errno != EINTR) return 1;
        }
        if (!in_open && in_off == in_len)
            shutdown(fd, SHUT_WR);
        if (p[i_sock].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (out_len == out_off) {
                ssize_t k = read(fd, obuf, sizeof obuf);
                if (k > 0) { out_len = (size_t)k; out_off = 0; }
                else if (k == 0) return 0;
                else if (errno != EAGAIN && errno != EINTR) return 1;
            }
        }
        if (out_off < out_len) {
            ssize_t k = write(1, obuf + out_off, out_len - out_off);
            if (k > 0) out_off += (size_t)k;
            else if (k < 0 && errno != EAGAIN && errno != EINTR) return 1;
            if (out_off == out_len) out_off = out_len = 0;
        }
        (void)i_out;
    }
}

/* ---- the other direction: offer and carry ---- */

static int offer_path(const char *name, const char *suffix, struct sockaddr_un *a)
{
    const char *home = getenv("HOME");
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    if (!home || !nineshare_valid_name(name))
        return -1;
    if (snprintf(a->sun_path, sizeof a->sun_path, "%s/.9share/%s.%s", home, name, suffix) >= (int)sizeof a->sun_path)
        return -1;
    return 0;
}

/* a connection to the relay of the mount NAME on this machine (the importer's ssh gets it as its standard input and output); -1 if there is none */
int p9_link_connect(const char *name)
{
    struct sockaddr_un a;
    int fd;
    if (link_path(name, &a) < 0 || (fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int p9_carry(int argc, char **argv)
{
    struct sockaddr_un a;
    int fd, i;
    if (argc != 1 || offer_path(argv[0], "offer", &a) < 0) {
        fprintf(stderr, "usage: 9share carry NAME\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    for (i = 0; i < 100; i++) {         /* the importer's ssh may be a moment behind daud's start: wait up to 20 seconds */
        if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0)
            return 1;
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0)
            return pump(fd);
        close(fd);
        usleep(200000);
    }
    fprintf(stderr, "9share: nothing is offering '%s' on this machine\n", argv[0]);
    return 1;
}

/* the state of the daud that serves NAME here: the record is what it was started with, so that a changed request starts it again */
struct offerreq {
    char name[80], dir[P9_LINE], rec[P9_LINE * 2 + 64], lockp[P9_LINE + 32], recp[P9_LINE + 32], pidp[P9_LINE + 32], log[P9_LINE + 64];
    char spath[P9_LINE];
    int  hold, has_secret;
};

static void daud_supervise(const struct offerreq *q, const char *self)
{
    /* runs detached from ssh: its own session, no descriptor of ssh's, and it holds the lock for as long as daud runs */
    char uid[32], hold[16], cmd[P9_LINE + 64];
    char *argv[20];
    int ac = 0, lk, fd, i, st;
    pid_t pid;
    char buf[32];
    setsid();
    for (i = 3; i < 256; i++)
        close(i);
    if ((lk = p9_lock(q->lockp)) < 0)
        _exit(0);                                   /* someone else is starting it */
    snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    p9_write_file(q->pidp, buf, strlen(buf), 0600);
    p9_write_file(q->recp, q->rec, strlen(q->rec), 0600);
    fd = open("/dev/null", O_RDONLY);
    dup2(fd, 0);
    fd = open(q->log, O_WRONLY | O_CREAT | O_APPEND, 0600);
    dup2(fd, 1);
    dup2(fd, 2);
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    snprintf(hold, sizeof hold, "%d", q->hold);
    snprintf(cmd, sizeof cmd, "exec '%s' carry %s", self, q->name);
    argv[ac++] = (char *)p9_daud();
    argv[ac++] = "-f"; argv[ac++] = "-n"; argv[ac++] = "-N";
    argv[ac++] = "-u"; argv[ac++] = uid;
    argv[ac++] = "-e"; argv[ac++] = (char *)q->dir;
    argv[ac++] = "--dial"; argv[ac++] = cmd;
    argv[ac++] = "--hold"; argv[ac++] = hold;
    argv[ac++] = "--once";
    if (q->has_secret) {
        argv[ac++] = "--link-secret-file";
        argv[ac++] = (char *)q->spath;
    }
    argv[ac++] = "-L"; argv[ac++] = "stderr";
    argv[ac] = NULL;
    pid = fork();
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    signal(SIGPIPE, SIG_IGN);
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_term;
        sigaction(SIGTERM, &sa, NULL);
    }
    while (pid > 0 && waitpid(pid, &st, 0) != pid) {
        if (stopping) {                              /* told to end (a changed request): daud ends with us */
            kill(pid, SIGTERM);
            stopping = 0;
        }
    }
    unlink(q->pidp);
    close(lk);
    _exit(0);
}

/* make sure a daud runs for the request; a different one running is ended first */
static void ensure_daud(const struct offerreq *q, const char *self)
{
    int i;
    if (p9_lock_held(q->lockp)) {
        size_t n;
        char *rec = p9_read_file(q->recp, &n);
        int same = rec && !strcmp(rec, q->rec);
        free(rec);
        if (same)
            return;
        {
            char *pid = p9_read_file(q->pidp, &n);
            if (pid && atoi(pid) > 1)
                kill(atoi(pid), SIGTERM);
            free(pid);
        }
        for (i = 0; i < 100 && p9_lock_held(q->lockp); i++)
            usleep(100000);
        if (p9_lock_held(q->lockp))
            return;
    }
    {
        pid_t p = fork();
        if (p == 0) {
            pid_t p2 = fork();                       /* not a child of offer: it must outlive it */
            if (p2 == 0)
                daud_supervise(q, self);
            _exit(0);
        }
        if (p > 0)
            waitpid(p, NULL, 0);
    }
}

int p9_offer(int argc, char **argv)
{
    struct offerreq q;
    struct sockaddr_un a;
    struct stat st;
    char rp[PATH_MAX], path[PATH_MAX], olock[P9_LINE + 32], opid[P9_LINE + 32], statedir[P9_LINE], why[P9_LINE + 64], secret[256];
    const char *home = getenv("HOME"), *name;
    char *self;
    int lsock, olk = -1, i, sr, fd = -1;
    time_t t0;

    q.hold = 600;
    if (argc < 2 || !home) {
usage:
        fprintf(stderr, "usage: 9share offer NAME DIR [--hold N]\n");
        return 2;
    }
    name = argv[0];
    if (argc == 4 && !strcmp(argv[2], "--hold"))
        q.hold = atoi(argv[3]);
    else if (argc != 2)
        goto usage;
    if (q.hold < 1 || q.hold > 86400 || !nineshare_valid_rpath(argv[1]) || offer_path(name, "offer", &a) < 0)
        goto usage;
    if (argv[1][0] == '/')
        snprintf(path, sizeof path, "%s", argv[1]);
    else
        snprintf(path, sizeof path, "%s/%s", home, argv[1]);
    if (!realpath(path, rp) || stat(rp, &st) < 0 || !S_ISDIR(st.st_mode) || strlen(rp) >= sizeof q.dir || !nineshare_valid_path(rp)) {
        fprintf(stderr, "9share: %s is not a directory that can be shared (it needs letters, digits and . _ + @ - / only)\n", argv[1]);
        return 1;
    }
    snprintf(q.name, sizeof q.name, "%s", name);
    snprintf(q.dir, sizeof q.dir, "%s", rp);
    sr = p9_secret_load(NULL, name, secret, sizeof secret, q.spath, sizeof q.spath, why, sizeof why);
    memset(secret, 0, sizeof secret);
    if (sr < 0) {
        fprintf(stderr, "9share: %s\n", why);
        return 1;
    }
    q.has_secret = sr == 0;
    {
        struct stat ss;
        long long mt = 0, sz = 0;
        if (q.has_secret && stat(q.spath, &ss) == 0) { mt = ss.st_mtim.tv_sec * 1000000000LL + ss.st_mtim.tv_nsec; sz = ss.st_size; }
        snprintf(q.rec, sizeof q.rec, "%s\n%d\n%d %lld %lld\n", q.dir, q.hold, q.has_secret, mt, sz);
    }
    snprintf(statedir, sizeof statedir, "%s/.9share", home);
    p9_mkdir_p(statedir, 0700);
    snprintf(q.lockp, sizeof q.lockp, "%s/%s.daud.lock", statedir, name);
    snprintf(q.recp, sizeof q.recp, "%s/%s.daud.rec", statedir, name);
    snprintf(q.pidp, sizeof q.pidp, "%s/%s.daud.pid", statedir, name);
    snprintf(q.log, sizeof q.log, "%s/%s.daud.log", statedir, name);
    snprintf(olock, sizeof olock, "%s/%s.offer.lock", statedir, name);
    snprintf(opid, sizeof opid, "%s/%s.offer.pid", statedir, name);
    signal(SIGPIPE, SIG_IGN);

    /* an older offer whose ssh is gone but which has not noticed yet is ended: this one is the importer's current connection */
    for (i = 0; i < 100 && (olk = p9_lock(olock)) < 0; i++) {
        if (i == 0) {
            char *pid = p9_read_file(opid, NULL), cl[64], *c;
            int p = pid ? atoi(pid) : 0;
            free(pid);
            if (p > 1) {
                snprintf(cl, sizeof cl, "/proc/%d/cmdline", p);
                if ((c = p9_read_file(cl, NULL)) != NULL) {
                    if (strstr(c, "offer"))
                        kill(p, SIGTERM);
                    free(c);
                }
            }
        }
        usleep(100000);
    }
    if (olk < 0) {
        fprintf(stderr, "9share: an earlier offer of '%s' does not end\n", name);
        return 1;
    }
    {
        char buf[32];
        snprintf(buf, sizeof buf, "%d\n", (int)getpid());
        p9_write_file(opid, buf, strlen(buf), 0600);
    }
    unlink(a.sun_path);
    umask(077);
    if ((lsock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 || bind(lsock, (struct sockaddr *)&a, sizeof a) < 0 || listen(lsock, 1) < 0) {
        fprintf(stderr, "9share: cannot listen on %s: %s\n", a.sun_path, strerror(errno));
        return 1;
    }
    self = p9_self();
    if (!self || strchr(self, '\'')) {
        fprintf(stderr, "9share: cannot tell where the 9share program is\n");
        unlink(a.sun_path);
        return 1;
    }
    signal(SIGTERM, SIG_DFL);
    t0 = time(NULL);
    while (fd < 0 && time(NULL) - t0 < 60) {
        struct pollfd p[2];
        ensure_daud(&q, self);
        p[0].fd = lsock; p[0].events = POLLIN; p[0].revents = 0;
        p[1].fd = 0; p[1].events = 0; p[1].revents = 0;          /* ssh went away before daud came */
        if (poll(p, 2, 1000) < 0 && errno != EINTR)
            break;
        if (p[1].revents & (POLLHUP | POLLERR | POLLNVAL))
            break;
        if (p[0].revents & POLLIN) {
            int c = accept4(lsock, NULL, NULL, SOCK_CLOEXEC);
            if (c >= 0) {
                struct ucred cr;
                socklen_t l = sizeof cr;
                if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0 || cr.uid != getuid())
                    close(c);
                else
                    fd = c;
            }
        }
    }
    close(lsock);
    unlink(a.sun_path);
    free(self);
    if (fd < 0) {
        fprintf(stderr, "9share: no carrier came from the daud of '%s'; see %s\n", name, q.log);
        unlink(opid);
        return 1;
    }
    i = pump(fd);
    unlink(opid);
    close(olk);
    return i;
}
