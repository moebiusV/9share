/* With NINESHARE_TEST_IMPORT set the other direction is tested instead (an import): `9share serve` runs a stand-in for ssh that runs
 * `9share offer` on this machine (the two "machines" share a home directory; the files they keep there have different names), offer
 * starts daud (--once), and what is cut is the stand-in ssh: serve runs it again, a new offer is joined to the daud that is still there.
 *
 * test_resume.c -- the whole receiving side, short of the kernel: a daud that dials out, a relay that accepts, a socket pair standing
 * in for the kernel's end of the mount, and a client that reads and writes tens of megabytes while the carrier (the process that
 * ssh would be) is killed again and again.  Nothing may be lost, repeated or reordered; and when the relay itself is replaced,
 * daud's session ends and a new one carries on. */
#define _GNU_SOURCE
#include "test.h"
#include "p9client.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>

static char dir[96], home[128], etc[128], share[160], pidfile[160], dlog[160];
static const char *bin, *daud;
static char secretf[400];                          /* set when NINESHARE_TEST_SECRET asks for SCRAM on this run */

static int import_mode;                            /* NINESHARE_TEST_IMPORT: serve and offer, instead of daud directly */

static uint8_t pat(uint64_t i) { return (uint8_t)((i * 2654435761u) >> 11); }

static pid_t start_relay(int kr, int ctl_w)
{
    pid_t pid = fork();
    if (pid == 0) {
        char k[16], c[16];
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 0); dup2(dn, 1);
        fcntl(kr, F_SETFD, 0);                       /* the two this child needs stay open across exec */
        fcntl(ctl_w, F_SETFD, 0);
        snprintf(k, sizeof k, "%d", kr);
        snprintf(c, sizeof c, "%d", ctl_w);
        setenv("HOME", home, 1);
        setenv("NINESHARE_SYSCONFDIR", etc, 1);                 /* where the relay finds prefetch-kb */
        execl(bin, "9share", "relay", "t", "40", k, c, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t start_serve(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        char p[600], st[300];
        int dn = open("/dev/null", O_RDWR), lf = open(dlog, O_WRONLY | O_CREAT | O_APPEND, 0600);
        dup2(dn, 0); dup2(lf, 1); dup2(lf, 2);
        setenv("HOME", home, 1);
        unsetenv("XDG_CONFIG_HOME");
        snprintf(st, sizeof st, "%s/state", dir);
        setenv("XDG_STATE_HOME", st, 1);
        snprintf(p, sizeof p, "%s/bin:%s", dir, getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin");
        setenv("PATH", p, 1);
        setenv("NINESHARE_DAUD", daud, 1);
        setenv("CARRIER_LOG", pidfile, 1);
        setenv("NINESHARE_SERVE_DELAY_MAX", "1", 1);          /* the ssh that is cut is run again at once, not after 1, 2, 4 ... seconds */
        execl(bin, "9share", "serve", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static pid_t start_daud(void)
{
    pid_t pid;
    if (import_mode)
        return start_serve();
    pid = fork();
    if (pid == 0) {
        char cmd[400], uid[16];
        int dn = open("/dev/null", O_RDWR), lf = open(dlog, O_WRONLY | O_CREAT | O_APPEND, 0600);
        dup2(dn, 0); dup2(lf, 1); dup2(lf, 2);
        snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
        snprintf(cmd, sizeof cmd, "echo $$ >> %s; exec %s link t", pidfile, bin);
        setenv("HOME", home, 1);
        if (secretf[0])
            execl(daud, "daud", "-f", "-n", "-N", "-u", uid, "-e", share, "--dial", cmd, "--hold", "40",
                  "--link-secret-file", secretf, "-L", "stderr", (char *)NULL);
        execl(daud, "daud", "-f", "-n", "-N", "-u", uid, "-e", share, "--dial", cmd, "--hold", "40", "-L", "stderr", (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* kill the carrier that was started last; returns 1 if there was one */
static int cut(void)
{
    FILE *f = fopen(pidfile, "r");
    long pid = 0, p;
    if (!f)
        return 0;
    while (fscanf(f, "%ld", &p) == 1)
        pid = p;
    fclose(f);
    return pid > 0 && kill((pid_t)pid, SIGKILL) == 0;
}

static char seen[4096];                            /* what the control pipe had said when wait_line last found its line */

static int wait_line(int fd, const char *want, int secs)
{
    char buf[4096];
    size_t n = 0;
    time_t end = time(NULL) + secs;
    while (time(NULL) < end) {
        ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
        if (k > 0) {
            n += (size_t)k;
            buf[n] = '\0';
            if (strstr(buf, want)) {
                memcpy(seen, buf, n + 1);
                return 1;
            }
        } else
            usleep(50000);
    }
    return 0;
}

static int open_client(int fd)
{
    struct timeval tmo = { 30, 0 };
    struct msg q = { .n = 0 }, r;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    put32(&q, 65536); puts9(&q, "9P2000.L");
    return rpc(fd, 100, &q, &r) == 101 && attach(fd, NONUNAME, share) == 105;
}

static int pidcount(void)
{
    FILE *f = fopen(pidfile, "r");
    int n = 0;
    long p;
    if (!f)
        return 0;
    while (fscanf(f, "%ld", &p) == 1)
        n++;
    fclose(f);
    return n;
}

int main(void)
{
    const uint64_t size = 16u << 20, wsize = 4u << 20;
    int kc, kr, sv[2], ctl[2], k2c, k2r, ctl2[2], i, cuts = 0;
    pid_t relay, dd, relay2;
    struct msg q = { .n = 0 }, r;
    uint64_t off, bad = 0;
    FILE *f;
    char path[200];

    bin = getenv("NINESHARE_BIN");
    daud = getenv("NINESHARE_DAUD");
    if (!bin || !daud || access(bin, X_OK) || access(daud, X_OK)) {
        fprintf(stderr, "no 9share or daud to test (NINESHARE_BIN, NINESHARE_DAUD)\n");
        return 77;
    }
    snprintf(dir, sizeof dir, "/tmp/9share-test-res-%d", (int)getpid());
    snprintf(home, sizeof home, "%s/home", dir);
    snprintf(share, sizeof share, "%s/share", dir);
    snprintf(pidfile, sizeof pidfile, "%s/carriers", dir);
    import_mode = getenv("NINESHARE_TEST_IMPORT") != NULL;
    snprintf(dlog, sizeof dlog, import_mode ? "%s/home/.9share/t.daud.log" : "%s/daud.log", dir);
    snprintf(etc, sizeof etc, "%s/etc", dir);
    mkdir(dir, 0700); mkdir(home, 0700); mkdir(share, 0700); mkdir(etc, 0700);
    if (getenv("NINESHARE_TEST_SECRET")) {          /* both ends read the same secret: daud from this file, the relay from HOME */
        char d[300];
        FILE *sf;
        snprintf(d, sizeof d, "%s/.config", home); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/.config/9share", home); mkdir(d, 0700);
        snprintf(secretf, sizeof secretf, "%s/t.secret", d);
        sf = fopen(secretf, "w");
        CHECK(sf != NULL);
        if (sf) { fputs("a secret for the test\n", sf); fclose(sf); }
        CHECK(chmod(secretf, 0600) == 0);
    }
    if (import_mode) {
        char d[300];
        FILE *cf;
        snprintf(d, sizeof d, "%s/bin", dir); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/bin/ssh", dir);
        cf = fopen(d, "w");
        CHECK(cf != NULL);
        if (cf) {            /* ssh HOST COMMAND...: run the command here, as itself, so that cutting it is cutting ssh */
            fputs("#!/bin/sh\nwhile [ $# -gt 0 ]; do case $1 in -o) shift 2;; -T) shift;; *) break;; esac; done\nshift; shift\n"
                  "echo $$ >> \"$CARRIER_LOG\"\nexec \"$NINESHARE_BIN\" \"$@\"\n", cf);
            fclose(cf);
        }
        CHECK(chmod(d, 0700) == 0);
        snprintf(d, sizeof d, "%s/.config", home); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/.config/9share", home); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/.9share", home); mkdir(d, 0700);
        snprintf(d, sizeof d, "%s/.config/9share/9share.conf", home);
        cf = fopen(d, "w");
        CHECK(cf != NULL);
        if (cf) {
            fprintf(cf, "import \"t\" {\n\tdirectory \"%s\"\n\thost \"other.example.org\"\n\thold 40\n}\n", share);
            fclose(cf);
        }
        CHECK(chmod(d, 0600) == 0);
    }
    snprintf(path, sizeof path, "%s/big.bin", share);
    f = fopen(path, "w");
    for (off = 0; off < size; off++)
        fputc(pat(off), f);
    fclose(f);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && pipe2(ctl, O_NONBLOCK | O_CLOEXEC) == 0);
    kc = sv[0]; kr = sv[1];
    relay = start_relay(kr, ctl[1]);
    dd = start_daud();
    CHECK(open_client(kc));
    CHECK(wait_line(ctl[0], "NEW ", 20));                        /* the relay saw a new session */

    /* read a big file, cutting the carrier now and then */
    q.n = 0; put32(&q, 1); put32(&q, 2); put16(&q, 1); puts9(&q, "big.bin");
    CHECK(rpc(kc, 110, &q, &r) == 111);
    q.n = 0; put32(&q, 2); put32(&q, 0);
    CHECK(rpc(kc, 12, &q, &r) == 13);
    for (off = 0, i = 0; off < size; i++) {
        uint32_t n;
        uint64_t j;
        q.n = 0; put32(&q, 2); put64(&q, off); put32(&q, 8000);
        if (rpc(kc, 116, &q, &r) != 117)
            break;
        n = r.b[0] | (r.b[1] << 8) | (r.b[2] << 16) | ((uint32_t)r.b[3] << 24);
        if (n == 0)
            break;
        for (j = 0; j < n; j++)
            if (r.b[4 + j] != pat(off + j))
                bad++;
        off += n;
        if (i % 100 == 99)
            cuts += cut();
    }
    CHECK(off == size);
    CHECK(bad == 0);
    CHECK(cuts >= 10);
    CHECK(pidcount() > cuts);                                    /* daud started a carrier again after each cut */

    {   /* read-ahead is switched off and on while the mount is in use: the relay notices the file, with no restart and no remount */
        char p[300];
        FILE *pf;
        snprintf(p, sizeof p, "%s/prefetch-kb", etc);
        if ((pf = fopen(p, "w"))) { fputs("0\n", pf); fclose(pf); }
        CHECK(wait_line(ctl[0], "read-ahead now 0 KB", 6));
        if ((pf = fopen(p, "w"))) { fputs("2048\n", pf); fclose(pf); }
        CHECK(wait_line(ctl[0], "read-ahead now 2048 KB", 6));
        if ((pf = fopen(p, "w"))) { fputs("garbage\n", pf); fclose(pf); }
        CHECK(wait_line(ctl[0], "is not usable: read-ahead stays at 2048 KB", 6));      /* a broken file changes nothing */
        if ((pf = fopen(p, "w"))) { fputs("2048\n", pf); fclose(pf); }
    }

    /* write a big file, cutting as it goes */
    q.n = 0; put32(&q, 1); put32(&q, 3); put16(&q, 0);
    CHECK(rpc(kc, 110, &q, &r) == 111);
    q.n = 0; put32(&q, 3); puts9(&q, "up.bin"); put32(&q, O_WRONLY); put32(&q, 0644); put32(&q, (uint32_t)getgid());
    CHECK(rpc(kc, 14, &q, &r) == 15);
    for (off = 0, i = 0; off < wsize; i++) {
        uint32_t n = 8000;
        uint32_t j;
        q.n = 0; put32(&q, 3); put64(&q, off); put32(&q, n);
        for (j = 0; j < n; j++)
            q.b[q.n++] = pat(off + j);
        if (rpc(kc, 118, &q, &r) != 119)
            break;
        off += n;
        if (i % 100 == 99)
            cuts += cut();
    }
    CHECK(off >= wsize);
    q.n = 0; put32(&q, 3);
    CHECK(rpc(kc, 120, &q, &r) == 121);
    snprintf(path, sizeof path, "%s/up.bin", share);
    f = fopen(path, "r");
    CHECK(f != NULL);
    if (f) {
        uint64_t k, wrong = 0;
        int c;
        for (k = 0; (c = fgetc(f)) != EOF; k++)
            if ((uint8_t)c != pat(k))
                wrong++;
        CHECK(k == off && wrong == 0);
        fclose(f);
    }
    close(kc);                                                   /* the kernel's end goes away: the relay says so and ends */
    CHECK(wait_line(ctl[0], "KCLOSED", 10));
    {   /* the relay read ahead, and the kernel's reads were answered from it (prefetch-kb is at its default here) */
        const char *ra = strstr(seen, "read-ahead: used=");
        unsigned long used = ra ? strtoul(ra + strlen("read-ahead: used="), NULL, 10) : 0;
        CHECK(used > 100);
        if (used <= 100)
            fprintf(stderr, "   (the relay said: %s)\n", ra ? ra : "nothing about read-ahead");
    }
    kill(relay, SIGTERM);
    waitpid(relay, NULL, 0);

    /* a new relay (the machine restarted, say) does not know daud's session: daud's ends, and a new one begins */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0 && pipe2(ctl2, O_NONBLOCK | O_CLOEXEC) == 0);
    k2c = sv[0]; k2r = sv[1];
    relay2 = start_relay(k2r, ctl2[1]);
    CHECK(wait_line(ctl2[0], "NEW ", 60));
    CHECK(open_client(k2c));
    q.n = 0; put32(&q, 1); put32(&q, 5); put16(&q, 1); puts9(&q, "big.bin");
    CHECK(rpc(k2c, 110, &q, &r) == 111);
    q.n = 0; put32(&q, 5); put32(&q, 0);
    CHECK(rpc(k2c, 12, &q, &r) == 13);
    q.n = 0; put32(&q, 5); put64(&q, 1000); put32(&q, 100);
    CHECK(rpc(k2c, 116, &q, &r) == 117 && r.n >= 4 + 100 && r.b[4] == pat(1000));

    kill(dd, SIGTERM); waitpid(dd, NULL, 0);
    kill(relay2, SIGTERM); waitpid(relay2, NULL, 0);
    if (import_mode) {                              /* the daud that offer started is not serve's child: end it by its record */
        char pf[300];
        FILE *pidf;
        long dp = 0;
        snprintf(pf, sizeof pf, "%s/.9share/t.daud.pid", home);
        if ((pidf = fopen(pf, "r"))) {
            if (fscanf(pidf, "%ld", &dp) != 1)
                dp = 0;
            fclose(pidf);
        }
        CHECK(dp > 1);                              /* offer started it, and wrote who it is */
        if (dp > 1)
            kill((pid_t)dp, SIGTERM);
    }
    if (t_failed) {
        char cmd[300];
        fprintf(stderr, "--- the log (serve's, or daud's) ---\n");
        snprintf(cmd, sizeof cmd, "tail -30 %s >&2", dlog);
        if (system(cmd) != 0) {}
    }
    {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", dir);
    }
    printf("(%d carriers cut)\n", cuts);
    DONE();
}
