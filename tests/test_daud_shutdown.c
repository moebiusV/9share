/* test_daud_shutdown.c -- daud must leave when it is told to, even when its connections end at the same moment.
 *
 * Once, about one run in twelve of test_9p left daud alive after SIGTERM, every thread asleep.  The cause: a connection that has just ended
 * runs its cleanup (np_conn_cleanup in libnpfs/conn.c), and the shutdown (np_srv_shutdown) cancels the connection's thread; a cancellation
 * that landed at a cancellation point inside the cleanup (a log write, the wait for references, close) ended the thread before the connection was
 * counted out, and the main thread then waited for the count for ever.
 *
 * So: many connections, each with an attached fid (the cleanup logs "closed with N unclunked fids"), all dropped together and SIGTERM in the same
 * breath, again and again.  The unfixed daud failed about 3 rounds in 100 this way; the test does 120 rounds, so a daud with the bug fails it
 * in all but about one run in forty.  Each daud has 3 seconds to leave. */
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

#define ROUNDS 120
#define CONNS  50

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

int main(void)
{
    const char *daud = getenv("NINESHARE_DAUD");
    int round, hung = 0, setup_failed = 0;

    if (!daud || access(daud, X_OK) != 0) {
        fprintf(stderr, "no daud to test (NINESHARE_DAUD)\n");
        return 77;
    }
    for (round = 0; round < ROUNDS; round++) {
        char dir[64], share[128], sock[100], cmd[200], uid[32];
        int fds[CONNS], n = 0, i, tries, exited = 0;
        pid_t pid;
        struct msg q, r;

        snprintf(dir, sizeof dir, "/tmp/9share-test-dsd-%d-%d", (int)getpid(), round);
        snprintf(share, sizeof share, "%s/share", dir);
        snprintf(sock, sizeof sock, "%s/d.sock", dir);
        mkdir(dir, 0700);
        mkdir(share, 0700);
        snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
        pid = fork();
        if (pid == 0) {
            int dn = open("/dev/null", O_RDWR);
            dup2(dn, 1); dup2(dn, 2);
            execl(daud, "daud", "-f", "-n", "-N", "-u", uid, "-e", share, "-l", sock, "-L", "stderr", (char *)NULL);
            _exit(127);
        }
        for (tries = 0; tries < 100 && access(sock, F_OK) != 0; tries++)
            usleep(50000);
        for (i = 0; i < CONNS; i++) {
            int fd = connect_to(sock);
            if (fd < 0)
                break;
            q.n = 0; put32(&q, 65536); puts9(&q, "9P2000.L");
            if (rpc(fd, 100, &q, &r) != 101 || attach(fd, (uint32_t)getuid(), share) != 105) {
                close(fd);
                break;
            }
            fds[n++] = fd;
        }
        if (n != CONNS)
            setup_failed++;
        for (i = 0; i < n; i++)                                    /* all dropped ... */
            close(fds[i]);
        kill(pid, SIGTERM);                                        /* ... and the shutdown begins at once */
        for (tries = 0; tries < 300; tries++) {                    /* 3 seconds to leave */
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) {
                exited = 1;
                break;
            }
            usleep(10000);
        }
        if (!exited) {
            hung++;
            fprintf(stderr, "round %d: daud (pid %d) did not leave 3 s after SIGTERM\n", round, (int)pid);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
        }
        CHECK(exited);
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", dir);
    }
    CHECK(setup_failed == 0);                                      /* the test did what it says: every round had its 50 connections */
    CHECK(hung == 0);
    DONE();
}
