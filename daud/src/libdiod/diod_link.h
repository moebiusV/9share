/************************************************************\
 * Part of daud, a fork of the diod 9P server (see AUTHORS).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
\************************************************************/

#ifndef LIBDIOD_DIOD_LINK_H
#define LIBDIOD_DIOD_LINK_H

#include <signal.h>
#include "src/libnpfs/npfs.h"

/* Serve one 9P connection at a time over a resumable link (src/liblink), dialling out: the carrier is the command CMD run
 * by the shell with its standard input and output joined to the link (ssh HOST 9share link NAME).  The command is run again
 * when it dies, and the connection, with its fids, waits for it: only after HOLD seconds with no carrier, or when the other
 * end does not know the session, does the connection end and a new session begin.  Returns when *SHUTDOWN becomes true;
 * WAITMASK is the signal mask to wait under, so that the signal that sets it interrupts the wait. */
/* When set (daud --once) a session that ends, or never gets a carrier up within HOLD seconds, ends daud too, instead of a new
 * session beginning: for a daud that is started on demand (9share offer) and has no reason to outlive its importer. */
extern int diod_link_once;

void diod_link_run (Npsrv *srv, const char *cmd, int hold, const char *uname,
                    const char *aname, const char *secret,
                    volatile int *shutdown, const sigset_t *waitmask);

#endif
