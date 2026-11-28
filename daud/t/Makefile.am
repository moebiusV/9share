AM_TESTS_ENVIRONMENT =

TEST_EXTENSIONS = .t
T_LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) \
	$(top_srcdir)/config/tap-driver.sh

clean-local:
	rm -fr trash-directory.* test-results .prove *.output *.diod.log

# This list is included in both TESTS and dist_check_SCRIPTS.
TESTSCRIPTS = \
	t0000-sharness.t \
	t1000-valgrind.t \
	t0001-basic-runasuser.t \
	t0002-basic-allsquash.t \
	t0003-basic-multiuser.t \
	t0004-npclient-basic.t \
	t0005-npclient-xattr.t \
	t0008-npclient-vsock.t \
	t0006-npclient-issues.t \
	t0007-configfile.t \
	t0010-v9fs-runasuser.t \
	t0011-v9fs-allsquash.t \
	t0012-v9fs-multiuser.t \
	t0013-v9fs-acl.t \
	t0014-v9fs-mount8.t \
	t0020-dbench.t \
	t0021-postmark.t \
	t0022-scrub.t \
	t0023-rsync.t \
	t0024-selfhost.t \
	t0025-pathwalk.t \
	t0026-munge-auth.t \
	t0028-syspackage.t

# Upstream diod's own suite is shipped but NOT run by make check: it drives the diodcli client, the ctl file system and the multi-user
# and munge modes, none of which daud builds (configure passes --disable-auth --disable-config --disable-multiuser --disable-diodmount),
# so most of it fails or skips here.  9share's own tests (tests/) are the suite that decides.  Run it by hand with: make -C daud/t check TESTS="$(TESTSCRIPTS)"
TESTS =

dist_check_SCRIPTS = \
	$(TESTSCRIPTS)

EXTRA_DIST = \
	sharness.sh \
	aggregate-results.sh \
	sharness.d \
	etc


AM_CFLAGS = @WARNING_CFLAGS@

AM_CPPFLAGS = \
	-I$(top_srcdir)

common_ldadd =

check_PROGRAMS =
