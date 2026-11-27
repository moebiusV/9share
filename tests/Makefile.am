AM_CFLAGS = -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function
AM_CPPFLAGS = -I$(top_srcdir)/src

check_PROGRAMS = test_conf test_9p test_link test_resume test_scram test_proxy test_daud_shutdown

test_conf_SOURCES = test_conf.c test.h ../src/conf.c ../src/util.c ../src/9share_conf.c ../src/hbnf_conf.c
test_9p_SOURCES = test_9p.c test.h
test_proxy_SOURCES = test_proxy.c test.h ../src/proxy.c ../src/proxy.h
test_daud_shutdown_SOURCES = test_daud_shutdown.c test.h p9client.h
test_resume_SOURCES = test_resume.c test.h p9client.h
test_link_SOURCES = test_link.c test.h ../daud/src/liblink/link.c ../daud/src/liblink/link.h ../daud/src/liblink/auth.c ../daud/src/liblink/auth.h ../daud/src/liblink/crypto.c ../daud/src/liblink/crypto.h
test_scram_SOURCES = test_scram.c test.h ../daud/src/liblink/auth.c ../daud/src/liblink/auth.h ../daud/src/liblink/crypto.c ../daud/src/liblink/crypto.h

TESTS = test_conf test_scram test_link test_proxy test_9p test_daud_shutdown test_resume test_resume_scram.sh test_serve.sh test_pair.sh test_init.sh test_setup.sh test_unit.sh test_shutdown.sh test_readahead.sh test_confperm.sh test_mountaccess.sh test_clean.sh
TESTS_ENVIRONMENT = NINESHARE_DAUD=$(abs_top_builddir)/daud/src/cmd/daud NINESHARE_BIN=$(abs_top_builddir)/src/9share \
                    NINESHARE_SRCDIR=$(abs_top_srcdir) NINESHARE_INIT=$(abs_top_srcdir)/tools/9share-init NINESHARE_SETUP=$(abs_top_srcdir)/tools/9share-setup \
                    NINESHARE_UNIT=$(abs_top_builddir)/systemd/9share.service

EXTRA_DIST = test_resume_scram.sh test_serve.sh test_pair.sh test_init.sh test_setup.sh test_unit.sh test_shutdown.sh test_readahead.sh test_confperm.sh test_clean.sh
