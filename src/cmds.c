/* cmds.c -- the commands a person types: export, unexport, import, unimport, mount, unmount, rw|ro|off, status, wsl-boot. */
#include "cmds.h"
#include "9share.h"
#include "../daud/src/liblink/crypto.h"
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PB (2 * P9_LINE + 64)

static int fail(const char *fmt, const char *a)
{
    fprintf(stderr, "9share: ");
    fprintf(stderr, fmt, a);
    fputc('\n', stderr);
    return 1;
}

/* what ssh would log in as on HOST, to print the command to run there */
static void ssh_user(const char *host, char *out, size_t n)
{
    char buf[8192], *p;
    char *argv[] = { "ssh", "-G", (char *)host, NULL };
    snprintf(out, n, "USER");
    if (p9_run(argv, 10, buf, sizeof buf) != 0)
        return;
    for (p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, "user ", 5)) {
            size_t k = strcspn(p + 5, "\n");
            snprintf(out, n, "%.*s", (int)k, p + 5);
            return;
        }
}

/* A configuration that the daemon (or serve) would not use, because someone else could have changed it, is not edited either: the change would be made
 * to a file that is ignored.  See p9_conf_trust. */
static int conf_refused(const char *path, unsigned owner)
{
    char why[P9_LINE + 256];
    if (p9_conf_trust(path, owner, why, sizeof why) >= 0)
        return 0;
    fprintf(stderr, "9share: nothing was changed: %s\n", why);
    return 1;
}

int cmd_export(int argc, char **argv)
{
    char confp[PB], dir[P9_LINE], rp[PATH_MAX], text[3 * P9_LINE], user[128];
    const char *host, *name;
    struct nineshare_conf c;
    struct stat st;
    size_t i;
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: 9share export DIR HOST [NAME]\n");
        return 2;
    }
    /* realpath may write PATH_MAX bytes: into a buffer of that size, then what fits */
    if (!realpath(argv[0], rp) || stat(rp, &st) < 0 || !S_ISDIR(st.st_mode))
        return fail("%s is not a directory", argv[0]);
    if (strlen(rp) >= sizeof dir)
        return fail("%s: the path is too long", argv[0]);
    snprintf(dir, sizeof dir, "%s", rp);
    host = argv[1];
    if (argc == 3)
        name = argv[2];
    else {
        name = strrchr(dir, '/') + 1;
        if (!*name)
            return fail("give a name for %s", dir);
    }
    if (!nineshare_valid_name(name))
        return fail("'%s' cannot be the name of an export (letters, digits and . _ -)", name);
    if (!nineshare_valid_host(host))
        return fail("'%s' is not a host name as ssh knows it", host);
    if (!nineshare_valid_path(dir))
        return fail("%s: the directory needs an absolute path of letters, digits and . _ + @ - / (no spaces)", dir);
    p9_user_conf_path(NULL, confp, sizeof confp);
    if (conf_refused(confp, (unsigned)getuid()))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nexports; i++)
        if (!strcmp(c.exports[i].name, name)) {
            if (p9_block_remove(confp, "export", name, c.exports[i].line) < 0)
                return fail("cannot replace the export '%s' in the configuration", name);
            break;
        }
    nineshare_conf_free(&c);
    snprintf(text, sizeof text, "export \"%s\" {\n\tdirectory \"%s\"\n\thost \"%s\"\n}\n", name, dir, host);
    if (p9_block_append(confp, text) < 0)
        return fail("cannot write %s", confp);
    p9_reload_serve();
    printf("sharing %s with %s as '%s'\n", dir, host, name);
    ssh_user(host, user, sizeof user);
    printf("on %s:   sudo 9share mount %s %s /mnt/%s\n", host, user, name, name);
    {
        char state[P9_LINE], lockp[PB];
        p9_state_dir(state, sizeof state);
        snprintf(lockp, sizeof lockp, "%s/serve.lock", state);
        if (!p9_lock_held(lockp))
            printf("(nothing runs your exports yet: the 9share service does it at boot; to start it now and at every boot, run  sudo %s/9share-init enable)\n", NINESHARE_LIBEXECDIR);
    }
    return 0;
}

int cmd_unexport(int argc, char **argv)
{
    char confp[PB];
    struct nineshare_conf c;
    size_t i;
    int rc = 1;
    if (argc != 1) {
        fprintf(stderr, "usage: 9share unexport NAME\n");
        return 2;
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    if (conf_refused(confp, (unsigned)getuid()))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nexports; i++)
        if (!strcmp(c.exports[i].name, argv[0])) {
            if (p9_block_remove(confp, "export", argv[0], c.exports[i].line) == 0) {
                p9_reload_serve();
                printf("no longer sharing '%s'\n", argv[0]);
                rc = 0;
            } else
                fail("cannot change %s", confp);
            break;
        }
    if (i == c.nexports)
        fail("no export named '%s'", argv[0]);
    nineshare_conf_free(&c);
    return rc;
}

/* import HOST DIR [NAME]: show HOST's DIR here (absolute there, or relative to the home of the account ssh logs in as).  Needs the mount block too. */
int cmd_import(int argc, char **argv)
{
    char confp[PB], text[3 * P9_LINE], name[128];
    const char *host, *dir, *base;
    struct nineshare_conf c;
    struct passwd *pw = getpwuid(getuid());
    size_t i;
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: 9share import HOST DIR [NAME]\n");
        return 2;
    }
    host = argv[0];
    dir = argv[1];
    if (argc == 3)
        snprintf(name, sizeof name, "%s", argv[2]);
    else {
        base = strrchr(dir, '/');
        snprintf(name, sizeof name, "%s", base ? base + 1 : dir);
    }
    if (!nineshare_valid_name(name))
        return fail("'%s' cannot be the name of an import: give one (letters, digits and . _ -, not starting with . _ -) as a third argument", name);
    if (!nineshare_valid_host(host))
        return fail("'%s' is not a host name as ssh knows it", host);
    if (!nineshare_valid_rpath(dir))
        return fail("%s: the directory is a path of letters, digits and . _ + @ - / (no spaces, no ..), absolute or relative to the home there", dir);
    p9_user_conf_path(NULL, confp, sizeof confp);
    if (conf_refused(confp, (unsigned)getuid()))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nexports; i++)
        if (!strcmp(c.exports[i].name, name)) {
            nineshare_conf_free(&c);
            return fail("'%s' is already an export: use another name", name);
        }
    for (i = 0; i < c.nimports; i++)
        if (!strcmp(c.imports[i].name, name)) {
            if (p9_block_remove(confp, "import", name, c.imports[i].line) < 0) {
                nineshare_conf_free(&c);
                return fail("cannot replace the import '%s' in the configuration", name);
            }
            break;
        }
    nineshare_conf_free(&c);
    snprintf(text, sizeof text, "import \"%s\" {\n\tdirectory \"%s\"\n\thost \"%s\"\n}\n", name, dir, host);
    if (p9_block_append(confp, text) < 0)
        return fail("cannot write %s", confp);
    p9_reload_serve();
    printf("importing %s from %s as '%s'\n", dir, host, name);
    printf("here, once, as root:   sudo 9share mount %s %s /mnt/%s\n", pw ? pw->pw_name : "USER", name, name);
    printf("(9share must be installed on %s, with ssh to it without a password)\n", host);
    return 0;
}

int cmd_unimport(int argc, char **argv)
{
    char confp[PB];
    struct nineshare_conf c;
    size_t i;
    int rc = 1;
    if (argc != 1) {
        fprintf(stderr, "usage: 9share unimport NAME\n");
        return 2;
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    if (conf_refused(confp, (unsigned)getuid()))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nimports; i++)
        if (!strcmp(c.imports[i].name, argv[0])) {
            if (p9_block_remove(confp, "import", argv[0], c.imports[i].line) == 0) {
                p9_reload_serve();
                printf("no longer importing '%s' (a mount of that name here is still configured: sudo 9share unmount %s)\n", argv[0], argv[0]);
                rc = 0;
            } else
                fail("cannot change %s", confp);
            break;
        }
    if (i == c.nimports)
        fail("no import named '%s'", argv[0]);
    nineshare_conf_free(&c);
    return rc;
}

int cmd_mount(int argc, char **argv)
{
    char confp[PB], text[3 * P9_LINE], at[P9_LINE];
    struct nineshare_conf c;
    size_t i;
    char accline[40] = "";                          /* the access line, when --access was given: what was chosen stays in the file */
    if (geteuid() != 0 && !getenv("NINESHARE_SYSCONFDIR"))
        return fail("%s needs root: run it with sudo", "mount");
    if (argc == 5 && !strcmp(argv[3], "--access") && (!strcmp(argv[4], "owner") || !strcmp(argv[4], "any"))) {
        snprintf(accline, sizeof accline, "\taccess %s\n", argv[4]);
        argc = 3;
    }
    if (argc != 3) {
        fprintf(stderr, "usage: sudo 9share mount USER NAME DIR [--access owner|any]\n");
        return 2;
    }
    if (!nineshare_valid_user(argv[0]) || !getpwnam(argv[0]))
        return fail("'%s' is not a user of this machine", argv[0]);
    if (!nineshare_valid_name(argv[1]))
        return fail("'%s' cannot be the name of a mount (letters, digits and . _ -)", argv[1]);
    if (argv[2][0] == '/')
        snprintf(at, sizeof at, "%s", argv[2]);
    else if (!getcwd(at, sizeof at) || strlen(at) + strlen(argv[2]) + 2 > sizeof at)
        return fail("%s: cannot make the path absolute", argv[2]);
    else
        strcat(strcat(at, "/"), argv[2]);
    if (!nineshare_valid_path(at))
        return fail("%s: the mount point needs an absolute path of letters, digits and . _ + @ - / (no spaces)", at);
    p9_system_conf_path(confp, sizeof confp);
    if (conf_refused(confp, getenv("NINESHARE_SYSCONFDIR") ? (unsigned)getuid() : 0))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[1])) {
            if (p9_block_remove(confp, "mount", argv[1], c.mounts[i].line) < 0)
                return fail("cannot replace the mount '%s' in the configuration", argv[1]);
            break;
        }
    nineshare_conf_free(&c);
    snprintf(text, sizeof text, "mount \"%s\" {\n\tuser \"%s\"\n\tat \"%s\"\n%s}\n", argv[1], argv[0], at, accline);
    if (p9_block_append(confp, text) < 0)
        return fail("cannot write %s", confp);
    p9_mkdir_p(at, 0755);
    p9_reload_daemon();
    printf("'%s' of %s will be shown at %s as soon as the exporter's tunnel is up\n", argv[1], argv[0], at);
    if (p9_daemon_pid() <= 0 && !p9_lock_held("/run/9share.lock"))
        printf("(the daemon is not running: to start it now and at every boot, run  sudo %s/9share-init enable)\n", NINESHARE_LIBEXECDIR);
    return 0;
}

int cmd_unmount(int argc, char **argv)
{
    char confp[PB];
    struct nineshare_conf c;
    size_t i;
    int rc = 1;
    if (geteuid() != 0)
        return fail("%s needs root: run it with sudo", "unmount");
    if (argc != 1) {
        fprintf(stderr, "usage: sudo 9share unmount NAME\n");
        return 2;
    }
    p9_system_conf_path(confp, sizeof confp);
    if (conf_refused(confp, getenv("NINESHARE_SYSCONFDIR") ? (unsigned)getuid() : 0))
        return 1;
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[0])) {
            p9_unmount(c.mounts[i].at);
            if (p9_block_remove(confp, "mount", argv[0], c.mounts[i].line) == 0) {
                p9_reload_daemon();
                printf("unmounted %s\n", c.mounts[i].at);
                rc = 0;
            } else
                fail("cannot change %s", confp);
            break;
        }
    if (i == c.nmounts)
        fail("no mount named '%s'", argv[0]);
    nineshare_conf_free(&c);
    return rc;
}

int cmd_mode(const char *mode, int argc, char **argv)
{
    char confp[PB], path[PB], text[16];
    struct nineshare_conf c;
    struct passwd *pw = getpwuid(getuid());
    size_t i;
    int found = 0;
    if (argc != 1 || !pw) {
        fprintf(stderr, "usage: 9share %s NAME\n", mode);
        return 2;
    }
    p9_system_conf_path(confp, sizeof confp);
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[0]) && !strcmp(c.mounts[i].user, pw->pw_name))
            found = 1;
    nineshare_conf_free(&c);
    if (!found)
        return fail("no mount named '%s' for you here (see: 9share status)", argv[0]);
    p9_mode_path(pw->pw_dir, argv[0], path, sizeof path);
    snprintf(text, sizeof text, "%s\n", mode);
    if (p9_write_file(path, text, strlen(text), 0644) < 0)
        return fail("cannot write %s", path);
    printf("%s: %s (the mount follows within a few seconds)\n", argv[0], mode);
    return 0;
}

/* what daud's log says last about the link: up, or down and waiting */
static const char *link_state(const char *logpath)
{
    char *text = p9_read_file(logpath, NULL), *p;
    const char *r = "starting";
    if (!text)
        return r;
    for (p = text; (p = strstr(p, "link: ")); p += 6) {         /* the last of the three messages says how it is */
        if (!strncmp(p, "link: up", 8))
            r = "link up";
        else if (!strncmp(p, "link: carrier lost", 18))
            r = "link down, waiting for ssh to come back";
        else if (!strncmp(p, "link: the session is over", 25))
            r = "session over, a new one starting";
    }
    free(text);
    return r;
}

int cmd_status(void)
{
    char confp[PB], state[P9_LINE], sockp[PB];
    struct nineshare_conf c;
    size_t i;
    int dpid = p9_daemon_pid(), daemon = dpid > 0 || (!getenv("NINESHARE_RUNDIR") && p9_lock_held("/run/9share.lock"));
    if (daemon && dpid > 0)
        printf("daemon:  running (pid %d)\n", dpid);
    else
        printf("daemon:  %s\n", daemon ? "running" : "not running (sudo " NINESHARE_LIBEXECDIR "/9share-init enable)");
    {   /* a configuration file that others could change is not used: say so, where it is looked at */
        char why[P9_LINE + 256], path[PB];
        p9_system_conf_path(path, sizeof path);
        if (p9_conf_trust(path, 0, why, sizeof why) < 0)
            printf("warning: the daemon does not use %s: %s\n", path, why);
        p9_user_conf_path(NULL, path, sizeof path);
        if (p9_conf_trust(path, (unsigned)getuid(), why, sizeof why) < 0)
            printf("warning: 9share serve does not use %s: %s\n", path, why);
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    p9_state_dir(state, sizeof state);
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    if (c.nexports)
        printf("shared by you:\n");
    else
        printf("shared by you: nothing  (9share export DIR HOST)\n");
    for (i = 0; i < c.nexports; i++) {
        snprintf(sockp, sizeof sockp, "%s/%s/daud.log", state, c.exports[i].name);
        printf("  %-14s %s -> %s   %s\n", c.exports[i].name, c.exports[i].directory, c.exports[i].host, link_state(sockp));
    }
    for (i = 0; i < c.nimports; i++) {
        if (i == 0)
            printf("imported by you:\n");
        printf("  %-14s %s:%s   (ssh runs while the mount of that name is wanted)\n", c.imports[i].name, c.imports[i].host, c.imports[i].directory);
    }
    nineshare_conf_free(&c);
    p9_system_conf_path(confp, sizeof confp);
    nineshare_conf_init(&c);
    if (p9_file_exists(confp))
        nineshare_conf_parse(confp, &c);
    if (c.nmounts)
        printf("mounted here:\n");
    else
        printf("mounted here: nothing  (sudo 9share mount USER NAME DIR)\n");
    for (i = 0; i < c.nmounts; i++) {
        struct passwd *pw = getpwnam(c.mounts[i].user);
        char sock[PB];
        const char *want = "?";
        int up = 0;
        if (pw) {
            want = p9_mode_of(pw->pw_dir, c.mounts[i].name);
            snprintf(sock, sizeof sock, "%s/.9share/%s.link", pw->pw_dir, c.mounts[i].name);
            up = p9_file_exists(sock);
        }
        printf("  %-14s %s  from %s   wanted: %s   now: %s   relay: %s\n", c.mounts[i].name, c.mounts[i].at, c.mounts[i].user, want,
               p9_mount_state_name(p9_mount_state(c.mounts[i].at)), up ? "waiting for the exporter" : "not running");
    }
    nineshare_conf_free(&c);
    return 0;
}

/* wsl-boot --remove: take 9share out of the [boot] command of /etc/wsl.conf; a command of the user's that was kept comes back */
static int wsl_boot_remove(void)
{
    const char *path = getenv("NINESHARE_WSL_CONF") ? getenv("NINESHARE_WSL_CONF") : "/etc/wsl.conf";
    char *text = p9_read_file(path, NULL), *out, *line, *save = NULL, script[PB];
    int in_boot = 0, changed = 0;
    if (geteuid() != 0 && !getenv("NINESHARE_WSL_CONF"))
        return fail("%s needs root: run it with sudo", "wsl-boot --remove");
    if (!text)
        return 0;
    snprintf(script, sizeof script, "%s/wsl-boot", p9_sysconf_dir());
    if (!(out = calloc(1, strlen(text) + 4 * P9_LINE)))
        return 1;
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[')
            in_boot = !strncasecmp(t, "[boot]", 6);
        if (in_boot && !strncmp(t, "command", 7) && strchr(t, '=') && strstr(t, "9share")) {
            if (strstr(t, script)) {                    /* the user's own command was kept in the script, on its second line */
                char *body = p9_read_file(script, NULL), *nl, *orig;
                if (body && (nl = strchr(body, '\n')) && (orig = nl + 1) && (nl = strchr(orig, '\n'))) {
                    *nl = '\0';
                    if (!strstr(orig, "9share"))
                        sprintf(out + strlen(out), "command = \"%s\"\n", orig);
                }
                free(body);
                unlink(script);
            }
            changed = 1;
            continue;
        }
        sprintf(out + strlen(out), "%s\n", line);
    }
    if (changed) {
        if (p9_write_file(path, out, strlen(out), 0644) < 0)
            return fail("cannot write %s", path);
        printf("%s no longer starts 9share at boot.\n", path);
    }
    free(text);
    free(out);
    return 0;
}

/* shutdown and restart: the root daemon.  Under systemd the service goes through systemd (a process that was only signalled would be
 * started again at once: Restart=always); otherwise the process gets SIGTERM and is waited for, as long as it takes to end the mounts.
 * NINESHARE_RUNDIR (tests) keeps the root check and systemd out of it. */
static int systemd_runs_9share(void)
{
    char *av[] = { "systemctl", "is-active", "--quiet", "9share.service", NULL };
    if (getenv("NINESHARE_RUNDIR") || !p9_file_exists("/run/systemd/system"))
        return 0;
    return p9_run(av, 10, NULL, 0) == 0;
}

static int need_root(const char *what)
{
    if (geteuid() != 0 && !getenv("NINESHARE_RUNDIR"))
        return fail("%s needs root: run it with sudo", what);
    return 0;
}

static int daemon_stop(int force)                       /* 0: it is stopped (or was not running); 1: it is not */
{
    char pidp[P9_LINE], msg[P9_LINE];
    const char *w = getenv("NINESHARE_SHUTDOWN_WAIT");
    int pid, wait_s = w && atoi(w) > 0 ? atoi(w) : 30, i;
    if (systemd_runs_9share()) {
        char *av[] = { "systemctl", "stop", "9share.service", NULL };
        if (p9_run(av, 120, NULL, 0) != 0)
            return fail("%s", "systemctl stop 9share.service failed");
        printf("9share: stopped (the 9share service)\n");
        return 0;
    }
    if ((pid = p9_daemon_pid()) <= 0) {
        printf("9share: the daemon is not running\n");
        return 0;
    }
    snprintf(pidp, sizeof pidp, "%s/9share.pid", p9_run_dir());
    kill(pid, SIGTERM);
    for (i = 0; i < wait_s * 10 && p9_proc_alive(pid); i++)
        usleep(100000);
    if (p9_proc_alive(pid)) {
        if (!force) {
            snprintf(msg, sizeof msg, "the daemon (pid %d) is still there after %d s: it is either still taking the mounts down, or stuck. "
                     "Wait and run this again, or use  9share shutdown --force  (it kills the daemon; mounts and exports may stay behind)", pid, wait_s);
            return fail("%s", msg);
        }
        kill(pid, SIGKILL);
        for (i = 0; i < 50 && p9_proc_alive(pid); i++)
            usleep(100000);
        if (p9_proc_alive(pid)) {
            snprintf(msg, sizeof msg, "the daemon (pid %d) did not end even when killed", pid);
            return fail("%s", msg);
        }
        printf("9share: killed the daemon (pid %d); a mount or export it had may still be there (9share status)\n", pid);
    } else {
        printf("9share: stopped the daemon (pid %d)\n", pid);
    }
    unlink(pidp);
    return 0;
}

int cmd_shutdown(int argc, char **argv)
{
    int force = 0;
    if (argc == 1 && !strcmp(argv[0], "--force"))
        force = 1;
    else if (argc != 0) {
        fprintf(stderr, "usage: 9share shutdown [--force]\n");
        return 2;
    }
    if (need_root("shutdown"))
        return 1;
    return daemon_stop(force);
}

/* reload: make the running programs read their configuration again.  The commands that edit it do this themselves; this is for a
 * file that was edited by hand.  As an ordinary user: that user's exports and imports (9share serve).  As root: the daemon, which
 * re-reads /etc/9share/9share.conf (the mounts) and has the serves it started read their files too. */
int cmd_reload(int argc, char **argv)
{
    int did = 0, root = geteuid() == 0 || getenv("NINESHARE_RUNDIR");
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "usage: 9share reload\n");
        return 2;
    }
    if (p9_reload_serve()) {
        printf("9share: asked your exports and imports (9share serve) to read %s again\n", "~/.config/9share/9share.conf");
        did++;
    }
    if (root && p9_reload_daemon()) {
        printf("9share: asked the daemon to read %s again\n", "/etc/9share/9share.conf");
        did++;
    }
    if (!did) {
        if (root)
            return fail("%s", "nothing to reload: the daemon is not running (sudo " NINESHARE_LIBEXECDIR "/9share-init enable)");
        return fail("%s", "nothing to reload: 9share serve is not running for you (the daemon starts it within ten seconds of a configuration file appearing); the mounts are the daemon's: sudo 9share reload");
    }
    return 0;
}

int cmd_restart(int argc, char **argv)
{
    const char *hook = getenv("NINESHARE_RUNDIR") ? getenv("NINESHARE_BOOT_CMD") : NULL;     /* tests: what stands in for the start */
    char *self, *av[4];
    int i, pid = 0;
    (void)argv;
    if (argc != 0) {
        fprintf(stderr, "usage: 9share restart\n");
        return 2;
    }
    if (need_root("restart"))
        return 1;
    if (systemd_runs_9share()) {
        char *sv[] = { "systemctl", "restart", "9share.service", NULL };
        if (p9_run(sv, 120, NULL, 0) != 0)
            return fail("%s", "systemctl restart 9share.service failed");
        printf("9share: restarted (the 9share service)\n");
        return 0;
    }
    if (daemon_stop(0))
        return 1;
    if (hook) {
        char *hv[] = { "sh", "-c", (char *)hook, NULL };
        return p9_run(hv, 30, NULL, 0) == 0 ? 0 : fail("%s", "cannot start the daemon");
    }
    self = p9_self();
    av[0] = self ? self : "9share";
    av[1] = "boot";
    av[2] = "--detach";
    av[3] = NULL;
    i = p9_run(av, 20, NULL, 0);
    free(self);
    if (i != 0)
        return fail("%s", "cannot start the daemon (see /var/log/9share.log)");
    for (i = 0; i < 50 && (pid = p9_daemon_pid()) <= 0; i++)
        usleep(100000);
    if (pid <= 0)
        return fail("%s", "the daemon did not start (see /var/log/9share.log)");
    printf("9share: started the daemon (pid %d)\n", pid);
    return 0;
}

/* readahead: how far ahead the relays of this machine read.  The setting is the file prefetch-kb of the system directory; the relays look at it about
 * once a second, so a change needs no restart and no remount.  Without an argument it says what is set. */
int cmd_readahead(int argc, char **argv)
{
    char path[P9_LINE + 32], msg[P9_LINE + 128];
    int kb, bad;
    snprintf(path, sizeof path, "%s/prefetch-kb", p9_sysconf_dir());
    if (argc > 1) {
        fprintf(stderr, "usage: 9share readahead [off|on|KB]\n");
        return 2;
    }
    if (argc == 1) {
        char *end;
        long v = !strcmp(argv[0], "off") ? 0 : strtol(argv[0], &end, 10);
        int is_on = !strcmp(argv[0], "on") || !strcmp(argv[0], "default");
        if (!is_on && strcmp(argv[0], "off") != 0 && (*argv[0] == '\0' || *end != '\0' || v < 0 || v > 65536)) {
            fprintf(stderr, "usage: 9share readahead [off|on|KB]   (KB: 1 to 65536)\n");
            return 2;
        }
        if (geteuid() != 0 && !getenv("NINESHARE_SYSCONFDIR"))
            return fail("%s needs root: run it with sudo", "readahead");
        if (is_on) {                                       /* "on" is the default, which is no file at all */
            if (unlink(path) != 0 && errno != ENOENT) {
                snprintf(msg, sizeof msg, "cannot remove %s: %s", path, strerror(errno));
                return fail("%s", msg);
            }
        } else {
            char num[32];
            snprintf(num, sizeof num, "%ld\n", v);
            if (p9_write_file(path, num, strlen(num), 0644) < 0) {
                snprintf(msg, sizeof msg, "cannot write %s: %s", path, strerror(errno));
                return fail("%s", msg);
            }
        }
    }
    bad = p9_prefetch_kb(&kb) < 0;
    if (bad)
        printf("9share: %s is not usable: the relays keep what they have\n", path);
    else if (kb == 0)
        printf("9share: read-ahead is off");
    else
        printf("9share: read-ahead is %d KB%s", kb, access(path, F_OK) != 0 ? " (the default)" : "");
    if (!bad)
        printf("%s\n", argc == 1 ? "; mounts in use take it up within a second or two" : "");
    return bad ? 1 : 0;
}

/* WSL2 has no systemd: the [boot] command of /etc/wsl.conf starts the daemon.  A command that is there is kept and runs first. */
int cmd_wsl_boot(int argc, char **argv)
{
    const char *path = getenv("NINESHARE_WSL_CONF") ? getenv("NINESHARE_WSL_CONF") : "/etc/wsl.conf";
    char *text = p9_read_file(path, NULL), *out, *line, *save = NULL, *self, mine[P9_LINE];
    int in_boot = 0, done = 0, have_boot = 0, has_cmd = 0;
    size_t cap;
    if (argc == 1 && !strcmp(argv[0], "--remove"))
        return wsl_boot_remove();
    if (argc != 0) {
        fprintf(stderr, "usage: 9share wsl-boot [--remove]\n");
        return 2;
    }
    if (geteuid() != 0 && !getenv("NINESHARE_WSL_CONF"))
        return fail("%s needs root: run it with sudo", "wsl-boot");
    if (!text)
        text = strdup("");
    self = p9_self();
    snprintf(mine, sizeof mine, "%s boot --detach", self);
    free(self);
    cap = strlen(text) + 4 * P9_LINE;
    out = calloc(1, cap);
    if (!out)
        return 1;
    {   /* Is there a [boot] command already?  Look first: our own line goes in at the [boot] header, which comes BEFORE the
         * user's command, and writing both made a duplicate key (which of the two WSL runs is not something to depend on). */
        char *copy = strdup(text), *l, *sv = NULL;
        int b = 0;
        for (l = copy ? strtok_r(copy, "\n", &sv) : NULL; l; l = strtok_r(NULL, "\n", &sv)) {
            while (*l == ' ' || *l == '\t')
                l++;
            if (*l == '[')
                b = !strncasecmp(l, "[boot]", 6);
            else if (b && !strncmp(l, "command", 7) && strchr(l, '='))
                has_cmd = 1;
        }
        free(copy);
    }
    for (line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = line;
        while (*t == ' ' || *t == '\t')
            t++;
        if (*t == '[') {
            in_boot = !strncasecmp(t, "[boot]", 6);
            have_boot |= in_boot;
        }
        if (in_boot && !strncmp(t, "command", 7) && strchr(t, '=')) {
            char *old = strchr(t, '=') + 1;
            while (*old == ' ')
                old++;
            if (strstr(old, "9share")) {
                printf("already set: %s\n", old);
                free(text);
                free(out);
                return 0;
            }
            {
                char script[PB], body[3 * P9_LINE], *q = old;
                size_t n = strlen(q);
                if (n >= 2 && q[0] == '"' && q[n - 1] == '"') { q[n - 1] = '\0'; q++; }
                snprintf(script, sizeof script, "%s/wsl-boot", p9_sysconf_dir());
                snprintf(body, sizeof body, "#!/bin/sh\n%s\n%s\nexit 0\n", q, mine);
                if (p9_write_file(script, body, strlen(body), 0755) < 0)
                    return fail("cannot write %s", script);
                printf("kept your boot command '%s': it runs first, from %s\n", q, script);
                snprintf(out + strlen(out), cap - strlen(out), "command = %s\n", script);
                done = 1;
                continue;
            }
        }
        snprintf(out + strlen(out), cap - strlen(out), "%s\n", line);
        if (in_boot && !done && !has_cmd && !strncasecmp(t, "[boot]", 6)) {
            snprintf(out + strlen(out), cap - strlen(out), "command = %s\n", mine);
            done = 1;
        }
    }
    if (!have_boot)
        snprintf(out + strlen(out), cap - strlen(out), "\n[boot]\ncommand = %s\n", mine);
    if (p9_write_file(path, out, strlen(out), 0644) < 0)
        return fail("cannot write %s", path);
    printf("/etc/wsl.conf now starts 9share at boot.  Restart WSL once (wsl.exe --shutdown, from Windows), or start it now: sudo 9share boot --detach\n");
    free(text);
    free(out);
    return 0;
}

/* ---- pair: one secret, on both machines ---- */

/* `9share pair --receive NAME`, which ssh runs on the other machine: the secret comes in on standard input, never on a command line.
 * Exit status 3: there is one already (and --force was not given). */
static int pair_receive(const char *name, int force)
{
    char line[512], path[PB], *nl;
    size_t i;
    int rc;
    if (!fgets(line, sizeof line, stdin) || !(nl = strpbrk(line, "\r\n")))
        return fail("%s", "no secret on standard input");
    *nl = '\0';
    if (strlen(line) < 16 || strlen(line) > 255 || strspn(line, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != strlen(line))
        return fail("%s", "that is not a secret made by 9share pair");
    /* the receiving side reads it from HOME, as the relay does */
    if (p9_secret_path(getenv("HOME"), name, path, sizeof path) < 0)
        return fail("'%s' cannot be the name of a share", name);
    rc = p9_secret_store(path, line, force);
    for (i = 0; i < sizeof line; i++)
        line[i] = 0;
    if (rc == 1) {
        fprintf(stderr, "9share: %s already has a secret on this machine; run pair with --force to replace it\n", name);
        return 3;
    }
    if (rc < 0)
        return fail("cannot write the secret: %s", strerror(errno));
    return 0;
}

int cmd_pair(int argc, char **argv)
{
    const char *name = NULL, *host = NULL;
    int force = 0, receive = 0, i;
    char path[PB], raw[32], secret[64];
    struct stat st;
    int pfd[2], status = 0;
    pid_t pid;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--force")) force = 1;
        else if (!strcmp(argv[i], "--receive")) receive = 1;
        else if (argv[i][0] == '-') { fprintf(stderr, "usage: 9share pair [--force] NAME HOST\n"); return 2; }
        else if (!name) name = argv[i];
        else if (!host) host = argv[i];
        else { fprintf(stderr, "usage: 9share pair [--force] NAME HOST\n"); return 2; }
    }
    if (!name || (receive ? host != NULL : host == NULL)) {
        fprintf(stderr, "usage: 9share pair [--force] NAME HOST\n");
        return 2;
    }
    if (!nineshare_valid_name(name))
        return fail("'%s' cannot be the name of a share (letters, digits and . _ -)", name);
    if (receive)
        return pair_receive(name, force);
    if (!nineshare_valid_host(host))
        return fail("'%s' is not a host name as ssh knows it", host);
    if (p9_secret_path(NULL, name, path, sizeof path) < 0)
        return fail("the secret of '%s' has no usable path", name);
    if (!force && lstat(path, &st) == 0) {
        fprintf(stderr, "9share: %s already has a secret here (%s); pair --force replaces it on both machines\n", name, path);
        return 1;
    }
    if (getrandom(raw, sizeof raw, 0) != (ssize_t)sizeof raw || !lk_b64_encode(raw, sizeof raw, secret, sizeof secret))
        return fail("%s", "cannot get random bytes");
    memset(raw, 0, sizeof raw);

    /* the other machine first: if that fails nothing here has changed (and --force has not thrown away a working secret) */
    if (pipe(pfd) < 0)
        return fail("%s", "pipe failed");
    signal(SIGPIPE, SIG_IGN);
    if ((pid = fork()) < 0)
        return fail("%s", "fork failed");
    if (pid == 0) {
        dup2(pfd[0], 0);
        close(pfd[0]);
        close(pfd[1]);
        if (force)
            execlp("ssh", "ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, "9share", "pair", "--receive", "--force", name, (char *)NULL);
        execlp("ssh", "ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host, "9share", "pair", "--receive", name, (char *)NULL);
        _exit(127);
    }
    close(pfd[0]);
    if (write(pfd[1], secret, strlen(secret)) < 0 || write(pfd[1], "\n", 1) < 0) { /* ssh is gone: its status says why */ }
    close(pfd[1]);
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        memset(secret, 0, sizeof secret);
        if (code == 3)
            fprintf(stderr, "9share: %s already has a secret on %s; pair --force replaces it on both machines\n", name, host);
        else if (code == 255)
            fprintf(stderr, "9share: ssh could not reach %s (ssh needs to log in there without a password)\n", host);
        else if (code == 127)
            fprintf(stderr, "9share: ssh is not installed here\n");
        else
            fprintf(stderr, "9share: pair failed on %s (is 9share installed there, and is it this version?)\n", host);
        fprintf(stderr, "9share: nothing was changed here\n");
        return 1;
    }
    if (p9_secret_store(path, secret, 1) < 0) {
        memset(secret, 0, sizeof secret);
        fprintf(stderr, "9share: %s now has the secret, but it could not be written here (%s): run pair --force again\n", host, strerror(errno));
        return 1;
    }
    memset(secret, 0, sizeof secret);
    printf("%s is paired with %s: from now on each connection proves a shared secret (SCRAM-SHA-256) as well as trusting ssh.\n"
           "The export here and the mount there pick it up by themselves.\n", name, host);
    return 0;
}
