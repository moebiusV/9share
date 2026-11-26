---
title: 9share
description: Share directories between machines over 9P, through ssh. One command on each side.
---

# 9share

**Share directories between machines over 9P, through ssh.** 9share makes a directory on one machine appear at a mount point on another. The machine with the files dials out, so the other needs **no open port** and no server to configure, and a connection that drops is picked up again where it stopped. Install the same package on both machines; nothing happens on a machine until something is configured on it.

[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](https://github.com/moebiusV/9share/blob/main/COPYING)
![Linux](https://img.shields.io/badge/Debian%20%7C%20Ubuntu%20%7C%20WSL2-orange)

## Fast path

You need 9share on both machines, and ssh from the first to the second without a password (`ssh HOST true`).

On the machine that has the files, as yourself:

```
9share export /srv/projects bob.example.org
```

On `bob.example.org`, once, as root (the command above prints this line, ready to copy):

```
sudo 9share mount alice projects /mnt/projects
```

`/srv/projects` now appears at `/mnt/projects`, and stays there. `alice` is the account on `bob.example.org` that ssh logs in as, and `projects` is the share's name, by default the directory's own. To be asked instead of typing these, run `9share setup` on either machine: three questions, the same result.

## The rest, all optional

```
9share ro projects        # read-only (as that account, no sudo)
9share rw projects        # read-write
9share off projects       # not mounted
9share status             # what is shared, what is mounted, what is running
9share pair projects bob.example.org   # a secret on both machines, proved on every connection
9share unexport projects  # stop sharing
```

Several directories, several machines, and both directions at once are each one more block of configuration.

### The configuration

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

### Install

```
sudo apt install ./9share_*.deb          # or: ./configure && make && sudo make install
```

The package finds out how the machine starts services and starts the daemon at boot that way: systemd, WSL2 (with or without systemd), sysvinit, OpenRC. Where it cannot (a container, runit) it says what to run. It starts with nothing shared, a commented template in `/etc/9share/9share.conf`, and a message to run `9share setup`.

Requirements: Linux with the 9p module, openssh-client, and key-based ssh from the machine with the files to the one that shows them.

### How it works

1. `9share serve` runs **daud**, a 9P2000.L file server, as you, unprivileged. daud is a fork of [diod](https://github.com/chaos/diod) that lives in this package (it needs no munge, no lua and no root).
2. daud runs **ssh** to the other machine, where `9share link` joins it to a relay run as the mount's user. The 9P connection rides a **resumable link** over that: each end keeps the bytes the other has not acknowledged, so when ssh drops and is run again they resend exactly what is missing, and the connection, with its open files, never notices. ssh runs with keepalives, so a dead connection is noticed in about 45 seconds.
3. The root daemon on the other machine mounts it with the kernel's 9p client, `nosuid,nodev`, and unmounts it only when the session is given up (the `hold` time, 600 seconds unless set).

### Security

The export is the directory you name, with your rights. The mount is `nosuid,nodev`, and any local user on the receiving machine who can reach the mount point sees the files as you: do not export a directory to a machine whose users you do not trust. The relay takes a connection only from a process of the mount's user and, once you have run `9share pair`, only one that proves the shared secret (SCRAM-SHA-256). The secret does not encrypt anything; ssh does. Every value that travels from the exporter into a mount option is checked first.

## Documentation

`9share(8)`, `9share-setup(1)`, `9share-init(8)`, `9share.conf(5)` and `daud(8)` ship with the package; [README](https://github.com/moebiusV/9share/blob/main/README), [INSTALL](https://github.com/moebiusV/9share/blob/main/INSTALL) and [HACKING](https://github.com/moebiusV/9share/blob/main/HACKING) are in the repository.

## License

9share is GPL-2.0-or-later. daud is diod's: GPL-2, with the 9P library (libnpfs) under an MIT-like licence; see `COPYING` and `daud/COPYING`.
