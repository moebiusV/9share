/* util.c -- logging, paths, files, running programs, and editing the text of a config file. */
#include "p9share.h"
#include <errno.h>
#include <fcntl.h>
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
    fprintf(stderr, "%s p9share: %s\n", ts, msg);
    if (geteuid() == 0)
        syslog(LOG_INFO, "%s", msg);
}

const char *p9_sysconf_dir(void)
{
    static char buf[P9_LINE];
    const char *e = getenv("P9SHARE_SYSCONFDIR");
    if (e && *e)
        return e;
    snprintf(buf, sizeof buf, "%s/p9share", P9SHARE_SYSCONFDIR);
    return buf;
}

const char *p9_daud(void)
{
    const char *e = getenv("P9SHARE_DAUD");
    return e && *e ? e : P9SHARE_DAUD;
}

void p9_user_conf_path(const char *home, char *buf, size_t n)
{
    const char *x = home ? NULL : getenv("XDG_CONFIG_HOME");
    if (home)
        snprintf(buf, n, "%s/.config/p9share/p9share.conf", home);
    else if (x && *x)
        snprintf(buf, n, "%s/p9share/p9share.conf", x);
    else
        snprintf(buf, n, "%s/.config/p9share/p9share.conf", getenv("HOME") ? getenv("HOME") : ".");
}

void p9_system_conf_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/p9share.conf", p9_sysconf_dir());
}

void p9_state_dir(char *buf, size_t n)
{
    const char *x = getenv("XDG_STATE_HOME");
    if (x && *x)
        snprintf(buf, n, "%s/p9share", x);
    else
        snprintf(buf, n, "%s/.local/state/p9share", getenv("HOME") ? getenv("HOME") : ".");
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
        return strdup("p9share");
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
    snprintf(buf, n, "%s/.config/p9share/mode.d/%s", home, name);
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
