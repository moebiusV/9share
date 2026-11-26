dnl configure.ac -- p9share.  Copyright (c) 2026 David Walther.  GPL-2.0-or-later.
AC_PREREQ([2.69])
AC_INIT([p9share],[0.1.0])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AM_SILENT_RULES([yes])
AC_PROG_CC
AC_PROG_INSTALL
AC_CHECK_HEADERS([sys/queue.h sys/mount.h],[],[AC_MSG_ERROR([a header needed to build is missing])])
AC_CHECK_FUNCS([clearenv initgroups umount2],[],[AC_MSG_ERROR([a Linux C library is needed])])

AC_ARG_WITH([systemdsystemunitdir],
  [AS_HELP_STRING([--with-systemdsystemunitdir=DIR],[where to install p9share.service (default: PREFIX/lib/systemd/system; "no" to skip)])],
  [],[with_systemdsystemunitdir='${prefix}/lib/systemd/system'])
AC_SUBST([systemdsystemunitdir],[$with_systemdsystemunitdir])
AM_CONDITIONAL([INSTALL_UNIT],[test "x$with_systemdsystemunitdir" != xno])

dnl daud, the 9P server (a fork of diod), is built with what p9share needs and nothing else: no munge, no lua config file,
dnl no multi-user mode (daud runs as the one user who exports).
ac_configure_args="$ac_configure_args --disable-auth --disable-config --disable-multiuser --disable-diodmount"
AC_CONFIG_SUBDIRS([daud])

AC_CONFIG_FILES([Makefile src/Makefile man/Makefile tests/Makefile systemd/p9share.service])
AC_OUTPUT
