#!/bin/sh
# autogen.sh -- make configure and the Makefile.in files from configure.ac and the Makefile.am files (and daud's), after a
# checkout.  Then:  ./configure && make && make check && sudo make install
set -e
cd "$(dirname "$0")"
autoreconf --force --install
echo "Now run ./configure."
