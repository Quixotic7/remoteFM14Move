#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Latch per slot (rfm1cs-abletonmove 0.5.4) on the simulator, in the build image: MIDI mode (rfm1cs 0.8.0: a note
# layout of the slot, Track n held + the top row's pad 3, Scale; both slots), latch on (Track 1 held + the top row's
# pad 8), a chord on slot 1 (Felucca); the active slot switched to slot 2 (ChoralRoot):
# slot 1 keeps sounding (no note off, no all-notes-off from the host) and slot 2 plays its own note; latch off: both
# let go (each note off to its own slot), silent.
#   tests/latch_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14latch.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17780
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
edit() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"op\":$1,\"a\":$2,\"b\":$3}" "$U/api/edit"; }
load() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":0}" "$U/api/load"; }
quiet() { [ "$(slotf "$1" peak)" -lt 30 ]; }

echo "latch_e2e"
build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca >>"$LOG" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' && wait_for 5 '[ "$(field session)" = 2 ]' \
    || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
[ "$(load 1 choralroot)" = 202 ] && wait_for 15 '[ "$(slotf 1 id)" = choralroot ] && [ "$(field active)" = 0 ]' \
    && ok "slot 1 Felucca (active), slot 2 ChoralRoot" || bad "the slots: '$(slotf 0 id)' '$(slotf 1 id)' active $(field active)"
sleep 1.5
cmd "cc 43 127"; sleep 0.2; press "note 94"; cmd "cc 43 0"; sleep 0.3   # Track 1 held + the top row's pad 3: Scale (MIDI mode)
cmd "cc 42 127"; sleep 0.2; press "note 94"; cmd "cc 42 0"; sleep 0.3   # slot 2's too (Track 2 held: not played)
cmd "cc 43 127"; sleep 0.2; press "note 99"; cmd "cc 43 0"; sleep 0.4   # Track 1 held + the top row's pad 8: latch on
wait_for 4 'quiet 0 && quiet 1'
n0=$(slotf 0 notes)
cmd "note 68 100" "note 72 100" "note 75 100"; sleep 0.3; cmd "note 68 0" "note 72 0" "note 75 0"
wait_for 4 '[ "$(slotf 0 notes)" -ge $((n0 + 3)) ] && [ "$(slotf 0 peak)" -gt 200 ]' && sleep 1.2 && [ "$(slotf 0 peak)" -gt 100 ] \
    && ok "MIDI mode, latch on: a chord on slot 1 keeps sounding after the pads let go (Felucca's peak $(slotf 0 peak), $(( $(slotf 0 notes) - n0 )) notes)" \
    || bad "the latched chord: notes $n0 -> $(slotf 0 notes), peak $(slotf 0 peak)"
n1=$(slotf 1 notes)
[ "$(edit 16 1 0)" = 202 ] && wait_for 4 '[ "$(field active)" = 1 ]'
sleep 1.2
p0=$(slotf 0 peak)
[ "$(field active)" = 1 ] && [ "$p0" -gt 100 ] \
    && ok "slot 2 made the active one: slot 1's latched chord still sounds (peak $p0)" || bad "after the switch: active $(field active), slot 1's peak $p0"
cmd "note 68 100"; sleep 0.3; cmd "note 68 0"
wait_for 4 '[ "$(slotf 1 notes)" -gt "$n1" ] && [ "$(slotf 1 peak)" -gt 100 ]' && [ "$(slotf 0 peak)" -gt 100 ] \
    && ok "slot 2 plays its own note (ChoralRoot's peak $(slotf 1 peak)) while slot 1's chord sounds on (peak $(slotf 0 peak))" \
    || bad "slot 2's note: notes $n1 -> $(slotf 1 notes), peak $(slotf 1 peak); slot 1 $(slotf 0 peak)"
cmd "cc 42 127"; sleep 0.2; press "note 99"; cmd "cc 42 0"   # Track 2 held + pad 8: latch off (it is shared): every slot's latched notes let go, each to its slot
wait_for 8 'quiet 0 && quiet 1' && sleep 1.2 && quiet 0 && quiet 1 \
    && ok "latch off: both slots let go, silent (peaks $(slotf 0 peak), $(slotf 1 peak))" \
    || bad "latch off: peaks $(slotf 0 peak), $(slotf 1 peak)"
cmd quit; wait $HOST
echo "latch_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
