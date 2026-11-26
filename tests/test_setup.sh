#!/bin/sh
# test_setup.sh -- 9share-setup: the simple, usual configuration, from arguments and from questions asked at a terminal (a
# pseudo-terminal made by script(1)); and /etc/9share/9share.conf as the package installs it (comments only: nothing is shared or
# mounted until the user says so).
set -u
BIN=${NINESHARE_BIN:?}; SETUP=${NINESHARE_SETUP:?}; SRC=${NINESHARE_SRCDIR:?}
T=$(mktemp -d /tmp/9share-setup.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
mkdir -p "$T/bin" "$T/home" "$T/remote" "$T/share/data" "$T/sys"
ln -s "$BIN" "$T/bin/9share"; ln -s "$SETUP" "$T/bin/9share-setup"
cat > "$T/bin/ssh" <<'EOS'
#!/bin/sh
while [ $# -gt 0 ]; do
    case $1 in -o) shift 2;; -T|-N|-G) shift;; *) break;; esac
done
host=$1; shift
[ -n "${FAKE_FAIL:-}" ] && exit "$FAKE_FAIL"
exec env -u XDG_CONFIG_HOME HOME="$FAKE_REMOTE" "$@"
EOS
cat > "$T/bin/sudo" <<'EOS'
#!/bin/sh
echo "sudo $*" >> "$FAKE_LOG"
exit 0
EOS
chmod +x "$T/bin/ssh" "$T/bin/sudo"
fail=0; n=0
check() { n=$((n + 1)); if ! eval "$2"; then echo "FAILED: $1"; fail=$((fail + 1)); fi; }
unset XDG_CONFIG_HOME FAKE_FAIL
export HOME=$T/home FAKE_REMOTE=$T/remote FAKE_LOG=$T/log PATH=$T/bin:$PATH
CONF=$HOME/.config/9share/9share.conf
D=$T/share/data

out=$(9share setup --export "$D" other.example.org proj --no-pair 2>&1); rc=$?
check "setup --export writes the export" '[ $rc -eq 0 ] && grep -q "^export \"proj\"" "$CONF" && grep -q "directory \"$D\"" "$CONF" && grep -q "host \"other.example.org\"" "$CONF"'
check "and says what to type on the other machine" 'echo "$out" | grep -q "sudo 9share mount .* proj /mnt/proj"'
check "--no-pair makes no secret" '[ ! -e "$HOME/.config/9share/proj.secret" ] && [ ! -e "$T/remote/.config/9share/proj.secret" ]'

out=$(9share setup --export "$D" other.example.org 2>&1); rc=$?
check "the name is the directory's by default" '[ $rc -eq 0 ] && grep -q "^export \"data\"" "$CONF"'
check "and by default the secret is made on both machines" '[ -f "$HOME/.config/9share/data.secret" ] && cmp -s "$HOME/.config/9share/data.secret" "$T/remote/.config/9share/data.secret"'

FAKE_FAIL=255 9share setup --export "$D" other.example.org later > "$T/o" 2>&1; rc=$?
check "when the secret cannot be made the share is still set up" '[ $rc -eq 0 ] && grep -q "^export \"later\"" "$CONF"'
check "and what to do later is said" 'grep -q "9share pair later other.example.org" "$T/o"'
check "and no secret was left behind" '[ ! -e "$HOME/.config/9share/later.secret" ]'

cp "$CONF" "$T/conf.before"
9share setup --export "$T/nonexistent" host x > "$T/o" 2>&1; rc=$?
check "a directory that is not there is refused" '[ $rc -ne 0 ] && grep -q "is not a directory" "$T/o" && cmp -s "$CONF" "$T/conf.before"'
9share setup --export "$D" > /dev/null 2>&1; rc=$?; check "--export with too few arguments is a usage error" '[ $rc -eq 2 ]'
9share setup --export "$D" h n extra > /dev/null 2>&1; rc=$?; check "--export with too many is a usage error" '[ $rc -eq 2 ]'
9share setup --export "$D" h --bogus > /dev/null 2>&1; rc=$?; check "an unknown option is a usage error" '[ $rc -eq 2 ]'
9share setup bogus > /dev/null 2>&1; rc=$?; check "an unknown word is a usage error" '[ $rc -eq 2 ]'
9share setup --help > "$T/o" 2>&1; rc=$?; check "--help is usage, and not an error" '[ $rc -eq 0 ] && grep -q "usage: 9share setup" "$T/o"'
9share setup < /dev/null > "$T/o" 2>&1; rc=$?; check "without a terminal and without arguments it does not hang or guess" '[ $rc -ne 0 ] && grep -q "not a terminal" "$T/o"'

: > "$FAKE_LOG"
out=$(9share setup --mount alice proj /mnt/proj 2>&1); rc=$?
check "setup --mount asks root for the mount" '[ $rc -eq 0 ] && grep -qx "sudo 9share mount alice proj /mnt/proj" "$FAKE_LOG"'
check "and says what to do on the other machine" 'echo "$out" | grep -q "9share setup --export DIR" && echo "$out" | grep -q "9share pair proj"'

# the questions, at a pseudo-terminal
if command -v script >/dev/null 2>&1; then
    rm -rf "$HOME/.config" "$T/remote/.config"
    printf '1\n%s\nother.example.org\nasked\nn\n' "$D" | timeout 30 script -qec "9share setup" /dev/null > "$T/o" 2>&1
    check "asked: the usual case (this machine has the files) is the first and the default" 'grep -q "^export \"asked\"" "$CONF" && grep -q "directory \"$D\"" "$CONF"'
    check "asked: no secret when the answer is no" '[ ! -e "$HOME/.config/9share/asked.secret" ]'
    rm -rf "$HOME/.config"
    printf '\n%s\nother.example.org\n\ny\n' "$D" | timeout 30 script -qec "9share setup" /dev/null > "$T/o" 2>&1
    check "asked: an empty answer takes the default (1, and the directory's name), and yes makes the secret" 'grep -q "^export \"data\"" "$CONF" && cmp -s "$HOME/.config/9share/data.secret" "$T/remote/.config/9share/data.secret"'
    : > "$FAKE_LOG"
    printf '2\nalice\nproj\n\n' | timeout 30 script -qec "9share setup" /dev/null > "$T/o" 2>&1
    check "asked: the other case (this machine shows the files), the mount point defaulting to /mnt/NAME" 'grep -qx "sudo 9share mount alice proj /mnt/proj" "$FAKE_LOG"'
    printf '3\n' | timeout 30 script -qec "9share setup" /dev/null > "$T/o" 2>&1
    check "asked: another answer is refused" 'grep -q "answer 1 or 2" "$T/o"'
else
    echo "script(1) is missing: the questions are not tested"
fi

# the template the package puts in /etc/9share: comments only, so nothing is shared or mounted, and it reads without complaint
cp "$SRC/etc/9share.conf" "$T/sys/9share.conf"
check "the template has no export or mount block" '! grep -q "^[^#]" "$T/sys/9share.conf"'
check "and it tells the reader to run 9share setup" 'grep -q "9share setup" "$T/sys/9share.conf"'
out=$(NINESHARE_SYSCONFDIR=$T/sys HOME=$T/emptyhome 9share status 2>&1); rc=$?
check "status reads it without a complaint and shows nothing shared" '[ $rc -eq 0 ] && ! echo "$out" | grep -q "9share.conf:[0-9]*:"'

echo "$n checks, $fail failed"
[ $fail -eq 0 ]
