/* main.c -- 9share: share directories between machines over 9P, through ssh.  See 9share(8). */
#include "cmds.h"
#include "9share.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f)
{
    fputs("9share " NINESHARE_VERSION ": share directories between machines over 9P, through ssh\n"
          "\n"
          "  first time:\n"
          "    9share setup                     a few questions, then the simple, usual configuration\n"
          "\n"
          "  the machine that has the files (as yourself, no sudo):\n"
          "    9share export DIR HOST [NAME]    share DIR with HOST under NAME (default: the directory's name)\n"
          "    9share unexport NAME             stop sharing it\n"
          "    9share import HOST DIR [NAME]    the other way round: show HOST's DIR here (this machine's ssh dials out; needs the mount below)\n"
          "    9share unimport NAME             stop importing it\n"
          "    9share pair NAME HOST            make one secret for NAME and put it on both machines (SCRAM on every connection)\n"
          "\n"
          "  the machine that shows the files:\n"
          "    sudo 9share mount USER NAME DIR [--access owner|any]\n"
          "                                     show USER's share NAME at DIR (USER: the account here that the exporter logs in as; --access any: every user here may use it)\n"
          "    sudo 9share unmount NAME\n"
          "    9share rw|ro|off NAME            what the mount does now (as the user, no sudo)\n"
          "\n"
          "  either:\n"
          "    9share status                    what is shared, what is mounted, what is running\n"
          "    sudo 9share shutdown [--force]   stop the daemon (its mounts and exports end with it)\n"
          "    9share reload                    read the configuration again (yourself: your exports and imports; sudo: the mounts); the commands do it themselves, this is for a hand-edited file\n"
          "    sudo 9share restart              shutdown, then start it again\n"
          "    sudo 9share readahead [off|on|KB]  how far ahead reads are made, now (no restart); with no argument: what it is\n"
          "    sudo 9share wsl-boot [--remove]  start 9share at boot on WSL2 (no systemd there)\n"
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
    if (!strcmp(c, "import")) return cmd_import(argc, argv);
    if (!strcmp(c, "unimport")) return cmd_unimport(argc, argv);
    if (!strcmp(c, "mount")) return cmd_mount(argc, argv);
    if (!strcmp(c, "unmount")) return cmd_unmount(argc, argv);
    if (!strcmp(c, "rw") || !strcmp(c, "ro") || !strcmp(c, "off")) return cmd_mode(c, argc, argv);
    if (!strcmp(c, "setup")) {                        /* the questions are a shell script's: it runs 9share's other commands */
        char **av = calloc((size_t)argc + 2, sizeof *av);
        int i;
        if (!av) return 1;
        av[0] = "9share-setup";
        for (i = 0; i < argc; i++) av[i + 1] = argv[i];
        execvp(av[0], av);
        fprintf(stderr, "9share: cannot run 9share-setup: %s\n", strerror(errno));
        return 1;
    }
    if (!strcmp(c, "pair")) return cmd_pair(argc, argv);
    if (!strcmp(c, "status")) return cmd_status();
    if (!strcmp(c, "wsl-boot")) return cmd_wsl_boot(argc, argv);
    if (!strcmp(c, "shutdown")) return cmd_shutdown(argc, argv);
    if (!strcmp(c, "restart")) return cmd_restart(argc, argv);
    if (!strcmp(c, "reload")) return cmd_reload(argc, argv);
    if (!strcmp(c, "readahead")) return cmd_readahead(argc, argv);
    if (!strcmp(c, "serve")) return p9_serve();
    if (!strcmp(c, "relay")) return p9_relay(argc, argv);
    if (!strcmp(c, "link")) return p9_link(argc, argv);
    if (!strcmp(c, "offer")) return p9_offer(argc, argv);
    if (!strcmp(c, "carry")) return p9_carry(argc, argv);
    if (!strcmp(c, "boot")) return p9_boot(argc == 1 && !strcmp(argv[0], "--detach"));
    fprintf(stderr, "9share: unknown command '%s' (9share help)\n", c);
    return 2;
}
