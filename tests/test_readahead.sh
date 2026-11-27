#!/bin/sh
# test_readahead.sh -- `9share readahead`: what it writes, what it says, what it refuses.  NINESHARE_SYSCONFDIR points at a directory of ours (it also
# lifts the root check), so nothing of the machine is touched.
set -u
BIN=${NINESHARE_BIN:?}
T=$(mktemp -d /tmp/9share-readahead.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
export NINESHARE_SYSCONFDIR=$T
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
F=$T/prefetch-kb

out=$("$BIN" readahead 2>&1); rc=$?
check "with no file it says the default and writes nothing" '[ $rc -eq 0 ] && echo "$out" | grep -q "2048 KB (the default)" && [ ! -e "$F" ]'
out=$("$BIN" readahead off 2>&1); rc=$?
check "off writes 0 and says so" '[ $rc -eq 0 ] && [ "$(cat "$F")" = 0 ] && echo "$out" | grep -q "read-ahead is off"'
out=$("$BIN" readahead 4096 2>&1); rc=$?
check "a number is written as it is" '[ $rc -eq 0 ] && [ "$(cat "$F")" = 4096 ] && echo "$out" | grep -q "4096 KB"'
out=$("$BIN" readahead 2>&1)
check "with no argument it reports what is set, without changing it" 'echo "$out" | grep -q "4096 KB" && [ "$(cat "$F")" = 4096 ]'
out=$("$BIN" readahead on 2>&1); rc=$?
check "on is the default, which is no file" '[ $rc -eq 0 ] && [ ! -e "$F" ] && echo "$out" | grep -q "2048 KB (the default)"'
"$BIN" readahead on >/dev/null 2>&1; rc=$?
check "on again, with no file, is not an error" '[ $rc -eq 0 ]'
for bad in bogus -5 99999999 "" 12x; do
    echo 7 > "$F"
    "$BIN" readahead "$bad" >/dev/null 2>&1; rc=$?
    check "'$bad' is a usage error and leaves the file alone" '[ $rc -eq 2 ] && [ "$(cat "$F")" = 7 ]'
done
"$BIN" readahead 1 2 >/dev/null 2>&1; rc=$?
check "two arguments are a usage error" '[ $rc -eq 2 ]'
echo "garbage" > "$F"
out=$("$BIN" readahead 2>&1); rc=$?
check "a file that is not usable is reported, with a failing status" '[ $rc -ne 0 ] && echo "$out" | grep -q "not usable"'
check "help lists it" '"$BIN" help | grep -q "9share readahead"'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
