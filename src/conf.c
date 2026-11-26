/* conf.c -- the checks that p9share.conf's blocks go through, and the list they are added to. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "p9share_conf.h"

static int alnum(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

int p9share_valid_name(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 64 || !alnum((unsigned char)s[0]))
        return 0;
    for (i = 1; i < n; i++)
        if (!alnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-')
            return 0;
    return 1;
}

int p9share_valid_user(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 32 || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_'))
        return 0;
    for (i = 1; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-' || s[i] == '.'))
            return 0;
    return 1;
}

int p9share_valid_host(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 255 || !alnum((unsigned char)s[0]))
        return 0;
    for (i = 1; i < n; i++)
        if (!alnum((unsigned char)s[i]) && !strchr("._@:-", s[i]))
            return 0;
    return 1;
}

int p9share_valid_path(const char *s)
{
    size_t i, n = strlen(s);
    if (n == 0 || n > 1024 || s[0] != '/')
        return 0;
    for (i = 1; i < n; i++)
        if (!alnum((unsigned char)s[i]) && !strchr("._+@-/", s[i]))
            return 0;
    return 1;
}

static int grow(void **base, size_t *n, size_t size)
{
    void *p = realloc(*base, (*n + 1) * size);
    if (p == NULL)
        return -1;
    *base = p;
    return 0;
}

const char *p9share_conf_add_export(struct p9share_conf *c, const struct p9share_export *e)
{
    size_t i;
    if (!p9share_valid_name(e->name))
        return "the name of an export is letters, digits and . _ - (not starting with . _ -)";
    if (e->directory == NULL || e->host == NULL)
        return "an export needs a directory and a host";
    if (!p9share_valid_path(e->directory))
        return "the directory must be an absolute path of letters, digits and . _ + @ - / (no spaces)";
    if (!p9share_valid_host(e->host))
        return "the host must be a name as ssh knows it (letters, digits and . _ @ : -)";
    for (i = 0; i < c->nexports; i++)
        if (strcmp(c->exports[i].name, e->name) == 0)
            return "there is already an export with this name";
    if (grow((void **)&c->exports, &c->nexports, sizeof *c->exports) < 0)
        return "out of memory";
    c->exports[c->nexports++] = *e;
    return NULL;
}

const char *p9share_conf_add_mount(struct p9share_conf *c, const struct p9share_mount *m)
{
    size_t i;
    if (!p9share_valid_name(m->name))
        return "the name of a mount is letters, digits and . _ - (not starting with . _ -)";
    if (m->user == NULL || m->at == NULL)
        return "a mount needs a user and an at";
    if (!p9share_valid_user(m->user))
        return "the user must be a login name";
    if (!p9share_valid_path(m->at))
        return "the mount point must be an absolute path of letters, digits and . _ + @ - / (no spaces)";
    for (i = 0; i < c->nmounts; i++) {
        if (strcmp(c->mounts[i].name, m->name) == 0)
            return "there is already a mount with this name";
        if (strcmp(c->mounts[i].at, m->at) == 0)
            return "there is already a mount at this place";
    }
    if (grow((void **)&c->mounts, &c->nmounts, sizeof *c->mounts) < 0)
        return "out of memory";
    c->mounts[c->nmounts++] = *m;
    return NULL;
}
