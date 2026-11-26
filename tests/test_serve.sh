#!/bin/sh
# test_serve.sh -- `p9share serve` as the exporter does it, with a stand-in for ssh: the "other machine" is a directory.
# The stand-in runs the command that p9share sends to the host (with HOME there set to that directory) and, for ssh -R,
# records the forward and stays up until killed.  Checks: the host is prepared (~/.p9share, the .info file), the
# tunnel and daud start, a daud that dies is started again, an edited export is restarted, a removed one is stopped.
set -u
BIN=${P9SHARE_BIN:?}; DAUD=${P9SHARE_DAUD:?}
[ -x "$DAUD" ] || { echo "no daud to test"; exit 77; }
T=$(mktemp -d /tmp/p9share-serve.XXXXXX)
trap 'kill $SERVE 2>/dev/null; wait 2>/dev/null; rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/bin" "$T/home/.config/p9share" "$T/remote" "$T/share" "$T/state"
cat > "$T/bin/ssh" <<'EOS'
#!/bin/sh
# a stand-in for ssh: the arguments p9share uses are  -T [-N] -o OPT ... [-R FORWARD] HOST [COMMAND]
fwd=; while [ $# -gt 0 ]; do
    case $1 in -o) shift 2;; -R) fwd=$2; shift 2;; -T|-N) shift;; *) break;; esac
done
host=$1; shift
if [ -n "$fwd" ]; then echo "$host $fwd" >> "$FAKE_REMOTE/forwards"; exec sleep 600; fi
cd "$FAKE_REMOTE" && HOME=$FAKE_REMOTE exec sh -c "$1"
EOS
chmod +x "$T/bin/ssh"
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
waitfor() { i=0; while [ $i -lt 60 ]; do eval "$1" && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }

export HOME=$T/home XDG_CONFIG_HOME=$T/home/.config XDG_STATE_HOME=$T/state FAKE_REMOTE=$T/remote P9SHARE_DAUD=$DAUD PATH=$T/bin:$PATH
cat > "$HOME/.config/p9share/p9share.conf" <<EOC
# one share
export "one" {
	directory "$T/share"
	host "other.example.org"
}
EOC
"$BIN" serve 2> "$T/serve.log" &
SERVE=$!
check "the tunnel is started and the forward names the host's socket" 'waitfor "grep -q \"other.example.org $T/remote/.p9share/one.sock:\" \"$T/remote/forwards\" 2>/dev/null"'
check "the info file tells the mount the user and the directory" 'grep -q "^aname=$T/share\$" "$T/remote/.p9share/one.info" && grep -q "^uname=" "$T/remote/.p9share/one.info"'
check "daud is running on a private socket" 'waitfor "[ -S \"$T/state/p9share/one/daud.sock\" ]"'
check "the state directory is private" '[ "$(stat -c %a "$T/state/p9share/one")" = 700 ]'
check "a second serve for the same user is refused" '! "$BIN" serve 2>/dev/null'

pid=$(pgrep -f "daud -f -n -N -u .* -e $T/share" | head -1)
check "daud is a process of its own" '[ -n "$pid" ]'
[ -n "$pid" ] && kill -9 "$pid"
check "a daud that dies is started again (after a second)" 'waitfor "pgrep -f \"daud -f -n -N -u .* -e $T/share\" | grep -vx $pid >/dev/null"'

# a changed block is restarted, a new one started
cat >> "$HOME/.config/p9share/p9share.conf" <<EOC
export "two" {
	directory "$T/share"
	host "second.example.org"
}
EOC
check "a new export is started" 'waitfor "grep -q \"second.example.org\" \"$T/remote/forwards\" 2>/dev/null"'
check "its info file is written" '[ -f "$T/remote/.p9share/two.info" ]'

# a removed export is stopped: its daud goes
sed -i '/^export "two" {/,/^}/d' "$HOME/.config/p9share/p9share.conf"
check "a removed export is stopped" 'waitfor "[ ! -S \"$T/state/p9share/two/daud.sock\" ] || ! pgrep -f \"-l $T/state/p9share/two/daud.sock\" >/dev/null"'
check "the one that stays is still up" '[ -S "$T/state/p9share/one/daud.sock" ]'

kill -TERM $SERVE; wait $SERVE 2>/dev/null
check "serve ends on SIGTERM and takes daud and ssh with it" '! pgrep -f "daud -f -n -N -u .* -e $T/share" >/dev/null'
echo "$n checks, $fail failed"
[ $fail -eq 0 ]
