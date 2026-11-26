/* cmds.c -- the commands a person types: export, unexport, mount, unmount, rw|ro|off, status, wsl-boot. */
#include "cmds.h"
#include "p9share.h"
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PB (2 * P9_LINE + 64)

static int fail(const char *fmt, const char *a)
{
    fprintf(stderr, "p9share: ");
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

int cmd_export(int argc, char **argv)
{
    char confp[PB], dir[P9_LINE], text[3 * P9_LINE], user[128];
    const char *host, *name;
    struct p9share_conf c;
    struct stat st;
    size_t i;
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: p9share export DIR HOST [NAME]\n");
        return 2;
    }
    if (!realpath(argv[0], dir) || stat(dir, &st) < 0 || !S_ISDIR(st.st_mode))
        return fail("%s is not a directory", argv[0]);
    host = argv[1];
    if (argc == 3)
        name = argv[2];
    else {
        name = strrchr(dir, '/') + 1;
        if (!*name)
            return fail("give a name for %s", dir);
    }
    if (!p9share_valid_name(name))
        return fail("'%s' cannot be the name of an export (letters, digits and . _ -)", name);
    if (!p9share_valid_host(host))
        return fail("'%s' is not a host name as ssh knows it", host);
    if (!p9share_valid_path(dir))
        return fail("%s: the directory needs an absolute path of letters, digits and . _ + @ - / (no spaces)", dir);
    p9_user_conf_path(NULL, confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    for (i = 0; i < c.nexports; i++)
        if (!strcmp(c.exports[i].name, name)) {
            if (p9_block_remove(confp, "export", name, c.exports[i].line) < 0)
                return fail("cannot replace the export '%s' in the configuration", name);
            break;
        }
    p9share_conf_free(&c);
    snprintf(text, sizeof text, "export \"%s\" {\n\tdirectory \"%s\"\n\thost \"%s\"\n}\n", name, dir, host);
    if (p9_block_append(confp, text) < 0)
        return fail("cannot write %s", confp);
    printf("sharing %s with %s as '%s'\n", dir, host, name);
    ssh_user(host, user, sizeof user);
    printf("on %s:   sudo p9share mount %s %s /mnt/%s\n", host, user, name, name);
    {
        char state[P9_LINE], lockp[PB];
        p9_state_dir(state, sizeof state);
        snprintf(lockp, sizeof lockp, "%s/serve.lock", state);
        if (!p9_lock_held(lockp))
            printf("(nothing runs your exports yet: the p9share service does it at boot; now, run  sudo systemctl start p9share  or  p9share serve)\n");
    }
    return 0;
}

int cmd_unexport(int argc, char **argv)
{
    char confp[PB];
    struct p9share_conf c;
    size_t i;
    int rc = 1;
    if (argc != 1) {
        fprintf(stderr, "usage: p9share unexport NAME\n");
        return 2;
    }
    p9_user_conf_path(NULL, confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    for (i = 0; i < c.nexports; i++)
        if (!strcmp(c.exports[i].name, argv[0])) {
            if (p9_block_remove(confp, "export", argv[0], c.exports[i].line) == 0) {
                printf("no longer sharing '%s'\n", argv[0]);
                rc = 0;
            } else
                fail("cannot change %s", confp);
            break;
        }
    if (i == c.nexports)
        fail("no export named '%s'", argv[0]);
    p9share_conf_free(&c);
    return rc;
}

int cmd_mount(int argc, char **argv)
{
    char confp[PB], text[3 * P9_LINE], at[P9_LINE];
    struct p9share_conf c;
    size_t i;
    if (geteuid() != 0)
        return fail("%s needs root: run it with sudo", "mount");
    if (argc != 3) {
        fprintf(stderr, "usage: sudo p9share mount USER NAME DIR\n");
        return 2;
    }
    if (!p9share_valid_user(argv[0]) || !getpwnam(argv[0]))
        return fail("'%s' is not a user of this machine", argv[0]);
    if (!p9share_valid_name(argv[1]))
        return fail("'%s' cannot be the name of a mount (letters, digits and . _ -)", argv[1]);
    if (argv[2][0] == '/')
        snprintf(at, sizeof at, "%s", argv[2]);
    else if (!getcwd(at, sizeof at) || strlen(at) + strlen(argv[2]) + 2 > sizeof at)
        return fail("%s: cannot make the path absolute", argv[2]);
    else
        strcat(strcat(at, "/"), argv[2]);
    if (!p9share_valid_path(at))
        return fail("%s: the mount point needs an absolute path of letters, digits and . _ + @ - / (no spaces)", at);
    p9_system_conf_path(confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[1])) {
            if (p9_block_remove(confp, "mount", argv[1], c.mounts[i].line) < 0)
                return fail("cannot replace the mount '%s' in the configuration", argv[1]);
            break;
        }
    p9share_conf_free(&c);
    snprintf(text, sizeof text, "mount \"%s\" {\n\tuser \"%s\"\n\tat \"%s\"\n}\n", argv[1], argv[0], at);
    if (p9_block_append(confp, text) < 0)
        return fail("cannot write %s", confp);
    p9_mkdir_p(at, 0755);
    printf("'%s' of %s will be shown at %s as soon as the exporter's tunnel is up\n", argv[1], argv[0], at);
    if (!p9_lock_held("/run/p9share.lock"))
        printf("(the daemon is not running: it starts at boot; now, run  sudo systemctl start p9share)\n");
    return 0;
}

int cmd_unmount(int argc, char **argv)
{
    char confp[PB];
    struct p9share_conf c;
    size_t i;
    int rc = 1;
    if (geteuid() != 0)
        return fail("%s needs root: run it with sudo", "unmount");
    if (argc != 1) {
        fprintf(stderr, "usage: sudo p9share unmount NAME\n");
        return 2;
    }
    p9_system_conf_path(confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[0])) {
            p9_unmount(c.mounts[i].at);
            if (p9_block_remove(confp, "mount", argv[0], c.mounts[i].line) == 0) {
                printf("unmounted %s\n", c.mounts[i].at);
                rc = 0;
            } else
                fail("cannot change %s", confp);
            break;
        }
    if (i == c.nmounts)
        fail("no mount named '%s'", argv[0]);
    p9share_conf_free(&c);
    return rc;
}

int cmd_mode(const char *mode, int argc, char **argv)
{
    char confp[PB], path[PB], text[16];
    struct p9share_conf c;
    struct passwd *pw = getpwuid(getuid());
    size_t i;
    int found = 0;
    if (argc != 1 || !pw) {
        fprintf(stderr, "usage: p9share %s NAME\n", mode);
        return 2;
    }
    p9_system_conf_path(confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    for (i = 0; i < c.nmounts; i++)
        if (!strcmp(c.mounts[i].name, argv[0]) && !strcmp(c.mounts[i].user, pw->pw_name))
            found = 1;
    p9share_conf_free(&c);
    if (!found)
        return fail("no mount named '%s' for you here (see: p9share status)", argv[0]);
    p9_mode_path(pw->pw_dir, argv[0], path, sizeof path);
    snprintf(text, sizeof text, "%s\n", mode);
    if (p9_write_file(path, text, strlen(text), 0644) < 0)
        return fail("cannot write %s", path);
    printf("%s: %s (the mount follows within a few seconds)\n", argv[0], mode);
    return 0;
}

int cmd_status(void)
{
    char confp[PB], state[P9_LINE], sockp[PB];
    struct p9share_conf c;
    size_t i;
    int daemon = p9_lock_held("/run/p9share.lock");
    printf("daemon:  %s\n", daemon ? "running" : "not running (sudo systemctl start p9share; on WSL2: sudo p9share wsl-boot)");
    p9_user_conf_path(NULL, confp, sizeof confp);
    p9_state_dir(state, sizeof state);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    if (c.nexports)
        printf("shared by you:\n");
    else
        printf("shared by you: nothing  (p9share export DIR HOST)\n");
    for (i = 0; i < c.nexports; i++) {
        snprintf(sockp, sizeof sockp, "%s/%s/daud.sock", state, c.exports[i].name);
        printf("  %-14s %s -> %s   %s\n", c.exports[i].name, c.exports[i].directory, c.exports[i].host,
               p9_file_exists(sockp) ? "running" : "starting or waiting");
    }
    p9share_conf_free(&c);
    p9_system_conf_path(confp, sizeof confp);
    p9share_conf_init(&c);
    if (p9_file_exists(confp))
        p9share_conf_parse(confp, &c);
    if (c.nmounts)
        printf("mounted here:\n");
    else
        printf("mounted here: nothing  (sudo p9share mount USER NAME DIR)\n");
    for (i = 0; i < c.nmounts; i++) {
        struct passwd *pw = getpwnam(c.mounts[i].user);
        char sock[PB];
        const char *want = "?";
        int up = 0;
        if (pw) {
            want = p9_mode_of(pw->pw_dir, c.mounts[i].name);
            snprintf(sock, sizeof sock, "%s/.p9share/%s.sock", pw->pw_dir, c.mounts[i].name);
            up = p9_file_exists(sock);
        }
        printf("  %-14s %s  from %s   wanted: %s   now: %s   tunnel: %s\n", c.mounts[i].name, c.mounts[i].at, c.mounts[i].user, want,
               p9_mount_state_name(p9_mount_state(c.mounts[i].at)), up ? "up" : "down");
    }
    p9share_conf_free(&c);
    return 0;
}

/* WSL2 has no systemd: the [boot] command of /etc/wsl.conf starts the daemon.  A command that is there is kept and runs first. */
int cmd_wsl_boot(void)
{
    const char *path = "/etc/wsl.conf";
    char *text = p9_read_file(path, NULL), *out, *line, *save = NULL, *self, mine[P9_LINE];
    int in_boot = 0, done = 0, have_boot = 0;
    size_t cap;
    if (geteuid() != 0)
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
            if (strstr(old, "p9share")) {
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
        if (in_boot && !done && !strncasecmp(t, "[boot]", 6)) {
            snprintf(out + strlen(out), cap - strlen(out), "command = %s\n", mine);
            done = 1;
        }
    }
    if (!have_boot)
        snprintf(out + strlen(out), cap - strlen(out), "\n[boot]\ncommand = %s\n", mine);
    if (p9_write_file(path, out, strlen(out), 0644) < 0)
        return fail("cannot write %s", path);
    printf("/etc/wsl.conf now starts p9share at boot.  Restart WSL once (wsl.exe --shutdown, from Windows), or start it now: sudo p9share boot --detach\n");
    free(text);
    free(out);
    return 0;
}
