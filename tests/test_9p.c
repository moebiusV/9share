/* test_9p.c -- a real daud, unprivileged, reached by a small 9P2000.L client: the attach that names no numeric uid (which an
 * unprivileged diod refused and daud accepts), reading, creating and writing, and what it refuses. */
#define _GNU_SOURCE
#include "test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
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

int main(void)
{
    char dir[64], share[128], sock[100], data[160], other[128];
    const char *daud = getenv("P9SHARE_DAUD");
    struct sockaddr_un a;
    struct msg q, r;
    pid_t pid;
    int fd, i, tries, st;
    FILE *f;
    char buf[256], rb[64];

    if (!daud || access(daud, X_OK) != 0) {
        fprintf(stderr, "no daud to test (P9SHARE_DAUD)\n");
        return 77;
    }
    snprintf(dir, sizeof dir, "/tmp/p9share-test-9p-%d", (int)getpid());
    snprintf(share, sizeof share, "%s/share", dir);
    snprintf(other, sizeof other, "%s/other", dir);
    snprintf(sock, sizeof sock, "%s/d.sock", dir);
    snprintf(data, sizeof data, "%s/hello.txt", share);
    mkdir(dir, 0700); mkdir(share, 0700); mkdir(other, 0700);
    f = fopen(data, "w"); fputs("hello 9p\n", f); fclose(f);

    pid = fork();
    if (pid == 0) {
        char uid[32];
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1); dup2(dn, 2);
        snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
        execl(daud, "daud", "-f", "-n", "-N", "-u", uid, "-e", share, "-l", sock, "-L", "stderr", (char *)NULL);
        _exit(127);
    }
    for (tries = 0; tries < 50 && access(sock, F_OK) != 0; tries++)
        usleep(100000);
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);

    #define CONNECT() do { struct timeval tmo = { 10, 0 }; fd = socket(AF_UNIX, SOCK_STREAM, 0); \
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo); CHECK(connect(fd, (struct sockaddr *)&a, sizeof a) == 0); \
        q.n = 0; put32(&q, 65536); puts9(&q, "9P2000.L"); CHECK(rpc(fd, 100, &q, &r) == 101); } while (0)

    /* the kernel's attach: no numeric uid.  An unprivileged diod answers EPERM; daud takes it as the user it runs as. */
    CONNECT();
    CHECK(attach(fd, NONUNAME, share) == 105);
    CHECK(walk(fd, 1, 2, "hello.txt") == 111);
    q.n = 0; put32(&q, 2); put32(&q, 0);
    CHECK(rpc(fd, 12, &q, &r) == 13);                                    /* lopen read-only */
    q.n = 0; put32(&q, 2); put64(&q, 0); put32(&q, 100);
    CHECK(rpc(fd, 116, &q, &r) == 117);
    CHECK(r.n >= 4 && r.n - 4 == 9 && memcmp(r.b + 4, "hello 9p\n", 9) == 0);

    /* create and write a file */
    CHECK(walk(fd, 1, 3, NULL) == 111);                                  /* clone the root */
    q.n = 0; put32(&q, 3); puts9(&q, "new.txt"); put32(&q, O_WRONLY); put32(&q, 0644); put32(&q, (uint32_t)getgid());
    CHECK(rpc(fd, 14, &q, &r) == 15);
    q.n = 0; put32(&q, 3); put64(&q, 0); put32(&q, 7); memcpy(q.b + q.n, "written", 7); q.n += 7;
    CHECK(rpc(fd, 118, &q, &r) == 119);
    q.n = 0; put32(&q, 3);
    CHECK(rpc(fd, 120, &q, &r) == 121);
    snprintf(buf, sizeof buf, "%s/new.txt", share);
    f = fopen(buf, "r");
    CHECK(f != NULL);
    if (f) { memset(rb, 0, sizeof rb); CHECK(fgets(rb, sizeof rb, f) && !strcmp(rb, "written")); fclose(f); }
    close(fd);

    /* what it refuses: another user's uid, and a directory that is not exported */
    CONNECT();
    CHECK(attach(fd, (uint32_t)getuid() + 1, share) == 7);
    close(fd);
    CONNECT();
    CHECK(attach(fd, NONUNAME, other) == 7);
    close(fd);
    CONNECT();
    CHECK(attach(fd, (uint32_t)getuid(), share) == 105);                 /* a numeric uid that is the user's still works */
    close(fd);

    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    for (i = 0; i < 1; i++) {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", dir);
    }
    DONE();
}
