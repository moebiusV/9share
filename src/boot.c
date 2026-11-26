/* boot.c -- the root daemon: keeps `p9share serve` running for every user who has exports, and makes the mounts of
 * /etc/p9share/p9share.conf agree with two facts every two seconds: is the exporter's socket there and answering, and what
 * does the user want (rw, ro or off: ~USER/.config/p9share/mode.d/NAME, which the user writes with no sudo). */
#include "p9share.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define PB (2 * P9_LINE + 64)

static volatile sig_atomic_t stopping;

static void on_term(int sig) { (void)sig; stopping = 1; }

const char *p9_mount_state_name(int s)
{
    switch (s) {
    case MS_RW: return "rw";
    case MS_RO: return "ro";
    case MS_DEAD: return "not answering";
    case MS_OTHER: return "something else is mounted there";
    default: return "not mounted";
    }
}

/* what is mounted at AT, from /proc/self/mountinfo; MS_DEAD if it is 9p but a stat of it hangs or fails */
int p9_mount_state(const char *at)
{
    char real[P9_LINE], line[2048];
    FILE *f;
    int state = MS_NONE;
    pid_t pid;
    if (!realpath(at, real))
        snprintf(real, sizeof real, "%s", at);
    if (!(f = fopen("/proc/self/mountinfo", "r")))
        return MS_NONE;
    while (fgets(line, sizeof line, f)) {
        char *mp, *opts, *sep, *fstype, *sp;
        int i;
        /* id parent major:minor root mountpoint options [optional fields] - fstype source superoptions */
        for (mp = line, i = 0; i < 4 && mp; i++)
            mp = (sp = strchr(mp, ' ')) ? sp + 1 : NULL;
        if (!mp)
            continue;
        opts = strchr(mp, ' ');
        if (!opts)
            continue;
        *opts++ = '\0';
        if (strcmp(mp, real) != 0)
            continue;
        if (!(sep = strstr(opts, " - ")))
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
    if (state == MS_RW || state == MS_RO) {         /* a dead tunnel leaves a mount whose stat hangs: ask a child, with a timeout */
        pid = fork();
        if (pid == 0) {
            struct stat st;
            _exit(stat(real, &st) == 0 ? 0 : 1);
        }
        if (pid > 0) {
            int st = 0, i;
            pid_t r = 0;
            for (i = 0; i < 50 && (r = waitpid(pid, &st, WNOHANG)) == 0; i++)
                usleep(100000);
            if (r == 0) {
                kill(pid, SIGKILL);                 /* it may stay in the kernel; it is no longer ours to wait for */
                state = MS_DEAD;
            } else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
                state = MS_DEAD;
        }
    }
    return state;
}

void p9_unmount(const char *at)
{
    umount2(at, MNT_DETACH);
}

static int socket_answers(const char *path)
{
    struct sockaddr_un a;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0), ok;
    if (fd < 0)
        return 0;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { close(fd); return 0; }
    strcpy(a.sun_path, path);
    ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

/* the user's name and the directory the exporter says it serves, from NAME.info next to the socket (written by the exporter, so
 * both are checked before they go into a mount option).  0 on success. */
static int read_info(const char *path, uid_t uid, char *uname, size_t un, char *aname, size_t an)
{
    struct stat st;
    char *text, *line, *save = NULL;
    int ok = 0;
    if (lstat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != uid)
        return -1;
    if (!(text = p9_read_file(path, NULL)))
        return -1;
    uname[0] = aname[0] = '\0';
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, "uname=", 6))
            snprintf(uname, un, "%s", line + 6);
        else if (!strncmp(line, "aname=", 6))
            snprintf(aname, an, "%s", line + 6);
    }
    free(text);
    ok = p9share_valid_host(uname) && strchr(uname, '@') == NULL && strchr(uname, ':') == NULL && p9share_valid_path(aname);
    return ok ? 0 : -1;
}

struct said { char name[80]; char msg[200]; };

static void say_once(struct said *tab, const char *name, const char *msg)
{
    int i, free_slot = -1;
    for (i = 0; i < 32; i++) {
        if (!strcmp(tab[i].name, name)) {
            if (!strcmp(tab[i].msg, msg))
                return;
            snprintf(tab[i].msg, sizeof tab[i].msg, "%s", msg);
            p9_log("%s: %s", name, msg);
            return;
        }
        if (free_slot < 0 && !tab[i].name[0])
            free_slot = i;
    }
    if (free_slot >= 0) {
        snprintf(tab[free_slot].name, sizeof tab[free_slot].name, "%s", name);
        snprintf(tab[free_slot].msg, sizeof tab[free_slot].msg, "%s", msg);
    }
    p9_log("%s: %s", name, msg);
}

static void reconcile(const struct p9share_mount *m, struct said *said)
{
    struct passwd *pw = getpwnam(m->user);
    char sock[PB], info[PB], uname[64], aname[P9_LINE], opts[3 * P9_LINE], mode_ro[8];
    const char *want;
    struct stat st;
    int have, up;
    if (!pw)
        return;
    snprintf(sock, sizeof sock, "%s/.p9share/%s.sock", pw->pw_dir, m->name);
    snprintf(info, sizeof info, "%s/.p9share/%s.info", pw->pw_dir, m->name);
    want = p9_mode_of(pw->pw_dir, m->name);
    have = p9_mount_state(m->at);
    if (have == MS_OTHER) {
        say_once(said, m->name, "something that is not 9p is mounted at the mount point; leaving it");
        return;
    }
    if (have == MS_DEAD) {
        p9_log("%s: the mount does not answer, unmounting", m->at);
        p9_unmount(m->at);
        have = MS_NONE;
    }
    up = lstat(sock, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_uid == pw->pw_uid && socket_answers(sock);
    if (!strcmp(want, "off") || !up) {
        if (have != MS_NONE && umount2(m->at, MNT_DETACH) == 0)
            p9_log("unmounted %s (%s)", m->at, !strcmp(want, "off") ? "off" : "the tunnel is not there");
        return;
    }
    snprintf(mode_ro, sizeof mode_ro, "%s", want);
    if (have == MS_NONE) {
        if (read_info(info, pw->pw_uid, uname, sizeof uname, aname, sizeof aname) < 0) {
            say_once(said, m->name, "the exporter's info file is missing or has values that cannot be used");
            return;
        }
        p9_mkdir_p(m->at, 0755);
        snprintf(opts, sizeof opts, "trans=unix,version=9p2000.L,uname=%s,access=any,aname=%s,msize=1048576", uname, aname);
        if (mount(sock, m->at, "9p", MS_NOSUID | MS_NODEV | (!strcmp(want, "ro") ? MS_RDONLY : 0), opts) == 0)
            p9_log("mounted %s on %s (%s)", aname, m->at, want);
        else
            say_once(said, m->name, strerror(errno));
    } else if ((have == MS_RO) != !strcmp(want, "ro")) {
        if (mount(NULL, m->at, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | (!strcmp(want, "ro") ? MS_RDONLY : 0), NULL) == 0)
            p9_log("%s is now %s", m->at, want);
    }
}

static pid_t start_user_serve(const struct passwd *pw, const char *self)
{
    pid_t pid = fork();
    if (pid == 0) {
        char home[PB], user[96], logn[96];
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        dup2(dn, 1);
        dup2(dn, 2);
        if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0)
            _exit(126);
        if (chdir(pw->pw_dir) < 0)
            _exit(126);
        clearenv();
        snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
        snprintf(user, sizeof user, "USER=%s", pw->pw_name);
        snprintf(logn, sizeof logn, "LOGNAME=%s", pw->pw_name);
        putenv(home);
        putenv(user);
        putenv(logn);
        putenv("PATH=/usr/local/bin:/usr/bin:/bin");
        execl(self, "p9share", "serve", (char *)NULL);
        _exit(127);
    }
    return pid;
}

int p9_boot(int detach)
{
    char confp[P9_LINE], *self;
    struct sigaction sa;
    struct said said[32];
    pid_t kids[64];
    uid_t kid_uid[64];
    int nkids = 0, lk, i;
    char **managed = NULL;
    size_t nmanaged = 0, k;

    if (geteuid() != 0) {
        fprintf(stderr, "p9share: boot is for root (the p9share service runs it)\n");
        return 1;
    }
    if (detach) {
        pid_t pid = fork();
        if (pid < 0)
            return 1;
        if (pid > 0)
            return 0;
        setsid();
        if (freopen("/var/log/p9share.log", "a", stderr) == NULL)
            return 1;
        close(0);
        close(1);
    }
    if ((lk = p9_lock("/run/p9share.lock")) < 0) {
        fprintf(stderr, "p9share: boot is already running\n");
        return 1;
    }
    openlog("p9share", LOG_PID, LOG_DAEMON);
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
    memset(said, 0, sizeof said);
    p9_system_conf_path(confp, sizeof confp);

    while (!stopping) {
        struct passwd *pw;
        struct p9share_conf c;
        size_t j;

        /* each user who has exports gets a `p9share serve`, unless one runs (maybe started by hand) */
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
            snprintf(lockp, sizeof lockp, "%s/.local/state/p9share/serve.lock", pw->pw_dir);
            if (have || p9_lock_held(lockp) || nkids >= 64)
                continue;
            if ((kids[nkids] = start_user_serve(pw, self)) > 0) {
                kid_uid[nkids++] = pw->pw_uid;
                p9_log("started the exports of %s", pw->pw_name);
            }
        }
        endpwent();

        /* the mounts */
        p9share_conf_init(&c);
        if (p9_file_exists(confp))
            p9share_conf_parse(confp, &c);
        for (j = 0; j < c.nmounts; j++)
            reconcile(&c.mounts[j], said);
        for (k = 0; k < nmanaged; k++) {            /* a mount that left the file is unmounted */
            for (j = 0; j < c.nmounts && strcmp(c.mounts[j].at, managed[k]); j++)
                ;
            if (j == c.nmounts)
                p9_unmount(managed[k]);
            free(managed[k]);
        }
        free(managed);
        managed = c.nmounts ? calloc(c.nmounts, sizeof *managed) : NULL;
        nmanaged = managed ? c.nmounts : 0;
        for (k = 0; k < nmanaged; k++)
            managed[k] = strdup(c.mounts[k].at);
        p9share_conf_free(&c);
        for (i = 0; i < 20 && !stopping; i++)
            usleep(100000);
    }
    for (k = 0; k < nmanaged; k++) {
        p9_unmount(managed[k]);
        free(managed[k]);
    }
    free(managed);
    for (i = 0; i < nkids; i++)
        kill(kids[i], SIGTERM);
    free(self);
    close(lk);
    return 0;
}
