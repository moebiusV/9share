/* main.c -- 9share: share directories between machines over 9P, through ssh.  See 9share(8). */
#include "cmds.h"
#include "9share.h"
#include <stdio.h>
#include <string.h>

static void usage(FILE *f)
{
    fputs("9share " NINESHARE_VERSION ": share directories between machines over 9P, through ssh\n"
          "\n"
          "  the machine that has the files (as yourself, no sudo):\n"
          "    9share export DIR HOST [NAME]    share DIR with HOST under NAME (default: the directory's name)\n"
          "    9share unexport NAME             stop sharing it\n"
          "\n"
          "  the machine that shows the files:\n"
          "    sudo 9share mount USER NAME DIR  show USER's share NAME at DIR (USER: the account here that the exporter logs in as)\n"
          "    sudo 9share unmount NAME\n"
          "    9share rw|ro|off NAME            what the mount does now (as the user, no sudo)\n"
          "\n"
          "  either:\n"
          "    9share status                    what is shared, what is mounted, what is running\n"
          "    sudo 9share wsl-boot             start 9share at boot on WSL2 (no systemd there)\n"
          "\n"
          "  behind these: `9share boot [--detach]` (the root daemon, run by the 9share service) and\n"
          "  `9share serve` (one user's exports, in the foreground).  See 9share(8).\n", f);
}

int main(int argc, char **argv)
{
    const char *c;
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage(argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    c = argv[1];
    argc -= 2;
    argv += 2;
    if (!strcmp(c, "--version") || !strcmp(c, "version")) { puts("9share " NINESHARE_VERSION); return 0; }
    if (!strcmp(c, "export")) return cmd_export(argc, argv);
    if (!strcmp(c, "unexport")) return cmd_unexport(argc, argv);
    if (!strcmp(c, "mount")) return cmd_mount(argc, argv);
    if (!strcmp(c, "unmount")) return cmd_unmount(argc, argv);
    if (!strcmp(c, "rw") || !strcmp(c, "ro") || !strcmp(c, "off")) return cmd_mode(c, argc, argv);
    if (!strcmp(c, "status")) return cmd_status();
    if (!strcmp(c, "wsl-boot")) return cmd_wsl_boot();
    if (!strcmp(c, "serve")) return p9_serve();
    if (!strcmp(c, "relay")) return p9_relay(argc, argv);
    if (!strcmp(c, "link")) return p9_link(argc, argv);
    if (!strcmp(c, "boot")) return p9_boot(argc == 1 && !strcmp(argv[0], "--detach"));
    fprintf(stderr, "9share: unknown command '%s' (9share help)\n", c);
    return 2;
}
