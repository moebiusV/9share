/* test_conf.c -- the parser of p9share.conf, its checks, and the editing of the file's text. */
#include "../src/p9share.h"
#include "test.h"
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
    struct p9share_conf c;
    char path[512], *text;
    snprintf(dir, sizeof dir, "/tmp/p9share-test-conf-%d", (int)getpid());
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
    p9share_conf_init(&c);
    CHECK(p9share_conf_parse(path, &c) == 0);
    CHECK(c.nexports == 2 && c.nmounts == 1);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "projects") && !strcmp(c.exports[0].directory, "/srv/projects"));
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].host, "bob.example.org") && c.exports[0].line == 2);
    CHECK(c.nexports == 2 && !strcmp(c.exports[1].name, "music") && !strcmp(c.exports[1].host, "carol@example.org"));
    CHECK(c.nmounts == 1 && !strcmp(c.mounts[0].user, "alice") && !strcmp(c.mounts[0].at, "/mnt/docs"));
    p9share_conf_free(&c);

    /* bad blocks are reported and dropped; the good ones around them stay */
    put("bad.conf",
        "export a { directory /srv/a host h1 }\n"
        "export b { directory \"/srv/has space\" host h2 }\n"        /* a path with a space */
        "export c { directory /srv/c }\n"                            /* no host */
        "export d { directory /srv/d host h4 host h5 }\n"            /* a setting twice */
        "export a { directory /srv/a2 host h6 }\n"                   /* a name twice */
        "mount m { user Alice at /mnt/m }\n"                         /* not a login name */
        "export e { directory /srv/e host h7 }\n", path);
    p9share_conf_init(&c);
    fprintf(stderr, "(the errors that follow are expected)\n");
    CHECK(p9share_conf_parse(path, &c) == -1);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "a") && !strcmp(c.exports[1].name, "e"));
    CHECK(c.nmounts == 0);
    p9share_conf_free(&c);

    /* a syntax error */
    put("syntax.conf", "export a { directory /srv/a host h1 }\nfrobnicate x\n", path);
    p9share_conf_init(&c);
    CHECK(p9share_conf_parse(path, &c) == -1);
    p9share_conf_free(&c);

    /* the file is gone */
    snprintf(path, sizeof path, "%s/none.conf", dir);
    p9share_conf_init(&c);
    CHECK(p9share_conf_parse(path, &c) == -1);
    p9share_conf_free(&c);

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
    p9share_conf_init(&c);
    CHECK(p9share_conf_parse(path, &c) == 0 && c.nexports == 2);
    CHECK(c.nexports == 2 && !strcmp(c.exports[0].name, "two") && !strcmp(c.exports[1].name, "three"));
    CHECK(c.nexports == 2 && p9_block_remove(path, "export", "three", c.exports[1].line) == 0);
    p9share_conf_free(&c);
    text = p9_read_file(path, NULL);
    CHECK(text && !strstr(text, "three") && strstr(text, "\"two\""));
    free(text);
    /* the line a block is said to start on need not be exact: the block is found from there */
    CHECK(p9_block_remove(path, "export", "two", 1) == 0);
    text = p9_read_file(path, NULL);
    CHECK(text && !strstr(text, "two") && strstr(text, "# second"));
    free(text);

    /* what may go where */
    CHECK(p9share_valid_name("claude") && p9share_valid_name("a.b_c-d") && !p9share_valid_name("-a") && !p9share_valid_name("a b")
          && !p9share_valid_name("") && !p9share_valid_name("a/b"));
    CHECK(p9share_valid_user("alice") && p9share_valid_user("_x") && !p9share_valid_user("Alice") && !p9share_valid_user("a b"));
    CHECK(p9share_valid_host("bob.example.org") && p9share_valid_host("me@box") && !p9share_valid_host("-oProxyCommand=x")
          && !p9share_valid_host("a b") && !p9share_valid_host(""));
    CHECK(p9share_valid_path("/srv/a-b_c.d+e@f") && !p9share_valid_path("rel") && !p9share_valid_path("/a,b") && !p9share_valid_path("/a b")
          && !p9share_valid_path("/a\"b") && !p9share_valid_path("/a\nb"));

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
    DONE();
}
