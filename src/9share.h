/* 9share.h -- what the parts of 9share share. */
#ifndef NINESHARE_H
#define NINESHARE_H

#define _GNU_SOURCE
#include <stddef.h>
#include <sys/types.h>
#include "9share_conf.h"

#ifndef NINESHARE_VERSION
#define NINESHARE_VERSION "0.1.0"
#endif
#ifndef NINESHARE_SYSCONFDIR
#define NINESHARE_SYSCONFDIR "/etc"
#endif
#ifndef NINESHARE_LIBEXECDIR
#define NINESHARE_LIBEXECDIR "/usr/lib/9share"
#endif
#ifndef NINESHARE_DAUD
#define NINESHARE_DAUD "/usr/sbin/daud"
#endif

#define P9_LINE 1024

/* util.c */
void  p9_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *p9_sysconf_dir(void);                 /* /etc/9share, or $NINESHARE_SYSCONFDIR (tests) */
const char *p9_daud(void);                        /* the daud program, or $NINESHARE_DAUD (tests) */
void  p9_user_conf_path(const char *home, char *buf, size_t n);   /* HOME's 9share.conf; NULL: the caller's */
int   p9_secret_path(const char *home, const char *name, char *buf, size_t n);
int   p9_secret_store(const char *path, const char *secret, int force);
int   p9_secret_load(const char *home, const char *name, char *secret, size_t n, char *path, size_t pn, char *why, size_t wn);
void  p9_system_conf_path(char *buf, size_t n);
void  p9_state_dir(char *buf, size_t n);
int   p9_mkdir_p(const char *path, mode_t mode);
char *p9_read_file(const char *path, size_t *len);               /* malloc'd, NUL-terminated; NULL if unreadable */
int   p9_write_file(const char *path, const char *data, size_t len, mode_t mode);   /* through a new name, then rename */
int   p9_file_exists(const char *path);
int   p9_lock(const char *path);                  /* fd holding the lock, or -1 when someone else has it */
int   p9_lock_held(const char *path);             /* never creates the file */
/* Run argv; its standard output (up to outsz-1 bytes) goes to OUT if given.  Returns the exit status,
 * 124 after TIMEOUT seconds (the child is killed), -1 if it cannot be run. */
int   p9_run(char *const argv[], int timeout, char *out, size_t outsz);
/* Edit the file's text: add a block at the end; remove the block of KEYWORD NAME that starts at or after LINE. */
int   p9_block_append(const char *path, const char *text);
int   p9_block_remove(const char *path, const char *keyword, const char *name, size_t line);
char *p9_self(void);                              /* the path of this program, malloc'd */

/* relay.c: the receiving end of a link (run by the root daemon and by ssh) */
int   p9_relay(int argc, char **argv);
int   p9_link(int argc, char **argv);

/* serve.c: one user's exports, in the foreground */
int   p9_serve(void);

/* boot.c: the root daemon, the mounts, and what status says about them */
int   p9_boot(int detach);
enum  { MS_NONE, MS_RW, MS_RO, MS_OTHER };
int   p9_mount_state(const char *at);
void  p9_mode_path(const char *home, const char *name, char *buf, size_t n);
const char *p9_mode_of(const char *home, const char *name);       /* "rw", "ro" or "off" */
void  p9_unmount(const char *at);
const char *p9_mount_state_name(int s);

#endif
