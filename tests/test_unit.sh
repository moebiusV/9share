#!/bin/sh
# test_unit.sh -- the systemd unit that is installed must be one systemd accepts: an absolute ExecStart, nothing left unexpanded.
# (It once shipped with ExecStart=${exec_prefix}/bin/9share, and the service never started.)
U=${NINESHARE_UNIT:?}
[ -f "$U" ] || { echo "SKIP: no unit built (configured without systemd?)"; exit 77; }
fail=0
x=$(sed -n 's/^ExecStart=//p' "$U")
case $x in /*) ;; *) echo "FAILED: ExecStart is not an absolute path: $x"; fail=1;; esac
if grep -v -E '^ExecReload=/bin/kill -HUP \$MAINPID$' "$U" | grep -n -E '[$@]'; then echo "FAILED: unexpanded text in the unit"; fail=1; fi
exit $fail
