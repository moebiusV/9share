/* util.c -- logging, paths, files, running programs, and editing the text of a config file. */
#include "9share.h"
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

void p9_log(const char *fmt, ...)
{
    char msg[512], ts[32];
    time_t t = time(NULL);
    struct tm tm;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "%s 9share: %s\n", ts, msg);
    if (geteuid() == 0)
        syslog(LOG_INFO, "%s", msg);
}

const char *p9_sysconf_dir(void)
{
    static char buf[P9_LINE];
    const char *e = getenv("NINESHARE_SYSCONFDIR");
    if (e && *e)
        return e;
    snprintf(buf, sizeof buf, "%s/9share", NINESHARE_SYSCONFDIR);
    return buf;
}

const char *p9_daud(void)
{
    const char *e = getenv("NINESHARE_DAUD");
    return e && *e ? e : NINESHARE_DAUD;
}

void p9_user_conf_path(const char *home, char *buf, size_t n)
{
    const char *x = home ? NULL : getenv("XDG_CONFIG_HOME");
    if (home)
        snprintf(buf, n, "%s/.config/9share/9share.conf", home);
    else if (x && *x)
        snprintf(buf, n, "%s/9share/9share.conf", x);
    else
        snprintf(buf, n, "%s/.config/9share/9share.conf", getenv("HOME") ? getenv("HOME") : ".");
}

/* Where the link secret of share NAME is: HOME's .config/9share/NAME.secret (HOME NULL: beside the caller's 9share.conf). */
int p9_secret_path(const char *home, const char *name, char *buf, size_t n)
{
    char conf[P9_LINE], *slash;
    if (!nineshare_valid_name(name))
        return -1;
    p9_user_conf_path(home, conf, sizeof conf);
    if (!(slash = strrchr(conf, '/')))
        return -1;
    *slash = '\0';
    return snprintf(buf, n, "%s/%s.secret", conf, name) >= (int)n ? -1 : 0;
}

/* Put SECRET (one line) in PATH for its owner alone, whole or not at all.  0; 1 if the file is there and FORCE is not set; -1 on error. */
int p9_secret_store(const char *path, const char *secret, int force)
{
    char tmp[P9_LINE + 32], dir[P9_LINE], *slash;
    struct stat st;
    size_t n = strlen(secret);
    int fd, rc;

    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/'))) {
        *slash = '\0';
        if (p9_mkdir_p(dir, 0700) < 0)
            return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.new.%d", path, (int)getpid());
    if ((fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600)) < 0)
        return -1;
    if (write(fd, secret, n) != (ssize_t)n || write(fd, "\n", 1) != 1 || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);
    if (force)
        rc = rename(tmp, path) == 0 ? 0 : -1;
    else if (link(tmp, path) == 0)                      /* fails if PATH is there: nothing is replaced, and no one sees half a file */
        rc = 0;
    else if (errno == EEXIST)
        rc = 1;
    else                                                /* a file system without hard links: look, then rename */
        rc = lstat(path, &st) == 0 ? 1 : (rename(tmp, path) == 0 ? 0 : -1);
    unlink(tmp);
    return rc;
}

/* The link secret of share NAME.  Returns 0 and the secret (its first line) when the file is there and usable; 1 when there is no
 * such file (the link then trusts its carrier); -1 when it is there but must not be used, with the reason in WHY: the link must
 * not fall back to trusting the carrier. */
int p9_secret_load(const char *home, const char *name, char *secret, size_t n, char *path, size_t pn, char *why, size_t wn)
{
    char line[512], *nl;
    struct stat st;
    FILE *f;
    int fd;

    if (p9_secret_path(home, name, path, pn) < 0) {
        snprintf(why, wn, "%s is not a usable name, or its secret's path is too long", name);
        return -1;
    }
    if ((fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
        if (errno == ENOENT)
            return 1;
        snprintf(why, wn, "cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        snprintf(why, wn, "%s must be a regular file of this user", path);
        return -1;
    }
    if (st.st_mode & 077) {
        close(fd);
        snprintf(why, wn, "%s must not be readable by anyone but its owner (chmod 600)", path);
        return -1;
    }
    if (!(f = fdopen(fd, "r"))) {
        close(fd);
        snprintf(why, wn, "cannot read %s", path);
        return -1;
    }
    if (!fgets(line, sizeof line, f) || !(nl = strpbrk(line, "\r\n")) || nl == line || (size_t)(nl - line) >= n) {
        fclose(f);
        snprintf(why, wn, "%s must hold the secret, up to %d characters, on its first line", path, (int)n - 1);
        return -1;
    }
    *nl = '\0';
    fclose(f);
    snprintf(secret, n, "%s", line);
    memset(line, 0, sizeof line);
    return 0;
}

void p9_system_conf_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/9share.conf", p9_sysconf_dir());
}

void p9_state_dir(char *buf, size_t n)
{
    const char *x = getenv("XDG_STATE_HOME");
    if (x && *x)
        snprintf(buf, n, "%s/9share", x);
    else
        snprintf(buf, n, "%s/.local/state/9share", getenv("HOME") ? getenv("HOME") : ".");
}

int p9_mkdir_p(const char *path, mode_t mode)
{
    char tmp[P9_LINE], *p;
    snprintf(tmp, sizeof tmp, "%s", path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

char *p9_read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "r");
    char *buf = NULL;
    size_t n = 0, cap = 0, r;
    if (!f)
        return NULL;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            char *b = realloc(buf, cap = cap ? cap * 2 : 8192);
            if (!b) { free(buf); fclose(f); return NULL; }
            buf = b;
        }
        r = fread(buf + n, 1, 4096, f);
        n += r;
        if (r < 4096)
            break;
    }
    fclose(f);
    buf[n] = '\0';
    if (len)
        *len = n;
    return buf;
}

int p9_write_file(const char *path, const char *data, size_t len, mode_t mode)
{
    char tmp[P9_LINE + 8], dir[P9_LINE], *slash;
    FILE *f;
    int ok;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/')) && slash != dir) {
        *slash = '\0';
        if (p9_mkdir_p(dir, 0755) < 0)
            return -1;
    }
    snprintf(tmp, sizeof tmp, "%s.new", path);
    if (!(f = fopen(tmp, "w")))
        return -1;
    ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    if (!ok || chmod(tmp, mode) < 0 || rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int p9_file_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

int p9_lock(const char *path)
{
    char dir[P9_LINE], *slash;
    int fd;
    snprintf(dir, sizeof dir, "%s", path);
    if ((slash = strrchr(dir, '/')) && slash != dir) {
        *slash = '\0';
        p9_mkdir_p(dir, 0700);
    }
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int p9_lock_held(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC), held;
    if (fd < 0)
        return 0;
    held = flock(fd, LOCK_EX | LOCK_NB) < 0;
    close(fd);
    return held;
}

const char *p9_run_dir(void)
{
    const char *e = getenv("NINESHARE_RUNDIR");
    return e && *e ? e : "/run";
}

int p9_proc_alive(int pid)
{
    char path[64], buf[512], *p;
    FILE *f;
    if (pid <= 0)
        return 0;
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (!(f = fopen(path, "r")))
        return 0;
    p = fgets(buf, sizeof buf, f);
    fclose(f);
    if (!p || !(p = strrchr(buf, ')')))
        return 0;
    return p[2] != 'Z' && p[2] != 'X';             /* "pid (comm) S ...": a zombie has ended, only nobody has waited for it */
}

/* Is PID a `9share boot`?  Its command line has an argument whose last part is 9share, followed by "boot" (a script run through an
 * interpreter has the interpreter's name first, so the first argument is not the one to look at).  A pid from a file is never trusted
 * without this: after a reboot the number belongs to something else, and that must not be sent a signal. */
static int is_9share_cmd(int pid, const char *word)
{
    char path[64], buf[4096], *p, *end;
    ssize_t n;
    int fd;
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
        return 0;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    end = buf + n;
    for (p = buf; p < end; p += strlen(p) + 1) {
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        if (!strcmp(base, "9share")) {
            char *next = p + strlen(p) + 1;
            return next < end && !strcmp(next, word);
        }
    }
    return 0;
}

static int is_boot_process(int pid) { return is_9share_cmd(pid, "boot"); }

/* this user's `9share serve`: its pid is in the state directory, and is not trusted unless that process is a 9share serve */
int p9_serve_pid(void)
{
    char state[P9_LINE], path[P9_LINE + 16], *txt;
    long v = 0;
    p9_state_dir(state, sizeof state);
    snprintf(path, sizeof path, "%s/serve.pid", state);
    if ((txt = p9_read_file(path, NULL))) {
        v = strtol(txt, NULL, 10);
        free(txt);
    }
    return v > 1 && p9_proc_alive((int)v) && is_9share_cmd((int)v, "serve") ? (int)v : 0;
}

/* ask them to read their configuration again (SIGHUP); 1 if there was one to ask */
int p9_reload_serve(void)
{
    int pid = p9_serve_pid();
    return pid > 0 && kill(pid, SIGHUP) == 0;
}

int p9_reload_daemon(void)
{
    int pid = p9_daemon_pid();
    return pid > 0 && kill(pid, SIGHUP) == 0;
}

int p9_daemon_pid(void)
{
    char path[P9_LINE], *txt;
    DIR *d;
    struct dirent *e;
    int found = 0;
    snprintf(path, sizeof path, "%s/9share.pid", p9_run_dir());
    if ((txt = p9_read_file(path, NULL))) {
        long v = strtol(txt, NULL, 10);
        free(txt);
        if (v > 1 && p9_proc_alive((int)v) && is_boot_process((int)v))
            return (int)v;
    }
    if (getenv("NINESHARE_RUNDIR"))                  /* tests: only the file counts, whatever runs on this machine */
        return 0;
    /* no usable pid file (a daemon started before there was one): look through /proc */
    if (!(d = opendir("/proc")))
        return 0;
    while (!found && (e = readdir(d))) {
        int pid = atoi(e->d_name);
        if (pid > 1 && isdigit((unsigned char)e->d_name[0]) && pid != (int)getpid() && p9_proc_alive(pid) && is_boot_process(pid))
            found = pid;
    }
    closedir(d);
    return found;
}

/* the first line of the file at PATH, cut at its ends; NULL if there is no such file */
static char *first_line(const char *path)
{
    char *txt = p9_read_file(path, NULL), *nl, *b, *e;
    if (!txt)
        return NULL;
    if ((nl = strpbrk(txt, "\r\n")))
        *nl = '\0';
    for (b = txt; *b == ' ' || *b == '\t'; b++)
        ;
    for (e = b + strlen(b); e > b && (e[-1] == ' ' || e[-1] == '\t'); e--)
        ;
    *e = '\0';
    memmove(txt, b, (size_t)(e - b) + 1);
    return txt;
}

int p9_mount_options(char *opts, size_t n)
{
    static const char *own[] = { "trans", "rfdno", "wfdno", "uname", "aname", "access", "version", "msize", NULL };   /* 9share's own */
    char path[P9_LINE + 32], *l, *p, *tok, *sv = NULL;      /* the system directory (P9_LINE at most) and a file name */
    int rc = 0, i;
    opts[0] = '\0';
    snprintf(path, sizeof path, "%s/mount-options", p9_sysconf_dir());
    if ((l = first_line(path)) && *l) {
        int ok = strlen(l) < n && strlen(l) < 200 && l[0] != ',' && l[strlen(l) - 1] != ',' && !strstr(l, ",,");
        for (p = l; ok && *p; p++)
            if (!isalnum((unsigned char)*p) && !strchr("_=,.-", *p))
                ok = 0;
        if (ok) {
            char *copy = strdup(l);
            for (tok = copy ? strtok_r(copy, ",", &sv) : NULL; tok && ok; tok = strtok_r(NULL, ",", &sv)) {
                size_t klen = strcspn(tok, "=");
                for (i = 0; own[i]; i++)
                    if (strlen(own[i]) == klen && !strncmp(own[i], tok, klen))
                        ok = 0;
            }
            free(copy);
        }
        if (ok)
            snprintf(opts, n, "%s", l);
        else
            rc = -1;
    }
    free(l);
    return rc;
}

static int dir_trust(const char *file, unsigned owner, char *why, size_t wn)
{
    char dir[P9_LINE + 32], *slash;
    struct stat st;
    snprintf(dir, sizeof dir, "%s", file);
    if (!(slash = strrchr(dir, '/')))
        snprintf(dir, sizeof dir, ".");
    else if (slash == dir)
        slash[1] = '\0';
    else
        *slash = '\0';
    if (stat(dir, &st) < 0) {
        snprintf(why, wn, "cannot look at the directory %s: %s", dir, strerror(errno));
        return -1;
    }
    if (st.st_uid != owner && st.st_uid != 0) {
        snprintf(why, wn, "the directory %s is owned by user %u: it must be owned by %s", dir, (unsigned)st.st_uid, owner ? "its user or root" : "root");
        return -1;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        snprintf(why, wn, "the directory %s is writable by group or others (chmod go-w %s)", dir, dir);
        return -1;
    }
    return 0;
}

int p9_conf_trust(const char *path, unsigned owner, char *why, size_t wn)
{
    char real[PATH_MAX];
    struct stat st;
    why[0] = '\0';
    if (stat(path, &st) < 0) {
        if (errno == ENOENT)
            return 1;
        snprintf(why, wn, "cannot look at %s: %s", path, strerror(errno));
        return -1;
    }
    if (!realpath(path, real))
        snprintf(real, sizeof real, "%s", path);
    if (!S_ISREG(st.st_mode)) {
        snprintf(why, wn, "%s is not a regular file", real);
        return -1;
    }
    if (st.st_uid != owner && st.st_uid != 0) {
        snprintf(why, wn, "%s is owned by user %u: it must be owned by %s", real, (unsigned)st.st_uid, owner ? "its user or root" : "root");
        return -1;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        snprintf(why, wn, "%s is writable by group or others (chmod go-w %s)", real, real);
        return -1;
    }
    if (dir_trust(path, owner, why, wn) < 0)
        return -1;
    if (strcmp(real, path) != 0 && dir_trust(real, owner, why, wn) < 0)     /* through a link: the directory of the link is as good as the file's */
        return -1;
    return 0;
}

void p9_kernel_mount_opts(char *buf, size_t n, int kfd, const char *uname, const char *aname, unsigned msize, unsigned access_uid, int any_user)
{
    char acc[16];
    if (any_user)
        snprintf(acc, sizeof acc, "any");
    else
        snprintf(acc, sizeof acc, "%u", access_uid);
    snprintf(buf, n, "trans=fd,rfdno=%d,wfdno=%d,version=9p2000.L,uname=%s,access=%s,aname=%s,msize=%u", kfd, kfd, uname, acc, aname, msize);
}

int p9_prefetch_kb(int *kb)
{
    char path[P9_LINE + 32], *l;
    int rc = 0;
    *kb = 2048;                                      /* what scp keeps in flight: 64 requests of 32 KB */
    snprintf(path, sizeof path, "%s/prefetch-kb", p9_sysconf_dir());
    if ((l = first_line(path)) && *l) {
        char *end;
        long v = strtol(l, &end, 10);
        if (*end == '\0' && v >= 0 && v <= 65536)
            *kb = (int)v;
        else
            rc = -1;
    }
    free(l);
    return rc;
}

int p9_run(char *const argv[], int timeout, char *out, size_t outsz)
{
    int pfd[2] = { -1, -1 }, status = 0;
    pid_t pid;
    time_t end = time(NULL) + timeout;
    size_t n = 0;
    if (out && outsz) {
        out[0] = '\0';
        if (pipe(pfd) < 0)
            return -1;
    }
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0);
        dup2(pfd[1] >= 0 ? pfd[1] : dn, 1);
        dup2(dn, 2);
        if (pfd[0] >= 0) { close(pfd[0]); close(pfd[1]); }
        execvp(argv[0], argv);
        _exit(127);
    }
    if (pfd[1] >= 0)
        close(pfd[1]);
    for (;;) {
        struct pollfd p = { pfd[0], POLLIN, 0 };
        pid_t r;
        if (pfd[0] >= 0 && poll(&p, 1, 200) > 0) {
            char buf[512];
            ssize_t k = read(pfd[0], buf, sizeof buf);
            if (k > 0 && n + 1 < outsz) {
                size_t c = (size_t)k < outsz - 1 - n ? (size_t)k : outsz - 1 - n;
                memcpy(out + n, buf, c);
                n += c;
                out[n] = '\0';
            } else if (k <= 0) {
                close(pfd[0]);
                pfd[0] = -1;
            }
        } else if (pfd[0] < 0) {
            usleep(100000);
        }
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (time(NULL) >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            if (pfd[0] >= 0)
                close(pfd[0]);
            return 124;
        }
    }
    if (pfd[0] >= 0) {
        char buf[512];
        ssize_t k;
        while ((k = read(pfd[0], buf, sizeof buf)) > 0 && n + 1 < outsz) {
            size_t c = (size_t)k < outsz - 1 - n ? (size_t)k : outsz - 1 - n;
            memcpy(out + n, buf, c);
            n += c;
            out[n] = '\0';
        }
        close(pfd[0]);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

char *p9_self(void)
{
    char buf[P9_LINE];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return strdup("9share");
    buf[n] = '\0';
    return strdup(buf);
}

int p9_block_append(const char *path, const char *text)
{
    size_t len = 0, tl = strlen(text);
    char *old = p9_read_file(path, &len), *all;
    int rc;
    if (!old)
        old = strdup("");
    all = malloc(len + tl + 2);
    if (!all) { free(old); return -1; }
    memcpy(all, old, len);
    if (len && old[len - 1] != '\n')
        all[len++] = '\n';
    if (len)
        all[len++] = '\n';
    memcpy(all + len, text, tl);
    rc = p9_write_file(path, all, len + tl, 0644);
    free(old);
    free(all);
    return rc;
}

/* the first two words of a line, with quotes dropped; 1 if there are two */
static int two_words(const char *s, const char *end, char *w1, char *w2, size_t n)
{
    char *w[2] = { w1, w2 };
    int i;
    for (i = 0; i < 2; i++) {
        size_t k = 0;
        while (s < end && (*s == ' ' || *s == '\t'))
            s++;
        if (s >= end || *s == '#')
            return 0;
        if (*s == '"') {
            s++;
            while (s < end && *s != '"' && k + 1 < n)
                w[i][k++] = *s++;
            if (s < end && *s == '"')
                s++;
        } else {
            while (s < end && *s != ' ' && *s != '\t' && *s != '{' && k + 1 < n)
                w[i][k++] = *s++;
        }
        w[i][k] = '\0';
    }
    return 1;
}

int p9_block_remove(const char *path, const char *keyword, const char *name, size_t line)
{
    size_t len = 0, ln = 1;
    char *text = p9_read_file(path, &len), *p, *start = NULL, *q;
    int depth = 0, seen = 0, rc = -1;
    if (!text)
        return -1;
    /* the line the block starts on: the first at or after LINE that says KEYWORD NAME */
    for (p = text; *p; ) {
        char *eol = strchr(p, '\n'), w1[128], w2[128];
        if (!eol)
            eol = p + strlen(p);
        if (ln >= line && two_words(p, eol, w1, w2, sizeof w1) && !strcmp(w1, keyword) && !strcmp(w2, name)) {
            start = p;
            break;
        }
        p = *eol ? eol + 1 : eol;
        ln++;
    }
    if (!start)
        goto out;
    /* its end: the line of the brace that closes it, taking strings and comments as they are */
    for (q = start; *q; q++) {
        if (*q == '#') {
            while (*q && *q != '\n')
                q++;
            if (!*q)
                break;
        } else if (*q == '"') {
            for (q++; *q && *q != '"'; q++)
                if (*q == '\\' && q[1])
                    q++;
            if (!*q)
                break;
        } else if (*q == '{') {
            depth++;
            seen = 1;
        } else if (*q == '}' && --depth == 0 && seen) {
            break;
        }
    }
    if (!*q || !seen)
        goto out;
    while (*q && *q != '\n')
        q++;
    if (*q == '\n')
        q++;
    if (*q == '\n')                         /* one blank line after the block goes with it */
        q++;
    memmove(start, q, strlen(q) + 1);
    rc = p9_write_file(path, text, strlen(text), 0644);
out:
    free(text);
    return rc;
}

void p9_mode_path(const char *home, const char *name, char *buf, size_t n)
{
    snprintf(buf, n, "%s/.config/9share/mode.d/%s", home, name);
}

const char *p9_mode_of(const char *home, const char *name)
{
    char path[2 * P9_LINE + 64], buf[16] = "";
    int fd;
    ssize_t k;
    p9_mode_path(home, name, path, sizeof path);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        k = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (k > 0)
            buf[k] = '\0';
    }
    if (!strncmp(buf, "ro", 2))
        return "ro";
    if (!strncmp(buf, "off", 3))
        return "off";
    return "rw";
}
