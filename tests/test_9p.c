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
#include "p9client.h"

int main(void)
{
    char dir[64], share[128], sock[100], data[160], other[128];
    const char *daud = getenv("NINESHARE_DAUD");
    struct sockaddr_un a;
    struct msg q, r;
    pid_t pid;
    int fd, i, tries, st;
    FILE *f;
    char buf[256], rb[64];

    if (!daud || access(daud, X_OK) != 0) {
        fprintf(stderr, "no daud to test (NINESHARE_DAUD)\n");
        return 77;
    }
    snprintf(dir, sizeof dir, "/tmp/9share-test-9p-%d", (int)getpid());
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
    for (i = 0; i < 50 && waitpid(pid, &st, WNOHANG) != pid; i++)
        usleep(100000);
    CHECK(i < 50);                                                  /* daud leaves on SIGTERM (it once did not, now and then: test_daud_shutdown.c) */
    if (i == 50) {
        kill(pid, SIGKILL);
        waitpid(pid, &st, 0);
    }
    for (i = 0; i < 1; i++) {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", dir);
    }
    DONE();
}
