#!/bin/sh
# test_init.sh -- 9share-init: which kind of machine is this (systemd, WSL2, WSL1, OpenRC, sysvinit, runit, none), and what is done
# for each.  The machine is pretended with a directory (NINESHARE_ROOT) and the commands it would run are stand-ins that only
# record their arguments.  Also `9share wsl-boot` and `wsl-boot --remove` on a copy of /etc/wsl.conf.
set -u
BIN=${NINESHARE_BIN:?}; INIT=${NINESHARE_INIT:?}
T=$(mktemp -d /tmp/9share-init.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/bin"
for c in systemctl rc-update rc-service update-rc.d service 9share; do
    cat > "$T/bin/$c" <<'EOS'
#!/bin/sh
echo "${0##*/} $*" >> "$FAKE_LOG"
[ -n "${FAKE_FAIL:-}" ] && exit 1
exit 0
EOS
    chmod +x "$T/bin/$c"
done
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
export PATH=$T/bin:$PATH FAKE_LOG=$T/log
unset NINESHARE_ROOT FAKE_FAIL

# a pretended machine: machine NAME [files...]; the directory is NINESHARE_ROOT
machine() { rm -rf "$T/root"; mkdir -p "$T/root/proc" "$T/root/run" "$T/root/etc/init.d" "$T/root/sbin"; : > "$FAKE_LOG"; }
kind() { NINESHARE_ROOT=$T/root "$INIT" detect; }
run() { NINESHARE_ROOT=$T/root "$INIT" "$@" 2>&1; }

machine; check "an empty root is none" '[ "$(kind)" = none ]'
machine; mkdir -p "$T/root/run/systemd/system"; check "systemd is found by /run/systemd/system" '[ "$(kind)" = systemd ]'
machine; echo "Linux version 5.15.153.1-microsoft-standard-WSL2 (root@x)" > "$T/root/proc/version"
check "WSL2 is found in /proc/version" '[ "$(kind)" = wsl2 ]'
mkdir -p "$T/root/run/systemd/system"
check "WSL2 with systemd (systemd=true in wsl.conf) is systemd" '[ "$(kind)" = systemd ]'
machine; echo "Linux version 4.4.0-19041-Microsoft (Microsoft@Microsoft.com)" > "$T/root/proc/version"
check "WSL1 is told from WSL2" '[ "$(kind)" = wsl1 ]'
machine; mkdir -p "$T/root/proc/sys/fs/binfmt_misc"; : > "$T/root/proc/sys/fs/binfmt_misc/WSLInterop"
check "WSL2 is also found by its interop file" '[ "$(kind)" = wsl2 ]'
machine; mkdir -p "$T/root/run/openrc"; check "OpenRC is found by /run/openrc" '[ "$(kind)" = openrc ]'
machine; mkdir -p "$T/root/run/runit"; check "runit is found by /run/runit" '[ "$(kind)" = runit ]'
machine; : > "$T/root/sbin/init"; chmod +x "$T/root/sbin/init"; : > "$T/root/etc/inittab"
check "sysvinit is found by /sbin/init and inittab" '[ "$(kind)" = sysv ]'

# systemd: the package has done it (debhelper); by hand, systemctl does
machine; mkdir -p "$T/root/run/systemd/system"
out=$(run enable --from-package); check "systemd from the package: said, and nothing run" '[ $? -eq 0 ] && echo "$out" | grep -q "systemd: the service is enabled" && [ ! -s "$FAKE_LOG" ]'
out=$(run enable); check "systemd by hand: enable --now" 'grep -qx "systemctl enable --now 9share.service" "$FAKE_LOG" && echo "$out" | grep -q "enabled and started"'

# WSL2 without systemd: the boot command of wsl.conf, and start it now
machine; echo "5.15.0-microsoft-standard-WSL2" > "$T/root/proc/version"
out=$(run enable --from-package); check "WSL2: wsl-boot, then started now" '[ $? -eq 0 ] && [ "$(sed -n 1p "$FAKE_LOG")" = "9share wsl-boot" ] && [ "$(sed -n 2p "$FAKE_LOG")" = "9share boot --detach" ] && echo "$out" | grep -q "wsl2: started now"'
: > "$FAKE_LOG"; out=$(FAKE_FAIL=1 run enable); rc=$?
check "WSL2 when wsl.conf cannot be edited: never fails, says what to type" '[ $rc -eq 0 ] && echo "$out" | grep -q "sudo 9share wsl-boot"'
: > "$FAKE_LOG"; run disable >/dev/null; check "WSL2 disable removes the boot command" 'grep -qx "9share wsl-boot --remove" "$FAKE_LOG"'

# WSL1
machine; echo "4.4.0-Microsoft" > "$T/root/proc/version"
out=$(run enable); check "WSL1: explained, nothing run, not a failure" '[ $? -eq 0 ] && echo "$out" | grep -q "WSL1 has no 9p" && [ ! -s "$FAKE_LOG" ]'

# OpenRC
machine; mkdir -p "$T/root/run/openrc"; printf '#!/bin/sh\n' > "$T/root/etc/init.d/9share"; chmod +x "$T/root/etc/init.d/9share"
out=$(run enable); check "OpenRC: added to the default runlevel and started" 'grep -qx "rc-update add 9share default" "$FAKE_LOG" && grep -qx "rc-service 9share start" "$FAKE_LOG" && echo "$out" | grep -q "openrc: added"'
: > "$FAKE_LOG"; run disable >/dev/null; check "OpenRC disable: stopped and removed" 'grep -qx "rc-service 9share stop" "$FAKE_LOG" && grep -qx "rc-update del 9share default" "$FAKE_LOG"'
rm "$T/root/etc/init.d/9share"; : > "$FAKE_LOG"; out=$(run enable); rc=$?
check "OpenRC without the init script: says so, never fails" '[ $rc -eq 0 ] && echo "$out" | grep -q "could not add" && echo "$out" | grep -q "9share boot --detach"'

# sysvinit: update-rc.d is the package's (dh_installinit); by hand it is done here
machine; : > "$T/root/sbin/init"; chmod +x "$T/root/sbin/init"; : > "$T/root/etc/inittab"
out=$(run enable --from-package); check "sysvinit from the package: said, nothing run" '[ $? -eq 0 ] && echo "$out" | grep -q "sysvinit: the service is enabled" && [ ! -s "$FAKE_LOG" ]'
out=$(run enable); check "sysvinit by hand: update-rc.d defaults, service start" 'grep -qx "update-rc.d 9share defaults" "$FAKE_LOG" && grep -qx "service 9share start" "$FAKE_LOG"'

# runit and nothing at all: told what to type, not a failure
machine; mkdir -p "$T/root/run/runit"; out=$(run enable); check "runit: told how, not a failure" '[ $? -eq 0 ] && echo "$out" | grep -q "runit" && echo "$out" | grep -q "9share boot --detach"'
machine; out=$(run enable); check "no init (a container): told how, not a failure" '[ $? -eq 0 ] && echo "$out" | grep -q "no init system" && echo "$out" | grep -q "sudo 9share boot --detach" && [ ! -s "$FAKE_LOG" ]'
"$INIT" bogus 2>/dev/null; rc=$?; check "an unknown command is a usage error" '[ $rc -eq 2 ]'

# the real `9share wsl-boot` on a copy of wsl.conf
W=$T/wsl.conf; export NINESHARE_WSL_CONF=$W NINESHARE_SYSCONFDIR=$T/etc; mkdir -p "$T/etc"
rm -f "$W"; "$BIN" wsl-boot >/dev/null; rc=$?
check "wsl-boot makes a [boot] command" '[ $rc -eq 0 ] && grep -q "^\[boot\]" "$W" && grep -q "^command = .*9share boot --detach" "$W"'
"$BIN" wsl-boot > "$T/o"; check "and is told when it is there already" 'grep -q "already set" "$T/o" && [ "$(grep -c "^command" "$W")" = 1 ]'
"$BIN" wsl-boot --remove >/dev/null; check "--remove takes it out" '! grep -q 9share "$W"'
printf '[automount]\nenabled = true\n\n[boot]\ncommand = "service cron start"\n\n[user]\ndefault = alice\n' > "$W"
"$BIN" wsl-boot > "$T/o"; check "a boot command of the user's is kept and runs first" 'grep -q "kept your boot command" "$T/o" && sed -n 2p "$T/etc/wsl-boot" | grep -qx "service cron start" && grep -q "9share boot --detach" "$T/etc/wsl-boot"'
check "the rest of wsl.conf is untouched" 'grep -q "^enabled = true" "$W" && grep -q "^default = alice" "$W"'
"$BIN" wsl-boot --remove >/dev/null; check "--remove puts the user's command back" 'grep -qx "command = \"service cron start\"" "$W" && ! grep -q 9share "$W" && [ ! -e "$T/etc/wsl-boot" ]'
check "and still the rest is untouched" 'grep -q "^enabled = true" "$W" && grep -q "^default = alice" "$W"'
"$BIN" wsl-boot --remove > "$T/o"; rc=$?; check "--remove when there is nothing to remove is quiet" '[ $rc -eq 0 ] && [ ! -s "$T/o" ]'
"$BIN" wsl-boot --bogus 2>/dev/null; rc=$?; check "wsl-boot with a wrong option is a usage error" '[ $rc -eq 2 ]'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
