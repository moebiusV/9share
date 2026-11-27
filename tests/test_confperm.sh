#!/bin/sh
# test_confperm.sh -- a configuration file that others could change is not edited by 9share's commands, and `9share status` says it is not used.
# (The daemon and serve refusing such a file is in test_serve.sh and the unit test of p9_conf_trust in test_conf.c.)
set -u
BIN=${NINESHARE_BIN:?}
T=$(mktemp -d /tmp/9share-confperm.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/home/.config/9share" "$T/share"
chmod 755 "$T/home/.config/9share"
export HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_STATE_HOME=$T/state
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
CONF=$HOME/.config/9share/9share.conf

out=$("$BIN" export "$T/share" a.example.org one 2>&1); rc=$?
check "export works on a safe file" '[ $rc -eq 0 ] && grep -q "export \"one\"" "$CONF"'
before=$(cat "$CONF")

chmod g+w "$CONF"
out=$("$BIN" export "$T/share" b.example.org two 2>&1); rc=$?
check "export refuses a file the group can write, and says why" '[ $rc -ne 0 ] && echo "$out" | grep -q "nothing was changed" && echo "$out" | grep -q "writable by group or others"'
check "and leaves it as it was" '[ "$(cat "$CONF")" = "$before" ]'
out=$("$BIN" unexport one 2>&1); rc=$?
check "unexport refuses it too" '[ $rc -ne 0 ] && [ "$(cat "$CONF")" = "$before" ]'
out=$("$BIN" status 2>&1)
check "status warns that it is not used" 'echo "$out" | grep -q "warning: 9share serve does not use"'

chmod g-w "$CONF"
chmod o+w "$HOME/.config/9share"
out=$("$BIN" export "$T/share" b.example.org two 2>&1); rc=$?
check "a directory others can write is refused as well" '[ $rc -ne 0 ] && echo "$out" | grep -q "directory"'
check "and the file is unchanged" '[ "$(cat "$CONF")" = "$before" ]'
chmod o-w "$HOME/.config/9share"

out=$("$BIN" export "$T/share" b.example.org two 2>&1); rc=$?
check "with the permissions right again, export works" '[ $rc -eq 0 ] && grep -q "export \"two\"" "$CONF"'
out=$("$BIN" status 2>&1)
check "and status has no warning" '! echo "$out" | grep -q "^warning"'
chmod 600 "$CONF"
out=$("$BIN" unexport two 2>&1); rc=$?
check "a file only its owner can read and write is fine" '[ $rc -eq 0 ] && ! grep -q "export \"two\"" "$CONF"'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
