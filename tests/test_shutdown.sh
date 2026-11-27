#!/bin/sh
# test_shutdown.sh -- `9share shutdown` and `9share restart`, and what `9share status` says about the daemon.
# No root and no real daemon: NINESHARE_RUNDIR points at a directory of ours (it is also what turns off the root check and the systemd path),
# and the daemon is a stand-in script named 9share that is run as "9share boot".
set -u
BIN=${NINESHARE_BIN:?}
T=$(mktemp -d /tmp/9share-shutdown.XXXXXX)
trap 'kill $(cat "$T"/pids 2>/dev/null) 2>/dev/null; rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/run" "$T/home" "$T/etc"
: > "$T/pids"
export NINESHARE_RUNDIR=$T/run HOME=$T/home NINESHARE_SYSCONFDIR=$T/etc NINESHARE_SHUTDOWN_WAIT=3
unset XDG_CONFIG_HOME XDG_STATE_HOME

fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
alive() { [ -d "/proc/$1" ] && ! grep -q '^State:.Z' "/proc/$1/status" 2>/dev/null; }
# a stand-in is a daemon only once it has exec'd the script: until then its command line is still the test shell's
ready() { alive "$1" && tr '\0' ' ' < "/proc/$1/cmdline" 2>/dev/null | grep -q '9share boot'; }
waitfor() { i=0; while [ $i -lt 40 ]; do eval "$1" && return 0; sleep 0.1; i=$((i + 1)); done; return 1; }

# a stand-in daemon: run as "<dir>/9share boot", leaves on SIGTERM unless told to ignore it
cat > "$T/9share" <<'EOS'
#!/bin/sh
[ "${1:-}" = boot ] || exit 2
[ -n "${DEAF:-}" ] && trap '' TERM || trap 'exit 0' TERM
while :; do sleep 0.2; done
EOS
chmod +x "$T/9share"
start_daemon() {   # start_daemon [deaf]: its pid in $D, and in the pid file
    # no inherited stdin/stdout: a stand-in that keeps the caller's pipe open would hold up whatever reads the test's output
    if [ "${1:-}" = deaf ]; then DEAF=1 "$T/9share" boot </dev/null >/dev/null 2>&1 & else "$T/9share" boot </dev/null >/dev/null 2>&1 & fi
    D=$!; echo $D >> "$T/pids"; echo $D > "$T/run/9share.pid"
}

# --- nothing running
out=$("$BIN" shutdown 2>&1); rc=$?
check "shutdown with no daemon says so and succeeds" '[ $rc -eq 0 ] && echo "$out" | grep -qi "not running"'
out=$("$BIN" status 2>&1)
check "status says not running" 'echo "$out" | grep -q "^daemon:  not running"'

# --- a running daemon
start_daemon
waitfor 'ready $D'
out=$("$BIN" status 2>&1)
check "status, run by a plain user, sees the daemon through its pid file" 'echo "$out" | grep -q "^daemon:  running"'
out=$("$BIN" shutdown 2>&1); rc=$?
check "shutdown ends it and succeeds" '[ $rc -eq 0 ] && ! alive $D && echo "$out" | grep -qi "stopped"'
check "and the pid file is gone" '[ ! -e "$T/run/9share.pid" ]'
out=$("$BIN" shutdown 2>&1); rc=$?
check "a second shutdown is quiet success" '[ $rc -eq 0 ] && echo "$out" | grep -qi "not running"'

# --- a stale pid file must never get an unrelated process killed
sleep 300 </dev/null >/dev/null 2>&1 & S=$!; echo $S >> "$T/pids"; echo $S > "$T/run/9share.pid"
out=$("$BIN" shutdown 2>&1); rc=$?
check "a pid file naming some other process: not running, and that process is left alone" '[ $rc -eq 0 ] && alive $S && echo "$out" | grep -qi "not running"'
out=$("$BIN" status 2>&1)
check "status does not believe it either" 'echo "$out" | grep -q "^daemon:  not running"'
kill $S 2>/dev/null; rm -f "$T/run/9share.pid"

# --- a daemon that will not leave
start_daemon deaf
waitfor 'ready $D'; sleep 0.5
out=$("$BIN" shutdown 2>&1); rc=$?
check "a daemon that ignores SIGTERM: shutdown waits, then fails and says what to do" '[ $rc -ne 0 ] && alive $D && echo "$out" | grep -q -- "--force"'
out=$("$BIN" shutdown --force 2>&1); rc=$?
check "--force kills it" 'waitfor "! alive $D" && [ $rc -eq 0 ]'
rm -f "$T/run/9share.pid"

# --- restart: stop what runs, then start (the start is a stand-in command in the test)
cat > "$T/startcmd" <<EOS
#!/bin/sh
echo started >> "$T/start.log"
EOS
chmod +x "$T/startcmd"
export NINESHARE_BOOT_CMD=$T/startcmd
start_daemon
waitfor 'ready $D'
out=$("$BIN" restart 2>&1); rc=$?
check "restart stops the running daemon" '[ $rc -eq 0 ] && ! alive $D'
check "and starts it again" '[ "$(cat "$T/start.log" 2>/dev/null)" = started ]'
rm -f "$T/start.log"
out=$("$BIN" restart 2>&1); rc=$?
check "restart with nothing running just starts it" '[ $rc -eq 0 ] && [ "$(cat "$T/start.log" 2>/dev/null)" = started ]'

# --- usage
"$BIN" shutdown --bogus >/dev/null 2>&1; rc=$?
check "a wrong option is a usage error" '[ $rc -eq 2 ]'
check "help lists both commands" '"$BIN" help | grep -q "9share shutdown" && "$BIN" help | grep -q "9share restart"'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
