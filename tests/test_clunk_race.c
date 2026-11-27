/* test_clunk_race.c -- a Tclunk that arrives while Treads on the same fid are still being served must not crash daud.
 *
 * daud serves requests in a pool of threads.  Tread on an open fid and Tclunk of that fid can therefore run at the same moment; the read looked
 * at f->ioctx, and the clunk closed the file and set f->ioctx to NULL before the read used it: a SIGSEGV at address 0x2c (the offset of the file
 * descriptor in the io context) inside pread.  A client that behaves (waits for the answers to its reads before it clunks) never meets this, but
 * the read-ahead of the relay has reads in flight when the kernel clunks, and a server must not die of what a client sends.
 *
 * So: one connection; each round opens a file, then sends 8 reads and the clunk in ONE write, and collects the 9 answers.  Any order of answers
 * is fine, and a read answered after the clunk may be an error (the fid is gone); what is not fine is daud dying, which shows as a broken
 * connection or a process that is no longer there. */
#define _GNU_SOURCE
#include "test.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include "p9client.h"

#define ROUNDS 4000
#define READS  8
#define CHUNK  32768

static int connect_to(const char *sock)
{
    struct sockaddr_un a;
    struct timeval tmo = { 5, 0 };
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof tmo);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* append one request (size, type, tag, body) to m */
static void add(struct msg *m, uint8_t type, uint16_t tag, const struct msg *body)
{
    size_t at = m->n, len = 7 + (body ? body->n : 0);
    put32(m, (uint32_t)len); put8(m, type); put16(m, tag);
    if (body) { memcpy(m->b + m->n, body->b, body->n); m->n += body->n; }
    (void)at;
}

/* read one reply; its type, or -1 */
static int reply(int fd, uint16_t *tag)
{
    uint8_t h[4], rest[8192 + 64];
    uint32_t size;
    if (readn(fd, h, 4) < 0)
        return -1;
    size = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
    if (size < 7 || size > CHUNK + 64)
        return -1;
    {
        uint8_t *buf = malloc(size);
        int t;
        if (!buf || readn(fd, buf, size - 4) < 0) { free(buf); return -1; }
        t = buf[0];
        *tag = (uint16_t)(buf[1] | (buf[2] << 8));
        free(buf);
        (void)rest;
        return t;
    }
}

int main(void)
{
    const char *daud = getenv("NINESHARE_DAUD");
    char dir[64], share[128], sock[100], path[160], cmd[200], uid[32];
    int fd, round, bad = 0, alive_at_end, setup_ok = 0, errors = 0;
    pid_t pid;
    struct msg q, r;
    static char blk[CHUNK];

    if (!daud || access(daud, X_OK) != 0) {
        fprintf(stderr, "no daud to test (NINESHARE_DAUD)\n");
        return 77;
    }
    snprintf(dir, sizeof dir, "/tmp/9share-test-clr-%d", (int)getpid());
    snprintf(share, sizeof share, "%s/share", dir);
    snprintf(sock, sizeof sock, "%s/d.sock", dir);
    snprintf(path, sizeof path, "%s/f", share);
    mkdir(dir, 0700);
    mkdir(share, 0700);
    memset(blk, 'x', sizeof blk);
    {
        int f = open(path, O_WRONLY | O_CREAT, 0600), i;
        for (i = 0; f >= 0 && i < 32; i++)
            if (write(f, blk, sizeof blk) != (ssize_t)sizeof blk)
                break;
        if (f >= 0)
            close(f);
    }
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    pid = fork();
    if (pid == 0) {
        int dn = open("/dev/null", O_RDWR);
        dup2(dn, 1); dup2(dn, 2);
        execl(daud, "daud", "-f", "-n", "-N", "-u", uid, "-e", share, "-l", sock, "-L", "stderr", (char *)NULL);
        _exit(127);
    }
    {
        int tries;
        for (tries = 0; tries < 100 && access(sock, F_OK) != 0; tries++)
            usleep(50000);
    }
    fd = connect_to(sock);
    if (fd >= 0) {
        q.n = 0; put32(&q, 131072); puts9(&q, "9P2000.L");
        if (rpc(fd, 100, &q, &r) == 101 && attach(fd, (uint32_t)getuid(), share) == 105)
            setup_ok = 1;
    }
    CHECK(setup_ok);
    for (round = 0; setup_ok && round < ROUNDS; round++) {
        struct msg out = { .n = 0 }, b;
        int i, got = 0, clunked = 0;
        uint16_t tag;

        if (walk(fd, 1, 2, "f") != 111) { bad++; break; }
        b.n = 0; put32(&b, 2); put32(&b, 0);                       /* Tlopen fid 2, O_RDONLY */
        if (rpc(fd, 12, &b, &r) != 13) { bad++; break; }
        for (i = 0; i < READS; i++) {
            b.n = 0; put32(&b, 2); put64(&b, (uint64_t)i * CHUNK); put32(&b, CHUNK);
            add(&out, 116, (uint16_t)(100 + i), &b);
        }
        b.n = 0; put32(&b, 2);
        add(&out, 120, 200, &b);                                    /* the clunk, in the same write as the reads */
        if (write(fd, out.b, out.n) != (ssize_t)out.n) { bad++; break; }
        for (i = 0; i < READS + 1; i++) {
            int t = reply(fd, &tag);
            if (t == 121 && tag == 200)
                clunked = 1;
            else if (t == 117 || t == 7)
                errors += (t == 7);
            else { bad++; break; }
            got++;
        }
        if (got != READS + 1 || !clunked) { bad++; break; }
    }
    if (fd >= 0)
        close(fd);
    alive_at_end = (kill(pid, 0) == 0 && waitpid(pid, NULL, WNOHANG) == 0);
    if (!alive_at_end)
        fprintf(stderr, "daud is gone after %d rounds\n", round);
    CHECK(bad == 0);
    CHECK(alive_at_end);
    fprintf(stderr, "%d rounds, %d reads answered with an error after the clunk\n", round, errors);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", dir);
    DONE();
}
