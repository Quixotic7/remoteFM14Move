#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The firmware catalog on Move Remote's simulator, end to end, in the build image: the Move's browser (rfm1cs's
# projects view, Mute: the catalog) downloads and updates firmwares from a local server, as the Move does from the
# catalog's base URL; loads while another firmware plays; the firmwares' log lines.
#   tests/dl_e2e.sh       (after make and the test catalog: build/testcat, tools/build_move_catalog.py --only
#                          amb,choralroot,felucca; build/testfw/choralroot-0.40.*, an older ChoralRoot to update)
# The servers are BusyBox httpd on 127.0.0.1 (the catalog; a copy whose AMB-1 object is damaged). Downloads go through
# BusyBox wget (phase A, as on the Move) and Debian's GNU wget (phase B): the same flags (-q -T -O), the same exit codes.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14dl.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17731; GOOD=18080; BAD=18081
mkdir -p "$SIM" "$T/bundled"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
status() { curl -s "http://127.0.0.1:$PORT/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
start() {   # start [VAR=value ...]: the host, with those in its environment (XARGS: more arguments)
    env "$@" build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares "$T/bundled" \
        --site build/site --catalog-index build/testcat/index.json --fw felucca ${XARGS:-} >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' &&
        wait_for 5 '[ "$(field session)" = 2 ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
url() { echo "$1" > "$ROOT/catalog-url"; }
view() { cmd "cc 43 127"; sleep 0.15; press "note 16"; cmd "cc 43 0"; sleep 0.3; }   # Track 1 held + step 1
back() { press "cc 51"; press "cc 51"; press "cc 51"; press "cc 51"; }
U="http://127.0.0.1:$PORT"
post() { curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "$1" "$U/api/load"; }
files() { ls "$ROOT/firmwares" | grep "^$1-" | tr '\n' ' '; }
phase() { case "${PHASES:-ABCDE}" in *$1*) return 0;; esac; return 1; }   # (PHASES=AD: only those)

echo "dl_e2e"
for f in build/fw/felucca-1.8.2.* build/testfw/choralroot-0.40.*; do cp "$f" "$T/bundled/"; done
AMB=$(cd build/testcat && ls amb-*.rfm1m); AMB=${AMB%.rfm1m}
CR=$(cd build/testcat && ls choralroot-*.rfm1m); CR=${CR%.rfm1m}
[ -n "$AMB" ] && [ -n "$CR" ] || { echo "  FAIL  no test catalog in build/testcat"; exit 1; }
# the servers: the catalog, and a copy with one byte of AMB-1's object changed (its SHA-256 no longer the index's)
mkdir -p "$T/badcat"; cp build/testcat/* "$T/badcat/"
printf 'X' | dd of="$T/badcat/$AMB.rfm1m" bs=1 seek=4096 conv=notrunc 2>/dev/null
busybox httpd -f -p 127.0.0.1:$GOOD -h build/testcat & S1=$!
busybox httpd -f -p 127.0.0.1:$BAD -h "$T/badcat" & S2=$!
sleep 0.3

# ---------------------------------------------------------------- phase A: BusyBox wget
if phase A; then
mkdir -p "$ROOT"; url "http://127.0.0.1:$BAD"
start RFM14_WGET="busybox wget" || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
[ "$(field cat_n)" = 3 ] && [ "$(field cat_installed)" = 2 ] && [ "$(field cat_from)" = 1 ] \
    && ok "the catalog: 3 firmwares from the bundled index ($(field cat_gen)), 2 installed (Felucca, ChoralRoot 0.40)" \
    || bad "the catalog: $(field cat_n) / $(field cat_installed) / from $(field cat_from)"
wait_for 8 '[ "$(field index_ok)" = 1 ]' && ok "the index fetched at start (busybox wget), the same as the bundled one: no new list" \
    || bad "the index at start: ok $(field index_ok), failed $(field index_failed): $(field index_err)"
# Mute: the catalog; A-Z: AMB-1 (not here), ChoralRoot (an update), Felucca; the first pad selects AMB-1
view; press "cc 88"; press "note 92"
cmd shot; sleep 0.4
# the thumbnail: the screen's right half (x 64..127) is the index's 64 x 64 bits, exactly
sed -n 's/.*"id":"amb","name":[^{]*"thumb":"\([^"]*\)".*/\1/p' build/testcat/index.json | base64 -d > "$T/thumb.bin"
xxd -b -c 8 "$T/thumb.bin" | awk '{ s = ""; for (i = 2; i <= 9; i++) s = s $i; print s }' > "$T/thumb.txt"
tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | cut -c65-128 > "$T/screen.txt"
want=$(tr -cd 1 < "$T/thumb.txt" | wc -c); got=$(tr -cd 1 < "$T/screen.txt" | wc -c)
[ "$(wc -l < "$T/thumb.txt")" = 64 ] && [ "$want" -gt 100 ] && cmp -s "$T/thumb.txt" "$T/screen.txt" \
    && ok "the catalog shows AMB-1's thumbnail from the index: the screen's 64 x 64 is its bits ($got pixels lit, $want in the index)" \
    || bad "the thumbnail: $got pixels lit on the screen's right half, $want in the index"
# a damaged file: refused
press "cc 3"
wait_for 10 '[ "$(field dl_state)" = 3 ]' && field dl_err | grep -q "refused: not the catalog's SHA-256" \
    && [ -z "$(files amb)" ] && [ -z "$(ls "$ROOT/firmwares/.dl")" ] \
    && ok "a file whose SHA-256 is not the index's is refused: -1, nothing installed, nothing left in .dl ($(field dl_err | cut -c1-70)..)" \
    || bad "the bad SHA: state $(field dl_state), err '$(field dl_err)', files '$(files amb)'"
# no network: -1, and a clear state (the next one works)
url "http://127.0.0.1:1"; sleep 2.2   # (the failure's popup)
press "cc 3"
wait_for 10 '[ "$(field dl_failed)" = 2 ]' && [ -z "$(files amb)" ] && [ -z "$(ls "$ROOT/firmwares/.dl")" ] \
    && ok "no network: -1 ($(field dl_err | cut -c1-60)), nothing installed, nothing left" \
    || bad "no network: failed $(field dl_failed), err '$(field dl_err)'"
url "http://127.0.0.1:$GOOD"; sleep 2.2
press "cc 3"
wait_for 15 '[ "$(field dl_state)" = 2 ]' && [ "$(field dl_pct)" = 100 ] && [ -f "$ROOT/firmwares/$AMB.rfm1m" ] \
    && [ -f "$ROOT/firmwares/$AMB.json" ] && [ -f "$ROOT/firmwares/$AMB.licenses.txt" ] \
    && ok "Mute, AMB-1, jog press: downloaded (progress 100), checked, installed: $(files amb)" \
    || bad "the download: state $(field dl_state), pct $(field dl_pct), err '$(field dl_err)', files '$(files amb)'"
fws=$(curl -s "$U/api/firmwares" | tr -d '\n')
echo "$fws" | grep -q '"id":"amb"' && [ "$(field cat_installed)" = 3 ] \
    && echo "$fws" | grep -q '"id":"amb"[^}]*"description":"A generative ambient' \
    && echo "$fws" | grep -q '"id":"amb"[^}]*"categories":\["Synth","Other"\]' \
    && ok "listed installed (3 now): /api/firmwares has AMB-1, with its description and categories" \
    || bad "/api/firmwares after the download: $(echo "$fws" | head -c 300)"
[ "$(post '{"slot":1,"id":"amb"}')" = 202 ] && wait_for 10 '[ "$(field id1)" = amb ]' \
    && ok "the downloaded firmware loads (slot 2: AMB-1 $(field ver1))" || bad "AMB-1 does not load: slot 2 '$(field id1)'"
back
stop
[ "$RC" = 0 ] && ok "phase A: the host left cleanly" || bad "phase A: exit $RC"
fi

# ---------------------------------------------------------------- phase B: Debian's GNU wget; an update
if phase B; then
start
[ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field id0)" = choralroot ]' && [ "$(field ver0)" = 0.40 ] \
    && ok "slot 1: ChoralRoot 0.40" || bad "slot 1 is '$(field id0)' $(field ver0)"
b0=$(field boot0); sleep 2.6   # (its flash saved)
view; press "cc 88"; press "note 93"; press "cc 3"
wait_for 15 '[ "$(field dl_ok)" = 1 ]' && [ -f "$ROOT/firmwares/$CR.rfm1m" ] && [ -f "$ROOT/firmwares/choralroot-0.40.rfm1m" ] \
    && ok "the update (GNU wget): $CR installed beside 0.40, which slot 1 still runs" \
    || bad "the update: ok $(field dl_ok), state $(field dl_state), err '$(field dl_err)', files '$(files choralroot)'"
[ "$(field ver0)" = 0.40 ] && [ "$(field boot0)" = "$b0" ] && ok "slot 1 kept playing 0.40 through the update (not restarted)" \
    || bad "slot 1: $(field ver0), boot $b0 -> $(field boot0)"
fws=$(curl -s "$U/api/firmwares" | tr -d '\n')
echo "$fws" | grep -q "\"id\":\"choralroot\",\"name\":\"ChoralRoot\",\"version\":\"${CR#choralroot-}\"" \
    && ok "/api/firmwares lists the new version" || bad "/api/firmwares: $(echo "$fws" | grep -o '"id":"choralroot"[^,]*,[^,]*,[^,]*')"
back
[ "$(post '{"slot":0,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(field ver0)" = "${CR#choralroot-}" ]' \
    && ok "loaded again: slot 1 runs ChoralRoot $(field ver0)" || bad "the reload: slot 1 runs $(field ver0)"
grep -q "slot 1: ChoralRoot ${CR#choralroot-} (its saved flash)" "$LOG" && ok "with the flash 0.40 saved (states are keyed by id)" \
    || bad "the new version did not start from the saved flash"
wait_for 5 '[ ! -f "$ROOT/firmwares/choralroot-0.40.rfm1m" ] && [ ! -f "$ROOT/firmwares/choralroot-0.40.json" ]' \
    && ok "0.40's files removed once no slot runs it: $(files choralroot)" || bad "0.40 is still there: $(files choralroot)"
stop
fi

# ---------------------------------------------------------------- phase C: loads while another firmware plays
late_run() {   # late_run ENV: Felucca plays in slot 1 while slot 2 loads ChoralRoot, AMB-1, ChoralRoot; the late blocks
    rm -f "$ROOT/move/library.rmvl"
    start RFM14_TEST_BOOT_MS=40 "$1"
    cmd "note 84 110" "note 88 110" "note 91 110"; sleep 0.5
    for id in choralroot amb choralroot; do
        n=$(field boot1); post "{\"slot\":1,\"id\":\"$id\"}" >/dev/null
        wait_for 10 '[ "$(field boot1)" != "$n" ] && [ "$(field id1)" = $id ]' || echo "  info  $id did not load"
        sleep 0.4
    done
    cmd "note 84 0" "note 88 0" "note 91 0"; sleep 1.2
    LATE=$(field late); PEAK=$(field peak)
    stop
}
if phase C; then
late_run RFM14_NO_PREROLL=1; control=$LATE
late_run RFM14_NO_PREROLL=0
[ "$LATE" = 0 ] && [ "$control" -gt 0 ] \
    && ok "3 loads into slot 2 while Felucca plays in slot 1, each start held 40 ms (RFM14_TEST_BOOT_MS, a slow Move): 0 late blocks (without the ring filled first: $control)" \
    || bad "late blocks during loads: $LATE (without the preroll: $control)"
fi

# ---------------------------------------------------------------- phase D: the firmwares' log lines
# ChoralRoot prints three lines a key ("touch: key 7 vel 100 fixed", "chord: C (1 note)", "chord: -"): 30 keys in 6 s
# (pad 88: its key 7)
if phase D; then
LOG=$T/host-d.log; ROOT=$T/root-d; mkdir -p "$ROOT"; url "http://127.0.0.1:1"
XARGS="--fw choralroot" start
wait_for 10 '[ "$(field id0)" = choralroot ]'
sleep 1
i=0; while [ $i -lt 30 ]; do cmd "note 88 100"; sleep 0.12; cmd "note 88 0"; sleep 0.08; i=$((i + 1)); done
sleep 1.5
stop
said=$(cat "$LOG".1 "$LOG" 2>/dev/null | grep -c '^\[choralroot\] ')
dropped=$(cat "$LOG".1 "$LOG" 2>/dev/null | sed -n 's/^\[choralroot\] (\([0-9]*\) lines dropped.*/\1/p' | awk '{ s += $1 } END { print s + 0 }')
info "ChoralRoot's lines: $said in the log, $dropped dropped (30 keys in 6 s: 90 lines)"
[ "$dropped" -gt 20 ] && [ "$said" -le 55 ] && ok "its lines are rate-limited (at most 4 a second after a burst of 20; $dropped dropped, said so)" \
    || bad "the rate limit: $said lines, $dropped dropped"
[ -n "${DEBUG:-}" ] && cat "$LOG"
fi

# ---------------------------------------------------------------- phase E: the log's cap
if phase E; then
LOG=$T/host-e.log; ROOT=$T/root-e; mkdir -p "$ROOT"; url "http://127.0.0.1:1"
XARGS="--log-cap 600" start
sleep 4
stop
[ -f "$LOG.1" ] && grep -q "^remotefm1-host .*: root" "$LOG.1" && grep -q "the lines before are in $LOG.1" "$LOG" \
    && [ "$(stat -c %s "$LOG.1")" -lt 3000 ] \
    && ok "the log past its cap (600 bytes here, 1 MB on the Move) moved to .1, a new one in its place: $(stat -c %s "$LOG.1") + $(stat -c %s "$LOG") bytes" \
    || bad "the log's cap: $(ls -l "$LOG"* 2>/dev/null | awk '{print $5}' | tr '\n' ' ')"
fi

kill $S1 $S2 2>/dev/null
echo "dl_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T/host.log"; }
mv "$T" "$T.done" 2>/dev/null
exit $fail
