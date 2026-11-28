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
#define NINESHARE_LIBEXECDIR "/usr/libexec/9share"
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
const char *p9_run_dir(void);                     /* /run, or $NINESHARE_RUNDIR (tests) */
int   p9_proc_alive(int pid);                     /* running (not gone, not a zombie) */
int   p9_serve_pid(void);                        /* this user's `9share serve`, or 0 */
int   p9_reload_serve(void);                     /* SIGHUP to it: read the configuration again; 1 if there was one */
int   p9_reload_daemon(void);                    /* the same for the root daemon */
int   p9_daemon_pid(void);                        /* the pid of the running `9share boot`, or 0 */
/* The optional files in the system directory that tune the mount.  mount-options: extra options for the kernel's 9p mount, such as cache=loose
 * (none of 9share's own can be set this way); OPTS gets them.  Returns 0, or -1 when the file was there and was refused (it is then as if it
 * were not): the caller says so.  prefetch-kb: how far ahead the relay reads for a file read in order, in KB (default 2048, 0 = not at all);
 * KB gets it, and -1 is returned when the file was refused (the default is used). */
int   p9_mount_options(char *opts, size_t n);
int   p9_prefetch_kb(int *kb);
/* May a configuration file be acted on?  A file says what is shared and who may use it, so it is trusted only if it could not have been changed by anyone
 * else: a regular file (a symbolic link is followed, and what it points at is what is checked), owned by OWNER or by root, not writable by group or others,
 * in a directory that is owned by OWNER or root and not writable by group or others.  Returns 0 (usable), 1 (there is no such file) or -1 (refused: WHY
 * says in one line what is wrong and what to do). */
int   p9_conf_trust(const char *path, unsigned owner, char *why, size_t wn);
/* The options of the kernel's 9p mount.  ACCESS_UID is the account on THIS machine that may use the mount: access=UID makes the kernel refuse every other
 * user, root included (diod's own manual calls this its single-user mode); ANY_USER (a mount block's `access any`) makes it access=any, which lets every
 * local user in, with the exporting user's rights. */
void  p9_kernel_mount_opts(char *buf, size_t n, int kfd, const char *uname, const char *aname, unsigned msize, unsigned access_uid, int any_user);
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
int   p9_offer(int argc, char **argv);
int   p9_carry(int argc, char **argv);
int   p9_link_connect(const char *name);

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
