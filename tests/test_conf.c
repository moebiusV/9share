/* test_conf.c -- the parser of 9share.conf, its checks, and the editing of the file's text. */
#include "../src/9share.h"
#include "test.h"
#include <stdlib.h>
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

    DONE();
}
