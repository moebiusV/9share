AM_CFLAGS = -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-function
AM_CPPFLAGS = -I$(top_srcdir)/src

check_PROGRAMS = test_conf test_9p

test_conf_SOURCES = test_conf.c test.h ../src/conf.c ../src/util.c ../src/p9share_conf.c ../src/hbnf_conf.c
test_9p_SOURCES = test_9p.c test.h

TESTS = test_conf test_9p test_serve.sh test_clean.sh
TESTS_ENVIRONMENT = P9SHARE_DAUD=$(abs_top_builddir)/daud/src/cmd/daud P9SHARE_BIN=$(abs_top_builddir)/src/p9share \
                    P9SHARE_SRCDIR=$(abs_top_srcdir)

EXTRA_DIST = test_serve.sh test_clean.sh
