/* cmds.h -- the commands of cmds.c; each takes the arguments after its name and returns the exit status. */
#ifndef NINESHARE_CMDS_H
#define NINESHARE_CMDS_H
int cmd_export(int argc, char **argv);
int cmd_unexport(int argc, char **argv);
int cmd_mount(int argc, char **argv);
int cmd_unmount(int argc, char **argv);
int cmd_mode(const char *mode, int argc, char **argv);
int cmd_pair(int argc, char **argv);
int cmd_status(void);
int cmd_wsl_boot(int argc, char **argv);
#endif
