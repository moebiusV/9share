---
title: 9share
description: Share directories between machines over 9P, through ssh. One command on each side.
---

# 9share

**Share directories between machines over 9P, through ssh.** One command on the machine that has the files, one on the machine that shows them.

[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](https://github.com/moebiusV/9share/blob/main/COPYING)
![Linux](https://img.shields.io/badge/Debian%20%7C%20Ubuntu%20%7C%20WSL2-orange)

## In two commands

On the machine that has the files, as yourself:

```
9share export /srv/projects bob.example.org
```

On the machine that should show them, once:

```
sudo 9share mount alice projects /mnt/projects
```

That is all. The machine with the files **dials out** to the other with ssh, so the machine that shows them needs **no open port** and no server for you to configure. A daemon that starts at boot keeps the tunnel up (starting it again when it drops) and keeps the mount right: mounted while the tunnel is there, unmounted when it is not.

The user on the receiving machine switches the mount without sudo:

```
9share ro projects        # read-only
9share rw projects        # read-write
9share off projects       # not mounted
9share status             # what is shared, what is mounted, what is running
```

Several directories, several machines, and both directions at once are each one more block of configuration. **Nothing happens on a machine until something is configured on it.**

## The configuration

Two small files, in [obconf](https://github.com/moebiusV/hbnf/blob/main/obconf.md), the notation of the OpenBSD daemons' configuration files. The commands above write them; you may edit them by hand, and comments you write stay.

```
# ~/.config/9share/9share.conf, on the machine that has the files
export "projects" {
    directory /srv/projects
    host bob.example.org
}
```

```
# /etc/9share/9share.conf, on the machine that shows them
mount "projects" {
    user alice
    at /mnt/projects
}
```

The parser is generated from [`grammars/9share.hbnf`](https://github.com/moebiusV/9share/blob/main/grammars/9share.hbnf) by [hbnf](https://github.com/moebiusV/hbnf) and committed, so building needs no hbnf.

## How it works

1. `9share serve` runs **daud**, a 9P2000.L file server, as you, unprivileged, on a private unix socket. daud is a fork of [diod](https://github.com/chaos/diod) that lives in this package (it needs no munge, no lua and no root).
2. `ssh -R` hands that socket to the other machine, into the user's `~/.9share/`, and the exporter writes there the user name and directory the mount asks for. ssh runs with keepalives, so a dead link is noticed in about 45 seconds, and both daud and ssh are started again whenever they die.
3. The root daemon on the other machine mounts it with the kernel's 9p client, `nosuid,nodev`, only from a socket that belongs to the configured user and answers, and unmounts it when the tunnel goes away.

## Install

```
sudo apt install ./9share_*.deb          # or: ./configure && make && sudo make install
sudo systemctl enable --now 9share       # the package does this for you
```

On WSL2, which has no systemd, `sudo 9share wsl-boot` puts the daemon in `/etc/wsl.conf`.

Requirements: Linux with the 9p module, openssh-client, and key-based ssh from the machine with the files to the one that shows them (`ssh HOST true` must work without a password).

## Security

The export is the directory you name, with your rights. The mount is `nosuid,nodev`, and any local user on the receiving machine who can reach the mount point sees the files as you: do not export a directory to a machine whose users you do not trust. Every value that travels from the exporter's files into a mount option is checked first.

## Documentation

`9share(8)`, `9share.conf(5)` and `daud(8)` ship with the package; [README](https://github.com/moebiusV/9share/blob/main/README), [INSTALL](https://github.com/moebiusV/9share/blob/main/INSTALL) and [HACKING](https://github.com/moebiusV/9share/blob/main/HACKING) are in the repository.

## License

9share is GPL-2.0-or-later. daud is diod's: GPL-2, with the 9P library (libnpfs) under an MIT-like licence; see `COPYING` and `daud/COPYING`.
