/* serve.c -- the exporter: for each export of the user's p9share.conf, daud (the 9P server, unprivileged, on a private
 * socket) and ssh -R, which hands that socket to the other machine.  Each is restarted when it dies, waiting 1, 2, 4 ...
 * 30 seconds after quick deaths.  The file is re-read as it changes; only the exports whose block changed are restarted. */
#include "p9share.h"
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PB (2 * P9_LINE + 64)       /* room for a state directory, a name and a file name in it */

struct child {
    pid_t  pid;
    time_t t0, wait_until;
    int    delay;
};

struct runner {
    char         *name, *directory, *host;
    char          dir[PB], sock[PB + 16];   /* its state directory and daud's socket */
    char          remote_sock[PB];            /* where ssh -R puts it on the host: known after prepare */
    struct child  daud, ssh;
};

static volatile sig_atomic_t stopping;

static void on_term(int sig) { (void)sig; stopping = 1; }

static void child_died(struct child *c, time_t now)
{
    if (now - c->t0 > 60)
        c->delay = 1;
    c->wait_until = now + c->delay;
    c->delay = c->delay * 2 > 30 ? 30 : c->delay * 2;
    c->pid = 0;
}

static int child_alive(struct child *c, time_t now)
{
    int st;
    if (c->pid > 0) {
        if (waitpid(c->pid, &st, WNOHANG) == 0)
            return 1;
        child_died(c, now);
    }
    return 0;
}

static void child_stop(struct child *c)
{
    int i, st;
    if (c->pid <= 0)
        return;
    kill(c->pid, SIGTERM);
    for (i = 0; i < 50; i++) {
        if (waitpid(c->pid, &st, WNOHANG) == c->pid) { c->pid = 0; return; }
        usleep(100000);
    }
    kill(c->pid, SIGKILL);
    waitpid(c->pid, &st, 0);
    c->pid = 0;
}

static pid_t spawn(char *const argv[], const char *logpath)
{
    pid_t pid = fork();
    if (pid == 0) {
        int lf = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0600), dn = open("/dev/null", O_RDONLY);
        dup2(dn, 0);
        dup2(lf, 1);
        dup2(lf, 2);
        umask(077);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* On the other machine: make ~/.p9share, clear the stale socket a dead tunnel left, say what the mount needs (the user's name
 * and the directory), and print the home directory so that ssh -R gets an absolute path.  0 on success. */
static int prepare_host(struct runner *r)
{
    struct passwd *pw = getpwuid(getuid());
    char cmd[4 * P9_LINE], out[P9_LINE], *nl, *home;
    char *argv[] = { "ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", r->host, cmd, NULL };
    int rc;
    if (!pw || !p9share_valid_host(pw->pw_name) || !p9share_valid_path(r->directory))
        return -1;
    snprintf(cmd, sizeof cmd,
             "umask 077; mkdir -p .p9share && rm -f .p9share/%s.sock && "
             "printf 'uname=%%s\\naname=%%s\\n' '%s' '%s' > .p9share/%s.info.new && mv .p9share/%s.info.new .p9share/%s.info && "
             "printf '%%s\\n' \"$HOME\"",
             r->name, pw->pw_name, r->directory, r->name, r->name, r->name);
    rc = p9_run(argv, 60, out, sizeof out);
    if (rc != 0)
        return -1;
    if ((nl = strrchr(out, '\n')) && nl[1] == '\0') {
        *nl = '\0';
        nl = strrchr(out, '\n');
    }
    home = nl ? nl + 1 : out;
    if (!p9share_valid_path(home))
        return -1;
    snprintf(r->remote_sock, sizeof r->remote_sock, "%s/.p9share/%s.sock", home, r->name);
    return 0;
}

static void start_daud(struct runner *r, time_t now)
{
    char uid[32], log[PB + 16];
    char *argv[] = { (char *)p9_daud(), "-f", "-n", "-N", "-u", uid, "-e", r->directory, "-l", r->sock, "-L", "stderr", NULL };
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    snprintf(log, sizeof log, "%s/daud.log", r->dir);
    unlink(r->sock);
    r->daud.pid = spawn(argv, log);
    r->daud.t0 = now;
    if (r->daud.pid < 0) { r->daud.pid = 0; child_died(&r->daud, now); }
}

static void start_ssh(struct runner *r, time_t now)
{
    char fwd[2 * PB + 32], log[PB + 16];
    char *argv[] = { "ssh", "-T", "-N", "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "-o", "StreamLocalBindUnlink=yes",
                     "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=3", "-o", "ConnectTimeout=10",
                     "-R", fwd, r->host, NULL };
    if (prepare_host(r) < 0) {
        p9_log("%s: cannot prepare %s (is ssh to it working without a password?)", r->name, r->host);
        child_died(&r->ssh, now);
        return;
    }
    snprintf(fwd, sizeof fwd, "%s:%s", r->remote_sock, r->sock);
    snprintf(log, sizeof log, "%s/ssh.log", r->dir);
    r->ssh.pid = spawn(argv, log);
    r->ssh.t0 = now;
    if (r->ssh.pid < 0) { r->ssh.pid = 0; child_died(&r->ssh, now); }
}

static void runner_tick(struct runner *r)
{
    time_t now = time(NULL);
    if (!child_alive(&r->daud, now) && now >= r->daud.wait_until)
        start_daud(r, now);
    if (!child_alive(&r->ssh, now) && now >= r->ssh.wait_until)
        start_ssh(r, now);
}

static void runner_free(struct runner *r)
{
    child_stop(&r->ssh);
    child_stop(&r->daud);
    free(r->name);
    free(r->directory);
    free(r->host);
}

static int same(const struct runner *r, const struct p9share_export *e)
{
    return !strcmp(r->name, e->name) && !strcmp(r->directory, e->directory) && !strcmp(r->host, e->host);
}

int p9_serve(void)
{
    char state[P9_LINE], lockp[P9_LINE + 16], confp[P9_LINE];
    struct runner *run = NULL;
    size_t nrun = 0, i, j;
    struct stat last;
    int lk, have_last = 0;
    struct sigaction sa;

    p9_state_dir(state, sizeof state);
    snprintf(lockp, sizeof lockp, "%s/serve.lock", state);
    if (p9_mkdir_p(state, 0700) < 0 || (lk = p9_lock(lockp)) < 0) {
        fprintf(stderr, "p9share: serve is already running for you\n");
        return 1;
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!stopping) {
        struct stat st;
        int changed = stat(confp, &st) == 0 ? (!have_last || st.st_mtime != last.st_mtime || st.st_size != last.st_size) : have_last;
        if (changed) {
            struct p9share_conf c;
            p9share_conf_init(&c);
            have_last = stat(confp, &last) == 0;
            if (have_last)
                p9share_conf_parse(confp, &c);
            for (i = 0; i < nrun; ) {           /* stop what is gone or changed */
                for (j = 0; j < c.nexports && !same(&run[i], &c.exports[j]); j++)
                    ;
                if (j == c.nexports) {
                    runner_free(&run[i]);
                    run[i] = run[--nrun];
                } else
                    i++;
            }
            for (j = 0; j < c.nexports; j++) {  /* start what is new */
                struct runner *r;
                for (i = 0; i < nrun && strcmp(run[i].name, c.exports[j].name); i++)
                    ;
                if (i < nrun)
                    continue;
                if (!(r = realloc(run, (nrun + 1) * sizeof *run)))
                    continue;
                run = r;
                r = &run[nrun++];
                memset(r, 0, sizeof *r);
                r->name = strdup(c.exports[j].name);
                r->directory = strdup(c.exports[j].directory);
                r->host = strdup(c.exports[j].host);
                r->daud.delay = r->ssh.delay = 1;
                snprintf(r->dir, sizeof r->dir, "%s/%s", state, r->name);
                snprintf(r->sock, sizeof r->sock, "%s/daud.sock", r->dir);
                if (strlen(r->sock) > 100) {    /* sockaddr_un.sun_path holds 108 bytes */
                    p9_log("%s: the socket path %s is too long for a unix socket; export it from a shorter state directory (XDG_STATE_HOME)",
                           r->name, r->sock);
                    runner_free(r);
                    nrun--;
                    continue;
                }
                p9_mkdir_p(r->dir, 0700);
                p9_log("sharing %s with %s as %s", r->directory, r->host, r->name);
            }
            p9share_conf_free(&c);
        }
        for (i = 0; i < nrun; i++)
            runner_tick(&run[i]);
        for (j = 0; j < 10 && !stopping; j++)   /* a second, in pieces, so that a signal is noticed */
            usleep(100000);
    }
    for (i = 0; i < nrun; i++)
        runner_free(&run[i]);
    free(run);
    close(lk);
    return 0;
}
