#!/bin/sh
# test_clean.sh -- nothing that names the machine this runs on, its users or its paths is in the files of the package.
# What to look for is found out here, at run time, so that this file names nothing itself: the host name, the local
# users' names (lower case: the author's name as author and copyright holder is not a user name) and $HOME; and, in the
# package's own files (not upstream daud's, which carry their authors' own notices), home paths and mail addresses.
# Skipped outside a git checkout.
SRC=${P9SHARE_SRCDIR:-$(dirname "$0")/..}
cd "$SRC" || exit 77
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 77
pats=$(
    { hostname; hostname -s; id -un; getent passwd 2>/dev/null | awk -F: '$3 >= 1000 && $3 < 65534 { print $1 }'; echo "$HOME"; } 2>/dev/null |
    awk 'length($0) >= 4' | sort -u
)
files=$(git ls-files | grep -v -E '^tests/test_clean\.sh$')
bad=0
for p in $pats; do
    # the maintainer line of debian/control and the trailer of a debian/changelog entry are the packager's published identity
    hits=$(echo "$files" | xargs grep -n -I -F -e "$p" 2>/dev/null | grep -v -E '^debian/control:[0-9]+:Maintainer: |^debian/changelog:[0-9]+: -- ')
    if [ -n "$hits" ]; then echo "this machine's '$p' is in:"; echo "$hits" | head -5; bad=1; fi
done
own=$(echo "$files" | grep -v '^daud/')
hits=$(echo "$own" | xargs grep -n -I -E '/home/[a-z]|/Users/|[A-Za-z0-9._-]+@(gmail|outlook|yahoo|hotmail)\.' 2>/dev/null)
if [ -n "$hits" ]; then echo "a home path or a mail address in the package's own files:"; echo "$hits" | head -5; bad=1; fi
exit $bad
