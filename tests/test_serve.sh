#!/bin/sh
# test_serve.sh -- `9share serve` as the exporter does it, with a stand-in for ssh that only records what it was asked to run:
# one daud is started for each export, with ssh to the right host and `9share link` of the right name as its carrier; a daud that
# dies is started again; a block added to the file is started, one removed is stopped; SIGTERM ends everything.
set -u
BIN=${NINESHARE_BIN:?}; DAUD=${NINESHARE_DAUD:?}
[ -x "$DAUD" ] || { echo "no daud to test"; exit 77; }
T=$(mktemp -d /tmp/9share-serve.XXXXXX)
trap 'kill $SERVE 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/bin" "$T/home/.config/9share" "$T/share" "$T/state"
cat > "$T/bin/ssh" <<'EOS'
#!/bin/sh
# a stand-in for ssh: record the host and the command that follows it, then stay up until killed
while [ $# -gt 0 ]; do
    case $1 in -o) shift 2;; -T|-N) shift;; *) break;; esac
done
echo "$*" >> "$FAKE_LOG"
exec sleep 601.37
EOS
chmod +x "$T/bin/ssh"
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
waitfor() { i=0; while [ $i -lt 60 ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }

export HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_STATE_HOME=$T/state FAKE_LOG=$T/ssh.log NINESHARE_DAUD=$DAUD PATH=$T/bin:$PATH
cat > "$HOME/.config/9share/9share.conf" <<EOC
# one share
export "one" {
	directory "$T/share"
	host "other.example.org"
	hold 120
}
EOC
"$BIN" serve 2> "$T/serve.log" &
SERVE=$!
check "ssh is run to the host, for \`9share link\` of the export's name" 'waitfor "grep -qx \"other.example.org 9share link one\" \"$T/ssh.log\" 2>/dev/null"'
check "daud runs for it, dialling, with the hold time" 'pgrep -f -- "--dial .* other.example.org 9share link one --hold 120" >/dev/null'
check "daud serves only that directory, as this user" 'pgrep -f -- "-u $(id -u) -e $T/share --dial" >/dev/null'
check "the state directory is private" '[ "$(stat -c %a "$T/state/9share/one")" = 700 ]'
check "a second serve for the same user is refused" '! "$BIN" serve 2>/dev/null'

pid=$(pgrep -f -- "--dial .* other.example.org 9share link one" | head -1)
check "daud is a process of its own" '[ -n "$pid" ]'
[ -n "$pid" ] && kill -9 "$pid"
check "a daud that dies is started again (after a second)" 'waitfor "pgrep -f -- \"--dial .* other.example.org 9share link one\" | grep -vx $pid >/dev/null"'

# a block added to the file is started
cat >> "$HOME/.config/9share/9share.conf" <<EOC
export "two" {
	directory "$T/share"
	host "second.example.org"
}
EOC
check "a new export is started" 'waitfor "grep -qx \"second.example.org 9share link two\" \"$T/ssh.log\" 2>/dev/null"'

# a block removed from the file is stopped; the other stays
sed -i '/^export "two" {/,/^}/d' "$HOME/.config/9share/9share.conf"
check "a removed export is stopped" 'waitfor "! pgrep -f -- \"second.example.org 9share link two\" >/dev/null"'
check "the one that stays is still up" 'pgrep -f -- "other.example.org 9share link one" >/dev/null'

# a secret file turns on SCRAM: daud is started again with it; an unusable one stops the export; fixing or removing it restarts it
SEC=$HOME/.config/9share/one.secret
check "without a secret file daud is given no secret" '! pgrep -f -- "9share link one --hold 120 --link-secret-file" >/dev/null'
( umask 077; echo "correct horse" > "$SEC" )
check "a secret file restarts the export with --link-secret-file" 'waitfor "pgrep -f -- \"9share link one --hold 120 --link-secret-file $SEC\" >/dev/null"'
chmod 644 "$SEC"
check "a secret others can read stops the export" 'waitfor "! pgrep -f -- \"other.example.org 9share link one\" >/dev/null"'
check "and the reason is reported" 'waitfor "grep -q \"one: not started: .*must not be readable\" \"$T/serve.log\""'
chmod 600 "$SEC"
check "making it private starts the export again, at once" 'waitfor "pgrep -f -- \"9share link one --hold 120 --link-secret-file $SEC\" >/dev/null"'
: > "$SEC"; chmod 600 "$SEC"
check "an empty secret stops the export" 'waitfor "! pgrep -f -- \"other.example.org 9share link one\" >/dev/null"'
rm -f "$SEC"
check "removing the file starts it again without a secret" 'waitfor "pgrep -f -- \"9share link one --hold 120 -L stderr\" >/dev/null"'

# a block with a mistake is reported and left out
cat >> "$HOME/.config/9share/9share.conf" <<EOC
export "bad" {
	directory "/srv/with space"
	host "third.example.org"
}
EOC
sleep 3
check "a block with a mistake is not started" '! pgrep -f -- "third.example.org" >/dev/null'
check "and the mistake is reported with its place" 'grep -q "9share.conf:[0-9]*: " "$T/serve.log"'

kill -TERM $SERVE; wait $SERVE 2>/dev/null
# Only this test's own processes: its hosts are *.example.org and its stand-in for ssh sleeps 601.37 s.  (Matching any "--dial ... 9share link" or
# "sleep 600" made the test fail on a machine that was really exporting a directory.)
check "serve ends on SIGTERM and takes daud and ssh with it" '! pgrep -f -- "--dial .* [a-z]*.example.org 9share link" >/dev/null && ! pgrep -f "sleep 601.37" >/dev/null'
echo "$n checks, $fail failed"
[ $fail -eq 0 ]
