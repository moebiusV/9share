#!/bin/sh
# test_mountaccess.sh -- `9share mount ... --access owner|any`: what it writes, and that the config it writes is read back as it was meant.
# NINESHARE_SYSCONFDIR points at a directory of ours (it also lifts the root check and makes this user the owner the file must have).
set -u
BIN=${NINESHARE_BIN:?}
T=$(mktemp -d /tmp/9share-mountaccess.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/etc"; chmod 755 "$T/etc"
export NINESHARE_SYSCONFDIR=$T/etc
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
ME=$(id -un); CONF=$T/etc/9share.conf

"$BIN" mount "$ME" plain "$T/m1" >/dev/null 2>&1; rc=$?
check "mount without --access writes no access line (the default: only the owner)" '[ $rc -eq 0 ] && grep -q "mount \"plain\"" "$CONF" && ! grep -q "^[[:space:]]*access " "$CONF"'
"$BIN" mount "$ME" open "$T/m2" --access any >/dev/null 2>&1; rc=$?
check "mount --access any writes it" '[ $rc -eq 0 ] && grep -q "^[[:space:]]*access any" "$CONF"'
"$BIN" mount "$ME" mine "$T/m3" --access owner >/dev/null 2>&1; rc=$?
check "mount --access owner is accepted" '[ $rc -eq 0 ] && [ "$(grep -c "^[[:space:]]*access " "$CONF")" = 2 ]'
"$BIN" mount "$ME" bad "$T/m4" --access everyone >/dev/null 2>&1; rc=$?
check "any other value is a usage error and writes nothing" '[ $rc -eq 2 ] && ! grep -q "mount \"bad\"" "$CONF"'
"$BIN" mount "$ME" bad "$T/m4" --acces any >/dev/null 2>&1; rc=$?
check "a misspelt flag is a usage error" '[ $rc -eq 2 ]'
out=$("$BIN" status 2>&1)
check "the file that was written is read back without complaint" '! echo "$out" | grep -q -i "error\|not used"'
chmod 666 "$CONF"
"$BIN" mount "$ME" third "$T/m5" --access any >/dev/null 2>&1; rc=$?
check "and a config the others can write is not edited, whatever it says" '[ $rc -ne 0 ] && ! grep -q "mount \"third\"" "$CONF"'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
