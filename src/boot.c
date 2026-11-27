/* boot.c -- the root daemon: keeps `9share serve` running for every user who has exports, and keeps each mount of
 * /etc/9share/9share.conf right.  A mount has a relay (`9share relay`, a process of the mount's user: relay.c) that holds the
 * link's session and one end of a socket pair; the daemon holds the other end until the relay reports that the exporter's
 * session began ("NEW"), and then mounts with the kernel's 9p client over that end (trans=fd).  When the exporter's ssh breaks
 * the mount stays: the relay waits for the next carrier.  When the relay ends (its hold time passed, the exporter began
 * another session, the kernel let go) the mount is unmounted and a new relay waits.  The user's mode (rw, ro, off:
 * ~USER/.config/9share/mode.d/NAME, written by `9share rw|ro|off`) is followed every tick. */
#include "9share.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PB (2 * P9_LINE + 64)
#define MAXMOUNTS 64
#define DEFAULT_HOLD 600

static volatile sig_atomic_t stopping;

static void on_term(int sig) { (void)sig; stopping = 1; }

const char *p9_mount_state_name(int s)
{
    switch (s) {
    case MS_RW: return "rw";
    case MS_RO: return "ro";
    case MS_OTHER: return "something else is mounted there";
    default: return "not mounted";
    }
}

/* what is mounted at AT, from /proc/self/mountinfo (nothing is asked of the mount itself: it may be waiting for a carrier) */
int p9_mount_state(const char *at)
{
    char real[P9_LINE], rp[PATH_MAX], line[2048];
    FILE *f;
    int state = MS_NONE;
    if (realpath(at, rp) && strlen(rp) < sizeof real)      /* realpath may write PATH_MAX bytes */
        snprintf(real, sizeof real, "%s", rp);
    else
        snprintf(real, sizeof real, "%s", at);
    if (!(f = fopen("/proc/self/mountinfo", "r")))
        return MS_NONE;
    while (fgets(line, sizeof line, f)) {
        char *mp, *opts, *sep, *fstype, *sp;
        int i;
        /* id parent major:minor root mountpoint options [optional fields] - fstype source superoptions */
        for (mp = line, i = 0; i < 4 && mp; i++)
            mp = (sp = strchr(mp, ' ')) ? sp + 1 : NULL;
        if (!mp || !(opts = strchr(mp, ' ')))
            continue;
        *opts++ = '\0';
        if (strcmp(mp, real) != 0 || !(sep = strstr(opts, " - ")))
            continue;
        fstype = sep + 3;
        if ((sp = strchr(fstype, ' ')))
            *sp = '\0';
        *sep = '\0';
        if ((sp = strchr(opts, ' ')))
            *sp = '\0';
        if (strcmp(fstype, "9p") != 0) {
            state = MS_OTHER;
        } else {
            char *o = opts, *e;
            state = MS_RW;
            for (; o; o = e ? e + 1 : NULL) {
                e = strchr(o, ',');
                if (!strncmp(o, "ro", 2) && (o[2] == ',' || o[2] == '\0'))
                    state = MS_RO;
            }
        }
    }
    fclose(f);
    return state;
}

void p9_unmount(const char *at)
{
    umount2(at, MNT_DETACH);
}

/* ---- one mount's process state ---- */

struct mstate {
    char    name[80];
    int     used;
    pid_t   relay, mounter;
    int     ctl, kfd;                  /* the relay's messages; the kernel's end of the socket pair until the mounter has it */
    char    buf[1024];
    size_t  buflen;
    time_t  retry_at, mount_t0;
    int     mounted;                   /* this daemon mounted it and the relay lives */
};

static struct mstate *state_of(struct mstate *tab, const char *name)
{
    int i;
    struct mstate *free_slot = NULL;
    for (i = 0; i < MAXMOUNTS; i++) {
        if (tab[i].used && !strcmp(tab[i].name, name))
            return &tab[i];
        if (!tab[i].used && !free_slot)
            free_slot = &tab[i];
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->used = 1;
        snprintf(free_slot->name, sizeof free_slot->name, "%s", name);
        free_slot->ctl = free_slot->kfd = -1;
    }
    return free_slot;
}

static void drop_child_privileges(const struct passwd *pw)
{
    char home[PB], user[96], logn[96];
    int dn = open("/dev/null", O_RDWR);
    dup2(dn, 0);
    dup2(dn, 1);
    dup2(dn, 2);
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0 || chdir(pw->pw_dir) < 0)
        _exit(126);
    clearenv();
    snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
    snprintf(user, sizeof user, "USER=%s", pw->pw_name);
    snprintf(logn, sizeof logn, "LOGNAME=%s", pw->pw_name);
    putenv(home);
    putenv(user);
    putenv(logn);
    putenv("PATH=/usr/local/bin:/usr/bin:/bin");
}

static void relay_start(struct mstate *ms, const struct nineshare_mount *m, const struct passwd *pw, const char *self)
{
    int sv[2], cp[2];
    char hold[16], r[16], c[16];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0 || pipe2(cp, O_CLOEXEC | O_NONBLOCK) < 0) {
        p9_log("%s: cannot make the sockets for a relay: %s", m->name, strerror(errno));
        ms->retry_at = time(NULL) + 5;
        return;
    }
    ms->relay = fork();
    if (ms->relay == 0) {
        drop_child_privileges(pw);
        fcntl(sv[1], F_SETFD, 0);                   /* the two this child needs stay open across exec */
        fcntl(cp[1], F_SETFD, 0);
        snprintf(hold, sizeof hold, "%d", m->hold ? m->hold : DEFAULT_HOLD);
        snprintf(r, sizeof r, "%d", sv[1]);
        snprintf(c, sizeof c, "%d", cp[1]);
        execl(self, "9share", "relay", m->name, hold, r, c, (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    close(cp[1]);
    if (ms->relay < 0) {
        ms->relay = 0;
        close(sv[0]);
        close(cp[0]);
        ms->retry_at = time(NULL) + 5;
        return;
    }
    ms->kfd = sv[0];
    ms->ctl = cp[0];
    ms->buflen = 0;
    ms->mounted = 0;
}

static void relay_end(struct mstate *ms, const struct nineshare_mount *m)
{
    if (ms->ctl >= 0) close(ms->ctl);
    if (ms->kfd >= 0) close(ms->kfd);
    ms->ctl = ms->kfd = -1;
    if (ms->mounter > 0) {
        kill(ms->mounter, SIGKILL);
        waitpid(ms->mounter, NULL, 0);
        ms->mounter = 0;
    }
    if (p9_mount_state(m->at) != MS_NONE && p9_mount_state(m->at) != MS_OTHER)
        p9_unmount(m->at);                          /* the 9p client of a mount whose relay is gone is dead */
    ms->mounted = 0;
    ms->relay = 0;
    ms->retry_at = time(NULL) + 1;
}

/* the exporter's session began: mount over the kernel's end of the socket pair.  mount(2) waits for the server's answer, which
 * needs the relay and the link to run, so a child makes the call and this loop goes on. */
/* The size of one 9P message: the most one request or one answer carries (a read of a big file is a series of requests of at most this size less a
 * 24 byte header).  It also sets the size of the buffers the kernel's 9p client asks for, one or two for every request, each a single contiguous
 * block of memory: 1 MB (order 8) was asked for on a machine whose free memory was 400 MB of 63 GB and had no free block that big, and a whole-file
 * read failed with ENOMEM ("Cannot allocate memory"); 64 KB had failed once as well, on a machine under pressure.  Since the relay reads ahead (see
 * proxy.c), the speed no longer depends on the size of a request, so it is small: 128 KB (order 5) is a block a busy machine still has.  The
 * author's choice, 2026-11-27 (it was 1 MB for a day: the prototype's). */
#define NINESHARE_MSIZE 131072

static void mounter_start(struct mstate *ms, const struct nineshare_mount *m, const char *uname, const char *aname, int ro)
{
    char opts[3 * P9_LINE], extra[256];
    if (ms->mounter > 0 || ms->kfd < 0)
        return;
    p9_mkdir_p(m->at, 0755);
    snprintf(opts, sizeof opts, "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,uname=%s,access=any,aname=%s,msize=%d",
             ms->kfd, ms->kfd, uname, aname, NINESHARE_MSIZE);
    if (p9_mount_options(extra, sizeof extra) < 0)
        syslog(LOG_WARNING, "%s/mount-options is not usable (see 9share(8)): ignored", p9_sysconf_dir());
    if (extra[0])
        snprintf(opts + strlen(opts), sizeof opts - strlen(opts), ",%s", extra);
    ms->mount_t0 = time(NULL);
    ms->mounter = fork();
    if (ms->mounter == 0) {
        int rc = mount("9share", m->at, "9p", MS_NOSUID | MS_NODEV | (ro ? MS_RDONLY : 0), opts);
        if (rc < 0)
            fprintf(stderr, "%s\n", strerror(errno));
        _exit(rc < 0 ? 1 : 0);
    }
    close(ms->kfd);                                 /* the child has its own copy; the kernel takes a reference of its own */
    ms->kfd = -1;
    if (ms->mounter < 0)
        ms->mounter = 0;
}

static int valid_uname(const char *s)
{
    return nineshare_valid_host(s) && !strchr(s, '@') && !strchr(s, ':');
}

static void ctl_read(struct mstate *ms, const struct nineshare_mount *m, int ro)
{
    char *nl;
    ssize_t k;
    if (ms->ctl < 0)
        return;
    k = read(ms->ctl, ms->buf + ms->buflen, sizeof ms->buf - 1 - ms->buflen);
    if (k > 0)
        ms->buflen += (size_t)k;
    ms->buf[ms->buflen] = '\0';
    while ((nl = strchr(ms->buf, '\n'))) {
        *nl = '\0';
        if (!strncmp(ms->buf, "NEW ", 4)) {
            char uname[64], aname[P9_LINE];
            if (sscanf(ms->buf + 4, "%63s %1023s", uname, aname) == 2 && valid_uname(uname) && nineshare_valid_path(aname))
                mounter_start(ms, m, uname, aname, ro);
            else
                p9_log("%s: the exporter's session asks for values that cannot be used", m->name);
        } else if (!strncmp(ms->buf, "LOG ", 4)) {
            p9_log("%s: %s", m->name, ms->buf + 4);
        } else {
            p9_log("%s: the link session ended (%s)", m->name, ms->buf);
        }
        ms->buflen -= (size_t)(nl + 1 - ms->buf);
        memmove(ms->buf, nl + 1, ms->buflen + 1);
    }
    if (ms->buflen >= sizeof ms->buf - 1)
        ms->buflen = 0;
}

static void mount_tick(struct mstate *ms, const struct nineshare_mount *m, const char *self)
{
    struct passwd *pw = getpwnam(m->user);
    const char *want;
    time_t now = time(NULL);
    int st, ro, have;
    if (!pw)
        return;
    want = p9_mode_of(pw->pw_dir, m->name);
    ro = !strcmp(want, "ro");
    if (ms->relay > 0 && waitpid(ms->relay, &st, WNOHANG) == ms->relay) {
        ctl_read(ms, m, ro);
        relay_end(ms, m);
    }
    if (ms->mounter > 0 && waitpid(ms->mounter, &st, WNOHANG) == ms->mounter) {
        ms->mounter = 0;
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
            ms->mounted = 1;
            p9_log("%s: mounted on %s (%s)", m->name, m->at, want);
        } else {
            p9_log("%s: the mount failed", m->name);
            if (ms->relay > 0)
                kill(ms->relay, SIGTERM);
        }
    } else if (ms->mounter > 0 && now - ms->mount_t0 > 60) {
        p9_log("%s: the mount did not complete in a minute", m->name);
        kill(ms->mounter, SIGKILL);
        if (ms->relay > 0)
            kill(ms->relay, SIGTERM);
    }
    ctl_read(ms, m, ro);
    if (!strcmp(want, "off")) {
        if (ms->relay > 0)
            kill(ms->relay, SIGTERM);
        else if (p9_mount_state(m->at) == MS_RW || p9_mount_state(m->at) == MS_RO)
            p9_unmount(m->at);
        return;
    }
    if (ms->relay <= 0 && now >= ms->retry_at) {
        have = p9_mount_state(m->at);
        if (have == MS_OTHER) {
            if (now >= ms->retry_at) {
                p9_log("%s: something that is not 9p is mounted at %s; leaving it", m->name, m->at);
                ms->retry_at = now + 300;
            }
            return;
        }
        if (have != MS_NONE)
            p9_unmount(m->at);                      /* a leftover of an earlier run */
        relay_start(ms, m, pw, self);
    }
    if (ms->mounted && ms->mounter <= 0) {
        have = p9_mount_state(m->at);
        if ((have == MS_RO) != ro && (have == MS_RO || have == MS_RW)) {
            if (mount(NULL, m->at, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | (ro ? MS_RDONLY : 0), NULL) == 0)
                p9_log("%s: %s is now %s", m->name, m->at, want);
        }
    }
}

static pid_t start_user_serve(const struct passwd *pw, const char *self)
{
    pid_t pid = fork();
    if (pid == 0) {
        drop_child_privileges(pw);
        execl(self, "9share", "serve", (char *)NULL);
        _exit(127);
    }
    return pid;
}

int p9_boot(int detach)
{
    char confp[P9_LINE], *self;
    struct sigaction sa;
    struct mstate mst[MAXMOUNTS];
    pid_t kids[64];
    uid_t kid_uid[64];
    int nkids = 0, lk, i;
    char pidp[P9_LINE];
    char **managed = NULL;
    size_t nmanaged = 0, k;

    if (geteuid() != 0) {
        fprintf(stderr, "9share: boot is for root (the 9share service runs it)\n");
        return 1;
    }
    if (detach) {
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid > 0)
            return 0;
        setsid();
        if (freopen("/var/log/9share.log", "a", stderr) == NULL)
            return 1;
        close(0);
        close(1);
    }
    if ((lk = p9_lock("/run/9share.lock")) < 0) {
        fprintf(stderr, "9share: boot is already running\n");
        return 1;
    }
    openlog("9share", LOG_PID, LOG_DAEMON);
    {   /* so that `9share status` (any user) and `9share shutdown` find this process; a stale file is not trusted: see p9_daemon_pid */
        char num[32];
        snprintf(pidp, sizeof pidp, "%s/9share.pid", p9_run_dir());
        snprintf(num, sizeof num, "%d\n", (int)getpid());
        if (p9_write_file(pidp, num, strlen(num), 0644) < 0)
            fprintf(stderr, "9share: cannot write %s: %s\n", pidp, strerror(errno));
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    {
        char *modprobe_9pnet[] = { "modprobe", "9pnet_fd", NULL }, *modprobe_9p[] = { "modprobe", "9p", NULL };
        p9_run(modprobe_9pnet, 20, NULL, 0);
        p9_run(modprobe_9p, 20, NULL, 0);
    }
    self = p9_self();
    memset(mst, 0, sizeof mst);
    p9_system_conf_path(confp, sizeof confp);

    while (!stopping) {
        struct passwd *pw;
        struct nineshare_conf c;
        size_t j;

        /* each user who has exports gets a `9share serve`, unless one runs (maybe started by hand) */
        for (i = 0; i < nkids; ) {
            int st;
            if (waitpid(kids[i], &st, WNOHANG) == kids[i]) {
                kids[i] = kids[--nkids];
                kid_uid[i] = kid_uid[nkids];
            } else
                i++;
        }
        setpwent();
        while ((pw = getpwent()) != NULL) {
            char upath[PB], lockp[PB];
            int have = 0;
            if (pw->pw_uid < 1000 || pw->pw_uid >= 65534 || !pw->pw_dir || pw->pw_dir[0] != '/')
                continue;
            p9_user_conf_path(pw->pw_dir, upath, sizeof upath);
            if (!p9_file_exists(upath))
                continue;
            for (i = 0; i < nkids; i++)
                if (kid_uid[i] == pw->pw_uid)
                    have = 1;
            snprintf(lockp, sizeof lockp, "%s/.local/state/9share/serve.lock", pw->pw_dir);
            if (have || p9_lock_held(lockp) || nkids >= 64)
                continue;
            if ((kids[nkids] = start_user_serve(pw, self)) > 0) {
                kid_uid[nkids++] = pw->pw_uid;
                p9_log("started the exports of %s", pw->pw_name);
            }
        }
        endpwent();

        /* the mounts */
        nineshare_conf_init(&c);
        if (p9_file_exists(confp))
            nineshare_conf_parse(confp, &c);
        for (j = 0; j < c.nmounts; j++) {
            struct mstate *ms = state_of(mst, c.mounts[j].name);
            if (ms)
                mount_tick(ms, &c.mounts[j], self);
        }
        for (k = 0; k < nmanaged; k++) {            /* a mount that left the file: its relay ends, it is unmounted */
            for (j = 0; j < c.nmounts && strcmp(c.mounts[j].name, managed[k]); j++)
                ;
            if (j == c.nmounts) {
                struct mstate *ms = state_of(mst, managed[k]);
                if (ms) {
                    if (ms->relay > 0) { kill(ms->relay, SIGTERM); waitpid(ms->relay, NULL, 0); }
                    if (ms->ctl >= 0) close(ms->ctl);
                    if (ms->kfd >= 0) close(ms->kfd);
                    ms->used = 0;
                }
            }
            free(managed[k]);
        }
        free(managed);
        managed = c.nmounts ? calloc(c.nmounts, sizeof *managed) : NULL;
        nmanaged = managed ? c.nmounts : 0;
        for (k = 0; k < nmanaged; k++)
            managed[k] = strdup(c.mounts[k].name);
        nineshare_conf_free(&c);
        for (i = 0; i < 5 && !stopping; i++)
            usleep(100000);
    }
    /* shutting down: the relays end, the mounts go */
    for (i = 0; i < MAXMOUNTS; i++)
        if (mst[i].used && mst[i].relay > 0) {
            kill(mst[i].relay, SIGTERM);
            waitpid(mst[i].relay, NULL, 0);
        }
    {
        struct nineshare_conf c2;
        nineshare_conf_init(&c2);
        if (p9_file_exists(confp))
            nineshare_conf_parse(confp, &c2);
        for (k = 0; k < c2.nmounts; k++)
            if (p9_mount_state(c2.mounts[k].at) == MS_RW || p9_mount_state(c2.mounts[k].at) == MS_RO)
                p9_unmount(c2.mounts[k].at);
        nineshare_conf_free(&c2);
    }
    for (k = 0; k < nmanaged; k++)
        free(managed[k]);
    free(managed);
    for (i = 0; i < nkids; i++)
        kill(kids[i], SIGTERM);
    free(self);
    unlink(pidp);
    close(lk);
    return 0;
}
