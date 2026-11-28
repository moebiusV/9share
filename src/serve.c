/* serve.c -- the exporter: for each export of the user's 9share.conf, one daud (the 9P server, unprivileged) that dials out:
 * `daud --dial` runs ssh to the other machine, there `9share link` joins ssh to the receiving relay, and the 9P connection
 * rides a resumable link over that, so a dropped ssh is run again and the connection, with its fids, waits for it.  daud is
 * started again if it dies, waiting 1, 2, 4 ... 30 seconds after quick deaths.  The file is re-read as it changes; only the
 * exports whose block changed are restarted.  The file is read when serve starts and when it gets SIGHUP (`9share reload`, which
 * the commands that edit the file run themselves), not by looking at it every second; its permissions are still looked at on every pass.
 *
 * An import is the other direction: the files are on the other machine and this one's ssh dials out.  For each import block the
 * runner is `ssh HOST 9share offer NAME DIR`, with the relay's socket (the mount of the same name, run by the root daemon) as its
 * standard input and output; there `offer` starts a daud that dials back through it.  ssh is run again, as daud is, with the same
 * waits. */
#include "9share.h"
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

#define PB (2 * P9_LINE + 64)
#define DEFAULT_HOLD 600

struct runner {
    char   *name, *directory, *host;
    int     hold, import;                    /* import: the files are on host (ssh runs `9share offer`), not here */
    char    dir[PB];                         /* its state directory: daud's log */
    struct sstamp { int sr; time_t sec; long nsec; off_t size; ino_t ino; } ss;   /* the secret file as daud was started with it */
    pid_t   pid;
    time_t  t0, wait_until;
    int     delay;
};

static volatile sig_atomic_t stopping, reload;

static void on_term(int sig) { (void)sig; stopping = 1; }
static void on_hup(int sig) { (void)sig; reload = 1; }

static void runner_stop(struct runner *r)
{
    int i, st;
    if (r->pid <= 0)
        return;
    kill(r->pid, SIGTERM);
    for (i = 0; i < 100; i++) {
        if (waitpid(r->pid, &st, WNOHANG) == r->pid) { r->pid = 0; return; }
        usleep(100000);
    }
    kill(r->pid, SIGKILL);
    waitpid(r->pid, &st, 0);
    r->pid = 0;
}

/* the state of the share's secret file: whether it is there, usable, and which version; SPATH is where it is */
static int secret_stamp(const char *name, struct sstamp *ss, char *spath, size_t pn, char *why, size_t wn)
{
    char secret[256];
    struct stat st;
    memset(ss, 0, sizeof *ss);
    ss->sr = p9_secret_load(NULL, name, secret, sizeof secret, spath, pn, why, wn);
    memset(secret, 0, sizeof secret);
    if (ss->sr != 1 && stat(spath, &st) == 0) {
        ss->sec = st.st_mtim.tv_sec;
        ss->nsec = st.st_mtim.tv_nsec;
        ss->size = st.st_size;
        ss->ino = st.st_ino;
    }
    return ss->sr;
}

/* an import: ssh, its standard input and output the relay of the mount of the same name; the relay may not be there yet */
static void import_start(struct runner *r, time_t now)
{
    char hold[16], log[PB + 16];
    char *argv[24];
    int ac = 0, sock = p9_link_connect(r->name);
    if (sock < 0) {                                  /* the mount is not wanted (or not up) yet: look again soon, with no backoff */
        r->wait_until = now + 2;
        return;
    }
    snprintf(hold, sizeof hold, "%d", r->hold ? r->hold : DEFAULT_HOLD);
    snprintf(log, sizeof log, "%s/ssh.log", r->dir);
    argv[ac++] = "ssh"; argv[ac++] = "-T";
    argv[ac++] = "-o"; argv[ac++] = "BatchMode=yes";
    argv[ac++] = "-o"; argv[ac++] = "ServerAliveInterval=15";
    argv[ac++] = "-o"; argv[ac++] = "ServerAliveCountMax=3";
    argv[ac++] = "-o"; argv[ac++] = "ConnectTimeout=10";
    argv[ac++] = r->host;
    argv[ac++] = "9share"; argv[ac++] = "offer"; argv[ac++] = r->name; argv[ac++] = r->directory;
    argv[ac++] = "--hold"; argv[ac++] = hold;
    argv[ac] = NULL;
    r->t0 = now;
    r->pid = fork();
    if (r->pid == 0) {
        int lf = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        dup2(sock, 0);
        dup2(sock, 1);
        dup2(lf, 2);
        if (sock > 2)
            close(sock);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(sock);
    if (r->pid < 0)
        r->pid = 0;
}

static void runner_start(struct runner *r, time_t now)
{
    char uid[32], hold[16], cmd[2 * P9_LINE], log[PB + 16], spath[P9_LINE], why[P9_LINE + 64];
    char *argv[16];
    int ac = 0;
    struct sstamp ss;
    if (r->import) {
        import_start(r, now);
        return;
    }
    /* a secret file makes the link prove it with SCRAM; one that is there but unusable stops the export (the link must
     * not quietly fall back to trusting the carrier) */
    if (secret_stamp(r->name, &ss, spath, sizeof spath, why, sizeof why) < 0) {
        p9_log("%s: not started: %s", r->name, why);
        r->wait_until = now + 30;
        r->ss = ss;
        return;
    }
    r->ss = ss;
    argv[ac++] = (char *)p9_daud();
    argv[ac++] = "-f"; argv[ac++] = "-n"; argv[ac++] = "-N";
    argv[ac++] = "-u"; argv[ac++] = uid;
    argv[ac++] = "-e"; argv[ac++] = r->directory;
    argv[ac++] = "--dial"; argv[ac++] = cmd;
    argv[ac++] = "--hold"; argv[ac++] = hold;
    if (ss.sr == 0) {
        argv[ac++] = "--link-secret-file";
        argv[ac++] = spath;
    }
    argv[ac++] = "-L"; argv[ac++] = "stderr";
    argv[ac] = NULL;
    snprintf(uid, sizeof uid, "%u", (unsigned)getuid());
    snprintf(hold, sizeof hold, "%d", r->hold ? r->hold : DEFAULT_HOLD);
    /* the carrier: ssh, which runs `9share link` on the other machine (names and host were checked when the file was read) */
    snprintf(cmd, sizeof cmd,
             "exec ssh -T -o BatchMode=yes -o ServerAliveInterval=15 -o ServerAliveCountMax=3 -o ConnectTimeout=10 %s 9share link %s",
             r->host, r->name);
    snprintf(log, sizeof log, "%s/daud.log", r->dir);
    r->t0 = now;
    r->pid = fork();
    if (r->pid == 0) {
        int lf = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600), dn = open("/dev/null", O_RDONLY);
        dup2(dn, 0);
        dup2(lf, 1);
        dup2(lf, 2);
        umask(077);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (r->pid < 0)
        r->pid = 0;
}

/* the longest wait between starts of a runner that keeps dying: 30 seconds ($NINESHARE_SERVE_DELAY_MAX, for the tests, says otherwise) */
static int delay_max(void)
{
    const char *e = getenv("NINESHARE_SERVE_DELAY_MAX");
    int n = e ? atoi(e) : 0;
    return n >= 1 && n <= 30 ? n : 30;
}

static void runner_tick(struct runner *r)
{
    time_t now = time(NULL);
    int st;
    char spath[P9_LINE], why[P9_LINE + 64];
    struct sstamp ss;
    if (!r->import && (r->pid > 0 || r->ss.sr < 0)) {          /* running, or held back by an unusable secret */
        secret_stamp(r->name, &ss, spath, sizeof spath, why, sizeof why);
        if (ss.sr != r->ss.sr || ss.sec != r->ss.sec || ss.nsec != r->ss.nsec || ss.size != r->ss.size || ss.ino != r->ss.ino) {
            p9_log("%s: the secret changed%s", r->name, r->pid > 0 ? "; restarting" : "");   /* appeared, went, or was replaced */
            runner_stop(r);
            r->wait_until = 0;
            r->delay = 1;
        }
    }
    if (r->pid > 0 && waitpid(r->pid, &st, WNOHANG) == r->pid) {
        if (now - r->t0 > 60)
            r->delay = 1;
        r->wait_until = now + r->delay;
        r->delay = r->delay * 2 > delay_max() ? delay_max() : r->delay * 2;
        r->pid = 0;
        p9_log("%s: %s ended; again in %d seconds", r->name, r->import ? "ssh" : "daud", (int)(r->wait_until - now));
    }
    if (r->pid <= 0 && now >= r->wait_until)
        runner_start(r, now);
}

static void runner_free(struct runner *r)
{
    runner_stop(r);
    free(r->name);
    free(r->directory);
    free(r->host);
}

static int same(const struct runner *r, const char *name, const char *directory, const char *host, int hold, int import)
{
    return r->import == import && !strcmp(r->name, name) && !strcmp(r->directory, directory) && !strcmp(r->host, host) && r->hold == hold;
}

/* is the runner still what the file says (an export, or an import, of that name and settings) */
static int wanted(const struct runner *r, const struct nineshare_conf *c)
{
    size_t j;
    if (r->import) {
        for (j = 0; j < c->nimports; j++)
            if (same(r, c->imports[j].name, c->imports[j].directory, c->imports[j].host, c->imports[j].hold, 1))
                return 1;
        return 0;
    }
    for (j = 0; j < c->nexports; j++)
        if (same(r, c->exports[j].name, c->exports[j].directory, c->exports[j].host, c->exports[j].hold, 0))
            return 1;
    return 0;
}

static void runner_add(struct runner **run, size_t *nrun, const char *state, const char *name, const char *directory, const char *host, int hold, int import)
{
    struct runner *r;
    size_t i;
    for (i = 0; i < *nrun && strcmp((*run)[i].name, name); i++)
        ;
    if (i < *nrun)
        return;                                       /* a name is one share: the first block of it wins */
    if (!(r = realloc(*run, (*nrun + 1) * sizeof *r)))
        return;
    *run = r;
    r = &r[(*nrun)++];
    memset(r, 0, sizeof *r);
    r->name = strdup(name);
    r->directory = strdup(directory);
    r->host = strdup(host);
    r->hold = hold;
    r->import = import;
    r->delay = 1;
    snprintf(r->dir, sizeof r->dir, "%s/%s", state, r->name);
    p9_mkdir_p(r->dir, 0700);
    if (import)
        p9_log("importing %s from %s as %s", r->directory, r->host, r->name);
    else
        p9_log("sharing %s with %s as %s", r->directory, r->host, r->name);
}

int p9_serve(void)
{
    char state[P9_LINE], lockp[P9_LINE + 16], confp[P9_LINE];
    struct runner *run = NULL;
    size_t nrun = 0, i, j;
    int lk, have_last = 0, last_trust = 2, first = 1;  /* 2: nothing seen yet */
    char pidp[PB + 16];
    struct sigaction sa;

    p9_state_dir(state, sizeof state);
    snprintf(lockp, sizeof lockp, "%s/serve.lock", state);
    if (p9_mkdir_p(state, 0700) < 0 || (lk = p9_lock(lockp)) < 0) {
        fprintf(stderr, "9share: serve is already running for you\n");
        return 1;
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    snprintf(pidp, sizeof pidp, "%s/serve.pid", state);
    {
        char num[32];
        snprintf(num, sizeof num, "%d\n", (int)getpid());
        p9_write_file(pidp, num, strlen(num), 0600);
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    while (!stopping) {
        struct stat st;
        char why[P9_LINE + 256];
        int tr = p9_conf_trust(confp, (unsigned)getuid(), why, sizeof why);       /* looked at on every pass: a chmod changes neither mtime nor size */
        int changed = first || reload || tr != last_trust;
        if (changed) {
            struct nineshare_conf c;
            first = 0;
            reload = 0;                          /* before reading: a SIGHUP that comes while we read asks again */
            nineshare_conf_init(&c);
            have_last = stat(confp, &st) == 0;
            if (tr < 0)
                fprintf(stderr, "9share: %s is not used: %s\n", confp, why);
            else if (last_trust < 0 && tr >= 0)
                fprintf(stderr, "9share: %s is used again\n", confp);
            last_trust = tr;
            if (have_last && tr == 0)
                nineshare_conf_parse(confp, &c);
            for (i = 0; i < nrun; ) {           /* stop what is gone or changed */
                if (!wanted(&run[i], &c)) {
                    runner_free(&run[i]);
                    run[i] = run[--nrun];
                } else
                    i++;
            }
            for (j = 0; j < c.nexports; j++)    /* start what is new */
                runner_add(&run, &nrun, state, c.exports[j].name, c.exports[j].directory, c.exports[j].host, c.exports[j].hold, 0);
            for (j = 0; j < c.nimports; j++)
                runner_add(&run, &nrun, state, c.imports[j].name, c.imports[j].directory, c.imports[j].host, c.imports[j].hold, 1);
            nineshare_conf_free(&c);
        }
        for (i = 0; i < nrun; i++)
            runner_tick(&run[i]);
        for (j = 0; j < 10 && !stopping && !reload; j++)   /* a second, in pieces, so that a signal is noticed */
            usleep(100000);
    }
    for (i = 0; i < nrun; i++)
        runner_free(&run[i]);
    free(run);
    unlink(pidp);
    close(lk);
    return 0;
}
