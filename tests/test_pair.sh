#!/bin/sh
# test_pair.sh -- `9share pair NAME HOST`: one secret made here, put on the other machine through ssh (a stand-in for ssh runs the
# command there under another HOME), then here.  Both files are private and equal; the secret is never on a command line; an existing
# secret is not replaced without --force; when the other machine cannot be reached nothing here changes.
set -u
BIN=${NINESHARE_BIN:?}
T=$(mktemp -d /tmp/9share-pair.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/bin" "$T/local" "$T/remote"
ln -s "$BIN" "$T/bin/9share"
cat > "$T/bin/ssh" <<'EOS'
#!/bin/sh
# a stand-in for ssh: record what it was asked, then run the command here as if on the other machine
while [ $# -gt 0 ]; do
    case $1 in -o) shift 2;; -T|-N) shift;; *) break;; esac
done
host=$1; shift
echo "$host $*" >> "$FAKE_LOG"
[ -n "${FAKE_FAIL:-}" ] && exit "$FAKE_FAIL"
exec env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$@"
EOS
chmod +x "$T/bin/ssh"
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }

unset XDG_CONFIG_HOME
export HOME=$T/local FAKE_REMOTE=$T/remote FAKE_LOG=$T/ssh.log PATH=$T/bin:$PATH
L=$HOME/.config/9share/proj.secret
R=$FAKE_REMOTE/.config/9share/proj.secret

out=$("$BIN" pair proj other.example.org 2>&1); rc=$?
check "pair succeeds" '[ $rc -eq 0 ]'
check "and says what it did" 'echo "$out" | grep -q "proj is paired with other.example.org"'
check "the secret is here, private" '[ "$(stat -c %a "$L")" = 600 ]'
check "and there, private" '[ "$(stat -c %a "$R")" = 600 ]'
check "both are the same secret" 'cmp -s "$L" "$R"'
check "it is one line of at least 40 characters" '[ "$(wc -l < "$L")" = 1 ] && [ "$(wc -c < "$L")" -ge 41 ]'
check "ssh was run for the host, with 9share pair --receive and the name" 'grep -qx "other.example.org 9share pair --receive proj" "$T/ssh.log"'
check "the secret is not on any command line" '! grep -qF "$(cat "$L")" "$T/ssh.log"'
check "the directory holding it is private" '[ "$(stat -c %a "$HOME/.config/9share")" = 700 ]'
check "no temporary file is left" '[ -z "$(ls "$HOME/.config/9share" | grep new)" ] && [ -z "$(ls "$FAKE_REMOTE/.config/9share" | grep new)" ]'

first=$(cat "$L")
out=$("$BIN" pair proj other.example.org 2>&1); rc=$?
check "pairing again is refused" '[ $rc -ne 0 ] && echo "$out" | grep -q "already has a secret here"'
check "and nothing changed" '[ "$(cat "$L")" = "$first" ] && cmp -s "$L" "$R"'

out=$("$BIN" pair --force proj other.example.org 2>&1); rc=$?
check "--force replaces it" '[ $rc -eq 0 ] && [ "$(cat "$L")" != "$first" ]'
check "on both machines alike" 'cmp -s "$L" "$R"'
check "the replacement is private too" '[ "$(stat -c %a "$L")" = 600 ] && [ "$(stat -c %a "$R")" = 600 ]'

# the other machine already has one, this one does not: refused there, nothing made here
rm -f "$L"
before=$(cat "$R")
out=$("$BIN" pair proj other.example.org 2>&1); rc=$?
check "a secret that exists only there is not replaced without --force" '[ $rc -ne 0 ] && echo "$out" | grep -q "already has a secret on other.example.org"'
check "and none was made here" '[ ! -e "$L" ] && [ "$(cat "$R")" = "$before" ]'
rm -f "$R"

# the other machine cannot be reached: nothing here changes, with or without an old secret
FAKE_FAIL=255 "$BIN" pair fresh other.example.org > "$T/o" 2>&1; rc=$?
check "an unreachable host fails" '[ $rc -ne 0 ] && grep -q "could not reach other.example.org" "$T/o"'
check "and no secret was made" '[ ! -e "$HOME/.config/9share/fresh.secret" ]'
( umask 077; echo "keep this old secret" > "$L" )
FAKE_FAIL=255 "$BIN" pair --force proj other.example.org > "$T/o" 2>&1; rc=$?
check "--force with an unreachable host fails" '[ $rc -ne 0 ]'
check "and keeps the working secret" '[ "$(cat "$L")" = "keep this old secret" ]'
rm -f "$L"

# --receive, as ssh runs it
r=$(printf 'tooshort\n' | env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$BIN" pair --receive proj 2>&1); rc=$?
check "--receive refuses what is not a secret" '[ $rc -ne 0 ] && [ ! -e "$R" ]'
r=$(printf 'bad secret with spaces and more than sixteen\n' | env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$BIN" pair --receive proj 2>&1); rc=$?
check "--receive refuses odd characters" '[ $rc -ne 0 ] && [ ! -e "$R" ]'
printf 'QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n' | env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$BIN" pair --receive proj; rc=$?
check "--receive stores a good one" '[ $rc -eq 0 ] && [ "$(stat -c %a "$R")" = 600 ]'
printf 'QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVo=\n' | env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$BIN" pair --receive proj 2>/dev/null; rc=$?
check "and exits 3 when there is one already" '[ $rc -eq 3 ]'

# usage and names
"$BIN" pair 2>/dev/null; rc=$?; check "no arguments is a usage error" '[ $rc -eq 2 ]'
"$BIN" pair 'bad name' host 2>/dev/null; rc=$?; check "a bad name is refused" '[ $rc -ne 0 ]'
"$BIN" pair proj 'bad host' 2>/dev/null; rc=$?; check "a bad host is refused" '[ $rc -ne 0 ]'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
