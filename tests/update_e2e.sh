#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page's Update (src/update.c; README "Updating from the page") on the simulator, in the build image after make and
# tests/update_test.sh (its fixtures in build/update-test, made by tools/make_update.py with the tests' key):
#   - a module folder of the test's (--module-dir: an old help.json, a firmware, bin/heal as the launcher leaves it) and
#     a RemoteFM1 folder with move/ in a data folder of the test's;
#   - refused, nothing changed (no .prev, no backup, no staged copy left): a tampered file, one signed with the
#     catalog's key, a ".." name, a link, a program that is not an ELF, one not executable, a cut-short upload, no room;
#   - good.rfm1u: done; the module has its files (the program, help.json, the marker), keeps the rest (bin/heal, the
#     firmware), .prev holds the old ones, a data backup of move/ that GNU tar and BusyBox tar read, the status;
#   - good2.rfm1u over it: .prev is the first update's now (one .prev); Undo: the first back, no .prev, Undo again 409;
#   - full.rfm1u (a module package): remotefm1/ dropped; the backups pruned to the newest 3;
#   - the inside layout (RFM14_TEST_UPDATE_INSIDE=1: the folder above the module's not writable): install and Undo
#     with .update-prev inside the module's folder.
#   tests/update_e2e.sh      (after make and tests/update_test.sh)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "update_e2e"
F=build/update-test
[ -f $F/good.rfm1u ] || { echo "  FAIL  no $F/good.rfm1u: run tests/update_test.sh"; exit 1; }
T=$(mktemp -d /tmp/rfm14upd.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; D=$T/UserData; R=$D/RemoteFM1; MODS=$T/modules/tools; M=$MODS/remotefm1
mkdir -p "$SIM" "$R/move/projects" "$M/site" "$M/bin" "$M/firmwares"
echo "http://127.0.0.1:1" > "$R/catalog-url"
printf 'settings' > "$R/move/settings.bin"
head -c 200000 /dev/zero | tr '\0' '\377' > "$R/move/projects/move-project-01.fw1.bin"
# the module as installed: an older build
cp build/move/remotefm1-host "$M/remotefm1-host"; printf '\n# old\n' >> "$M/remotefm1-host"; chmod 755 "$M/remotefm1-host"
echo '{"old": 1}' > "$M/help.json"
echo '{"id": "remotefm1", "version": "0.4.9"}' > "$M/module.json"
cp -R build/site/. "$M/site/"; echo old > "$M/site/old-only.txt"
echo heal > "$M/bin/heal"; chmod 4755 "$M/bin/heal"
echo fw > "$M/firmwares/felucca-old.txt"
PORT=17851; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
st() { curl -s "$U/api/update"; }
sfield() { st | sed -n "s/.*\"$1\":\"\([^\"]*\)\".*/\1/p" | head -1; }
state() { sfield state; }
msg() { st | sed -n 's/.*"msg":"\(\([^"\\]\|\\.\)*\)".*/\1/p' | sed 's/\\"/"/g'; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --no-fm1 --module-dir "$1" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ]' && wait_for 10 '[ -n "$(state)" ]'
}
stop() { cmd quit; wait $HOST; }
post() { curl -s -o "$T/post.json" -w '%{http_code}' -X POST --data-binary @"$1" -H 'Content-Type: application/octet-stream' "$U/api/update"; }
settle() { wait_for 60 '[ "$(state)" = done ] || [ "$(state)" = failed ]'; }   # (the install's worker)
tree() { (cd "$1" && find . -type f -exec md5sum {} + | sort -k2) | md5sum | cut -c1-32; }
backups() { ls "$D"/remotefm1-data-backup-*.tar.gz 2>/dev/null | wc -l | tr -d ' '; }
refused() {   # file, what the message says
    code=$(post "$F/$1")
    settle
    s=$(state); m=$(msg)
    if [ "$code" = 202 ] && [ "$s" = failed ] && echo "$m" | grep -q "$2" && [ "$(tree "$M")" = "$T0" ] && [ ! -e "$D/.remotefm1-update-prev" ] \
        && [ ! -e "$D/.remotefm1-update-new" ] && [ "$(backups)" = "$B0" ]; then
        ok "$1: refused ($m), nothing changed"
    else
        bad "$1: http $code state '$s' msg '$m' changed: $( [ "$(tree "$M")" = "$T0" ] && echo no || echo YES) prev $( [ -e "$D/.remotefm1-update-prev" ] && echo yes || echo no) backups $(backups)"
    fi
}

start "$M" || { bad "the host did not start"; tail -20 "$L"; exit 1; }
j=$(st)
echo "$j" | grep -q '"state":"idle"' && echo "$j" | grep -q '"running":{"version":"0.5.0"' && echo "$j" | grep -q '"can":true' \
    && echo "$j" | grep -q '"undo":false' && echo "$j" | grep -q '"installed":{"version":"0.4.9"' \
    && ok "the status: idle, running 0.5.0 ($(echo "$j" | sed -n 's/.*"running":{"version":"[^"]*","commit":"\([^"]*\)".*/\1/p')), installed 0.4.9 (module.json), no undo" || bad "the status: $j"
T0=$(tree "$M"); B0=$(backups)

# ---------------------------------------------------------------- refused, nothing changed
refused tampered.rfm1u "signature"
refused catsig.rfm1u "signature"
refused dotdot.rfm1u '\.\.'
refused symlink.rfm1u "link"
refused notelf.rfm1u "not an ELF"
refused noexec.rfm1u "not executable"
# a cut-short upload: the connection closes with bytes still owed
head -c 4000 $F/good.rfm1u > "$T/cut.bin"
curl -s -o /dev/null --max-time 2 -X POST --data-binary @"$T/cut.bin" -H "Content-Length: 900000" "$U/api/update"
wait_for 15 '[ "$(state)" = failed ]' && msg | grep -q "cut short" && [ "$(tree "$M")" = "$T0" ] && [ ! -e "$D/.remotefm1-update.part" ] \
    && ok "a cut-short upload: failed ($(msg)), its file gone, nothing changed" || bad "cut short: '$(state)' '$(msg)'"
code=$(curl -s -o "$T/post.json" -w '%{http_code}' -X POST "$U/api/update/undo")
[ "$code" = 409 ] && ok "Undo with no previous build: 409 ($(cat "$T/post.json"))" || bad "Undo with nothing: $code"

# ---------------------------------------------------------------- good: installed
sleep 1.1
code=$(post $F/good.rfm1u); settle
if [ "$code" = 202 ] && [ "$(state)" = done ] && msg | grep -q "Installed 9.9.9-test. Relaunch RemoteFM1 on the Move"; then
    ok "good.rfm1u: done ($(msg))"
else bad "good.rfm1u: http $code '$(state)' '$(msg)' ($(cat "$T/post.json"))"; fi
cmp -s build/move/remotefm1-host "$M/remotefm1-host" && [ -x "$M/remotefm1-host" ] && cmp -s module/help.json "$M/help.json" \
    && [ -f "$M/update-marker.txt" ] && cmp -s build/site/js/moveupdate.js "$M/site/js/moveupdate.js" \
    && grep -q 9.9.9-test "$M/update-manifest.json" \
    && ok "the module has the update's files (the program, help.json, site/, the marker, update-manifest.json)" \
    || bad "the module's files after the install"
[ -f "$M/bin/heal" ] && [ -f "$M/firmwares/felucca-old.txt" ] && [ -f "$M/site/old-only.txt" ] \
    && ok "the rest kept (bin/heal, a firmware, a site file the update does not name)" || bad "the rest after the install"
grep -q '"old": 1' "$D/.remotefm1-update-prev/help.json" && [ ! -f "$D/.remotefm1-update-prev/update-marker.txt" ] && ! cmp -s build/move/remotefm1-host "$D/.remotefm1-update-prev/remotefm1-host" \
    && [ "$(tree "$D/.remotefm1-update-prev")" = "$T0" ] && ok "<dir>.prev holds the old build, whole" || bad "the .prev after the install"
BK=$(ls -t "$D"/remotefm1-data-backup-*.tar.gz 2>/dev/null | head -1)
[ -n "$BK" ] && tar tzf "$BK" | grep -q '^move/settings.bin$' && tar tzf "$BK" | grep -q 'move-project-01.fw1.bin' \
    && busybox tar tzf "$BK" | grep -q 'move/settings.bin' \
    && [ "$(tar xzOf "$BK" move/projects/move-project-01.fw1.bin | md5sum)" = "$(md5sum < "$R/move/projects/move-project-01.fw1.bin")" ] \
    && ok "a data backup: $(basename "$BK") ($(wc -c < "$BK") bytes; GNU tar and BusyBox tar read it, a file in it the same)" \
    || bad "the data backup: '$BK'"
j=$(st)
echo "$j" | grep -q '"installed":{"version":"9.9.9-test"' && echo "$j" | grep -q '"undo":true' \
    && echo "$j" | grep -q '"undo_to":{"version":"0.4.9"' && echo "$j" | grep -q '"staged":{"version":"9.9.9-test"' \
    && ok "the status: installed 9.9.9-test, undo back to 0.4.9" || bad "the status after: $j"
curl -s "$U/js/moveupdate.js" | grep -q mountMoveUpdate && ok "the page still served (its Update script)" || bad "the page after the install"
[ ! -e "$D/.remotefm1-update-new" ] && [ ! -e "$D/.remotefm1-update-undone" ] && [ ! -e "$D/.remotefm1-update.part" ] && ok "nothing left over (.new, .undone, the upload)" \
    || bad "left over: $(ls -d "$M".* "$D"/.remotefm1* 2>/dev/null)"

# ---------------------------------------------------------------- a second update, Undo
sleep 1.1
code=$(post $F/good2.rfm1u); settle
[ "$(state)" = done ] && grep -q 9.9.10-test "$M/update-manifest.json" && grep -q 9.9.9-test "$D/.remotefm1-update-prev/update-manifest.json" \
    && [ "$(ls -d "$D"/.remotefm1-update-prev* | wc -l)" = 1 ] && [ "$(ls -A "$MODS")" = remotefm1 ] && ok "good2.rfm1u over it: .prev is the first update now (one .prev; nothing but remotefm1 in the tools folder)" \
    || bad "good2: '$(state)' '$(msg)' $(ls -d "$MODS"/remotefm1.*)"
code=$(curl -s -o "$T/post.json" -w '%{http_code}' -X POST "$U/api/update/undo"); settle
[ "$code" = 202 ] && [ "$(state)" = done ] && msg | grep -q "Put back 9.9.9-test" && grep -q 9.9.9-test "$M/update-manifest.json" \
    && [ ! -e "$D/.remotefm1-update-prev" ] && [ ! -e "$D/.remotefm1-update-undone" ] && st | grep -q '"undo":false' \
    && ok "Undo: 9.9.9-test back ($(msg)), no .prev, no Undo left" || bad "Undo: $code '$(state)' '$(msg)'"
code=$(curl -s -o "$T/post.json" -w '%{http_code}' -X POST "$U/api/update/undo")
[ "$code" = 409 ] && ok "Undo again: 409 (one level)" || bad "Undo again: $code"

# ---------------------------------------------------------------- a whole module package; the backups pruned
sleep 1.1
code=$(post $F/full.rfm1u); settle
[ "$(state)" = done ] && [ -f "$M/firmwares/full-marker.txt" ] && grep -q '"full": 1' "$M/help.json" && [ ! -e "$M/remotefm1" ] \
    && [ -f "$M/firmwares/felucca-old.txt" ] && ok "full.rfm1u: remotefm1/ dropped, its files in place, the rest kept" \
    || bad "full: '$(state)' '$(msg)'"
sleep 1.1
code=$(post $F/good.rfm1u); settle
[ "$(state)" = done ] && [ "$(backups)" = 3 ] && ok "a fourth install: the backups pruned to the newest 3" \
    || bad "the backups: $(backups) ('$(state)' '$(msg)')"
stop

# ---------------------------------------------------------------- no room
RFM14_TEST_DISK_FREE_MB=10 start "$M" || { bad "the host did not start (no room)"; exit 1; }
T1=$(tree "$M")
code=$(post $F/good2.rfm1u)
[ "$code" = 507 ] && [ "$(tree "$M")" = "$T1" ] && ok "no room on the disk: 507 ($(cat "$T/post.json")), nothing changed" || bad "no room: $code"
stop

# ---------------------------------------------------------------- the inside layout
M2=$T/ro/remotefm1; mkdir -p "$M2/bin"
cp build/move/remotefm1-host "$M2/remotefm1-host"; echo '{"old": 2}' > "$M2/help.json"; echo heal > "$M2/bin/heal"
echo '{"version": "0.4.8"}' > "$M2/module.json"
T2=$(tree "$M2")
RFM14_TEST_UPDATE_INSIDE=1 start "$M2" || { bad "the host did not start (inside)"; exit 1; }
st | grep -q '"layout":"inside"' && ok "the inside layout (the folder above not writable)" || bad "inside: $(st)"
sleep 1.1
code=$(post $F/good.rfm1u); settle
[ "$(state)" = done ] && cmp -s module/help.json "$M2/help.json" && [ -f "$M2/update-marker.txt" ] && [ -f "$M2/bin/heal" ] \
    && grep -q '"old": 2' "$M2/.update-prev/help.json" && [ ! -e "$M2.prev" ] && [ ! -e "$M2/.update-new" ] \
    && ok "inside: installed, the old build in <dir>/.update-prev" || bad "inside install: '$(state)' '$(msg)' $(ls -a "$M2")"
code=$(curl -s -o "$T/post.json" -w '%{http_code}' -X POST "$U/api/update/undo"); settle
[ "$(state)" = done ] && [ "$(tree "$M2")" = "$T2" ] && [ ! -e "$M2/.update-prev" ] && [ ! -e "$M2/.update-undone" ] \
    && ok "inside: Undo, the folder as it was (every file the same)" || bad "inside undo: '$(state)' '$(msg)' $(ls -a "$M2")"
stop
cp "$L" build/test-out/update_host.log
echo "update_e2e: $checks checks, $fail failed (the host's log: build/test-out/update_host.log)"
[ $fail = 0 ]
