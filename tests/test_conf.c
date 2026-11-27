/* test_conf.c -- the parser of 9share.conf, its checks, and the editing of the file's text. */
#include "../src/9share.h"
#include "test.h"
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

static char dir[256];

static void put(const char *name, const char *text, char *path)
{
    snprintf(path, 512, "%s/%s", dir, name);
    CHECK(p9_write_file(path, text, strlen(text), 0644) == 0);
}

int main(void)
{
    struct nineshare_conf c;
    char path[512], *text;
    snprintf(dir, sizeof dir, "/tmp/9share-test-conf-%d", (int)getpid());
    CHECK(p9_mkdir_p(dir, 0700) == 0);

    /* a good file: both kinds of block, barewords and strings, comments anywhere */
    put("good.conf",
        "# my shares\n"
        "export \"projects\" {\n"
        "\tdirectory /srv/projects   # the tree\n"
        "\thost bob.example.org\n"
        "}\n"
        "\n"
        "export music { host \"carol@example.org\"\n directory \"/srv/music\" }\n"
        "mount \"docs\" {\n"
        "\tuser alice\n"
        "\tat /mnt/docs\n"
        "}\n", path);
    nineshare_conf_init(&c);
    CHECK(nineshare_conf_parse(path, &c) == 0);
    CHECK(c.nexports == 2 && c.nmounts == 1);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "projects") && !strcmp(c.exports[0].directory, "/srv/projects"));
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].host, "bob.example.org") && c.exports[0].line == 2);
    CHECK(c.nexports == 2 && !strcmp(c.exports[1].name, "music") && !strcmp(c.exports[1].host, "carol@example.org"));
    CHECK(c.nmounts == 1 && !strcmp(c.mounts[0].user, "alice") && !strcmp(c.mounts[0].at, "/mnt/docs"));
    nineshare_conf_free(&c);

    /* bad blocks are reported and dropped; the good ones around them stay */
    put("bad.conf",
        "export a { directory /srv/a host h1 }\n"
        "export b { directory \"/srv/has space\" host h2 }\n"        /* a path with a space */
        "export c { directory /srv/c }\n"                            /* no host */
        "export d { directory /srv/d host h4 host h5 }\n"            /* a setting twice */
        "export a { directory /srv/a2 host h6 }\n"                   /* a name twice */
        "mount m { user Alice at /mnt/m }\n"                         /* not a login name */
        "export e { directory /srv/e host h7 }\n", path);
    nineshare_conf_init(&c);
    fprintf(stderr, "(the errors that follow are expected)\n");
    CHECK(nineshare_conf_parse(path, &c) == -1);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "a") && !strcmp(c.exports[1].name, "e"));
    CHECK(c.nmounts == 0);
    nineshare_conf_free(&c);

    /* a syntax error */
    put("syntax.conf", "export a { directory /srv/a host h1 }\nfrobnicate x\n", path);
    nineshare_conf_init(&c);
    CHECK(nineshare_conf_parse(path, &c) == -1);
    nineshare_conf_free(&c);

    /* the file is gone */
    snprintf(path, sizeof path, "%s/none.conf", dir);
    nineshare_conf_init(&c);
    CHECK(nineshare_conf_parse(path, &c) == -1);
    nineshare_conf_free(&c);

    /* editing keeps what the author wrote: comments, other blocks, the layout */
    put("edit.conf",
        "# first\n"
        "export \"one\" {\n\tdirectory /srv/one\n\thost h1 # \"quoted } brace\"\n}\n"
        "\n"
        "# second\n"
        "export \"two\" {\n\tdirectory /srv/two\n\thost h2\n}\n", path);
    CHECK(p9_block_remove(path, "export", "one", 2) == 0);
    text = p9_read_file(path, NULL);
    CHECK(text && !strstr(text, "one") && strstr(text, "# first") && strstr(text, "# second") && strstr(text, "\"two\""));
    free(text);
    CHECK(p9_block_remove(path, "export", "one", 1) == -1);          /* it is gone */
    CHECK(p9_block_append(path, "export \"three\" {\n\tdirectory \"/srv/three\"\n\thost \"h3\"\n}\n") == 0);
    nineshare_conf_init(&c);
    CHECK(nineshare_conf_parse(path, &c) == 0 && c.nexports == 2);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "two") && !strcmp(c.exports[1].name, "three"));
    CHECK(c.nexports == 2 && p9_block_remove(path, "export", "three", c.exports[1].line) == 0);
    nineshare_conf_free(&c);
    text = p9_read_file(path, NULL);
    CHECK(text && !strstr(text, "three") && strstr(text, "\"two\""));
    free(text);
    /* the line a block is said to start on need not be exact: the block is found from there */
    CHECK(p9_block_remove(path, "export", "two", 1) == 0);
    text = p9_read_file(path, NULL);
    CHECK(text && !strstr(text, "two") && strstr(text, "# second"));
    free(text);

    /* what may go where */
    CHECK(nineshare_valid_name("projects") && nineshare_valid_name("a.b_c-d") && !nineshare_valid_name("-a") && !nineshare_valid_name("a b")
          && !nineshare_valid_name("") && !nineshare_valid_name("a/b"));
    CHECK(nineshare_valid_user("alice") && nineshare_valid_user("_x") && !nineshare_valid_user("Alice") && !nineshare_valid_user("a b"));
    CHECK(nineshare_valid_host("bob.example.org") && nineshare_valid_host("me@box") && !nineshare_valid_host("-oProxyCommand=x")
          && !nineshare_valid_host("a b") && !nineshare_valid_host(""));
    CHECK(nineshare_valid_path("/srv/a-b_c.d+e@f") && !nineshare_valid_path("rel") && !nineshare_valid_path("/a,b") && !nineshare_valid_path("/a b")
          && !nineshare_valid_path("/a\"b") && !nineshare_valid_path("/a\nb"));

    /* the mode the user chose: rw unless the file says ro or off */
    {
        char home[300], mp[600];
        snprintf(home, sizeof home, "%s/home", dir);
        CHECK(!strcmp(p9_mode_of(home, "n"), "rw"));
        p9_mode_path(home, "n", mp, sizeof mp);
        CHECK(p9_write_file(mp, "ro\n", 3, 0644) == 0 && !strcmp(p9_mode_of(home, "n"), "ro"));
        CHECK(p9_write_file(mp, "off\n", 4, 0644) == 0 && !strcmp(p9_mode_of(home, "n"), "off"));
        CHECK(p9_write_file(mp, "garbage\n", 8, 0644) == 0 && !strcmp(p9_mode_of(home, "n"), "rw"));
    }

    {
        char cmd[400];
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "could not remove %s\n", dir);
    }

    /* the optional files that tune the mount and the relay (root-owned, in the system directory) */
    {
        char opts[256];
        int kb = -1;
        setenv("NINESHARE_SYSCONFDIR", dir, 1);
        unlink(strcat(strcpy(path, dir), "/mount-options"));
        unlink(strcat(strcpy(path, dir), "/prefetch-kb"));
        CHECK(p9_mount_options(opts, sizeof opts) == 0 && opts[0] == '\0');                              /* no file: nothing to add */
        put("mount-options", "cache=loose\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) == 0 && !strcmp(opts, "cache=loose"));
        put("mount-options", "  cache=loose,debug=1  \nsecond line is ignored\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) == 0 && !strcmp(opts, "cache=loose,debug=1"));
        put("mount-options", "cache=loose; rm -rf /\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) < 0 && opts[0] == '\0');                               /* characters that are not options */
        put("mount-options", "cache=loose,aname=/\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) < 0 && opts[0] == '\0');                               /* what 9share sets itself is not overridden */
        put("mount-options", "msize=65536\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) < 0 && opts[0] == '\0');
        put("mount-options", ",cache=loose\n", path);
        CHECK(p9_mount_options(opts, sizeof opts) < 0);                                                  /* an empty option */

        CHECK(p9_prefetch_kb(&kb) == 0 && kb == 2048);                                                   /* no file: the default */
        put("prefetch-kb", "4096\n", path);
        CHECK(p9_prefetch_kb(&kb) == 0 && kb == 4096);
        put("prefetch-kb", " 0 \n", path);
        CHECK(p9_prefetch_kb(&kb) == 0 && kb == 0);                                                      /* off */
        put("prefetch-kb", "abc\n", path);
        CHECK(p9_prefetch_kb(&kb) < 0 && kb == 2048);                                                    /* refused: the default */
        put("prefetch-kb", "-5\n", path);
        CHECK(p9_prefetch_kb(&kb) < 0 && kb == 2048);
        put("prefetch-kb", "99999999\n", path);
        CHECK(p9_prefetch_kb(&kb) < 0 && kb == 2048);
        unsetenv("NINESHARE_SYSCONFDIR");
    }

    /* may a configuration file be acted on?  Every rule, with files of our own (we are not root, and so are the owners) */
    {
        char why[512], f[600], d2[600], lnk[600], cmd[900];
        unsigned me = (unsigned)getuid();
        snprintf(f, sizeof f, "%s/trust.conf", dir);
        CHECK(p9_conf_trust(f, me, why, sizeof why) == 1);                                    /* no file: nothing to act on */
        put("trust.conf", "# nothing\n", path);
        CHECK(chmod(dir, 0755) == 0 && chmod(f, 0644) == 0);
        CHECK(p9_conf_trust(f, me, why, sizeof why) == 0);                                    /* the owner's, others may read it */
        CHECK(chmod(f, 0600) == 0 && p9_conf_trust(f, me, why, sizeof why) == 0);
        CHECK(chmod(f, 0664) == 0 && p9_conf_trust(f, me, why, sizeof why) < 0 && strstr(why, "writable by group or others"));
        CHECK(chmod(f, 0646) == 0 && p9_conf_trust(f, me, why, sizeof why) < 0);
        CHECK(chmod(f, 0666) == 0 && p9_conf_trust(f, me, why, sizeof why) < 0 && strstr(why, "chmod go-w"));
        CHECK(chmod(f, 0644) == 0 && chmod(dir, 0775) == 0 && p9_conf_trust(f, me, why, sizeof why) < 0 && strstr(why, "directory"));   /* it could be replaced */
        CHECK(chmod(dir, 0757) == 0 && p9_conf_trust(f, me, why, sizeof why) < 0);
        CHECK(chmod(dir, 0755) == 0 && p9_conf_trust(f, me, why, sizeof why) == 0);
        if (getuid() != 0) {
            CHECK(p9_conf_trust(f, 0, why, sizeof why) < 0 && strstr(why, "must be owned by root"));       /* the system file is root's alone */
            CHECK(p9_conf_trust(f, me + 1, why, sizeof why) < 0 && strstr(why, "must be owned by its user or root"));
        }
        snprintf(d2, sizeof d2, "%s/sub", dir);
        CHECK(mkdir(d2, 0755) == 0 || errno == EEXIST);
        CHECK(p9_conf_trust(d2, me, why, sizeof why) < 0 && strstr(why, "not a regular file"));
        snprintf(lnk, sizeof lnk, "%s/link.conf", dir);
        unlink(lnk);
        CHECK(symlink(f, lnk) == 0 && p9_conf_trust(lnk, me, why, sizeof why) == 0);          /* a link to a good file in a good directory */
        CHECK(chmod(f, 0666) == 0 && p9_conf_trust(lnk, me, why, sizeof why) < 0);            /* what it points at is what counts */
        CHECK(chmod(f, 0644) == 0);
        snprintf(cmd, sizeof cmd, "%s/elsewhere", dir);
        CHECK(mkdir(cmd, 0777) == 0 || errno == EEXIST);
        CHECK(chmod(cmd, 0777) == 0);
        snprintf(cmd, sizeof cmd, "%s/elsewhere/real.conf", dir);
        { FILE *x = fopen(cmd, "w"); if (x) { fputs("# x\n", x); fclose(x); } }
        CHECK(chmod(cmd, 0644) == 0);
        unlink(lnk);
        CHECK(symlink(cmd, lnk) == 0 && p9_conf_trust(lnk, me, why, sizeof why) < 0 && strstr(why, "directory"));   /* a good file in a directory that others can write */
        unlink(lnk);
    }

    /* the options of the kernel mount: only the named local account may use it */
    {
        char o[512];
        p9_kernel_mount_opts(o, sizeof o, 7, "exporter", "/srv/share", 131072, 1234, 0);
        CHECK(strstr(o, "access=1234,") != NULL);
        CHECK(strstr(o, "access=any") == NULL && strstr(o, "access=client") == NULL && strstr(o, "access=user") == NULL);
        CHECK(strstr(o, "trans=fd,rfdno=7,wfdno=7,version=9p2000.L,uname=exporter,") == o);
        CHECK(strstr(o, ",aname=/srv/share,msize=131072") != NULL);
        p9_kernel_mount_opts(o, sizeof o, 7, "exporter", "/srv/share", 131072, 1234, 1);        /* `access any` in the mount block: every local user, on purpose */
        CHECK(strstr(o, "access=any,") != NULL && strstr(o, "access=1234") == NULL);
    }

    /* the access setting of a mount block */
    {
        struct nineshare_conf ac;
        char p[600];
        nineshare_conf_init(&ac);
        put("access.conf",
            "mount a {\n user bob\n at /mnt/a\n}\n"
            "mount b {\n user bob\n at /mnt/b\n access owner\n}\n"
            "mount c {\n user bob\n at /mnt/c\n access any\n}\n"
            "mount d {\n user bob\n at /mnt/d\n access all\n}\n"
            "mount e {\n user bob\n at /mnt/e\n access any\n access owner\n}\n"
            "mount f {\n user bob\n access any\n at /mnt/f\n hold 30\n}\n", p);
        nineshare_conf_parse(p, &ac);
        CHECK(ac.nmounts == 4);                                    /* d (not user or any) and e (twice) are dropped, with a message */
        CHECK(ac.nmounts == 4 && !strcmp(ac.mounts[0].name, "a") && ac.mounts[0].access == 0);        /* not given: only the user, the default */
        CHECK(ac.nmounts == 4 && !strcmp(ac.mounts[1].name, "b") && ac.mounts[1].access == 1);
        CHECK(ac.nmounts == 4 && !strcmp(ac.mounts[2].name, "c") && ac.mounts[2].access == 2);
        CHECK(ac.nmounts == 4 && !strcmp(ac.mounts[3].name, "f") && ac.mounts[3].access == 2 && ac.mounts[3].hold == 30);   /* in any order, with the others */
        nineshare_conf_free(&ac);
    }

    DONE();
}
