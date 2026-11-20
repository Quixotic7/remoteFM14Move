#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# MIDI routes (rfm1cs 0.6.6, README "MIDI out"), in the build image, after make with ChoralRoot among the development
# builds (build/fw-dev): ChoralRoot in slot 1, Felucca in slot 2; pads played in FM-1 mode. Without a route Felucca
# plays nothing; with slot 1 routed to slot 2 and Out (the page's ENG_OP_MROUTE, as the Routing page's pads set it)
# ChoralRoot's notes reach Felucca and Out (queued, its clock filtered); a cycle (slot 2 back to 1) is refused; the
# route taken away: Felucca hears no more.
#   tests/mroute_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
echo "mroute_e2e"
CR=$(ls build/fw-dev/choralroot-*.rfm1m 2>/dev/null | sort -V | tail -1)
[ -n "$CR" ] || { echo "  skip  no ChoralRoot in build/fw-dev"; exit 0; }
T=$(mktemp -d /tmp/rfm14mr.XXXXXX); SIM=$T/sim; R=$T/root; DEV=$T/dev; FWS=$T/fws
mkdir -p "$SIM" "$R" "$DEV" "$FWS"
echo "http://127.0.0.1:1" > "$R/catalog-url"
cp "${CR%.rfm1m}".* "$DEV/"; cp build/fw/felucca-1.8.2.* "$FWS/"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
PORT=17798; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
get() { tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
mo() { status | sed 's/.*"midiout":\({[^}]*}\).*/\1/' | get "$1"; }
notes() { status | grep -o '"slots":\[.*\]' | grep -o '"notes":[0-9]*' | sed -n "$(($1 + 1))p" | cut -d: -f2; }
mroute() { status | grep -o '"mroute":\[[0-9,]*\]' | cut -d'[' -f2 | tr -d ']'; }
edit() { curl -s -o /dev/null -X POST --data "{\"op\":25,\"a\":$1,\"b\":$2}" "$U/api/edit"; sleep 0.4; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
play() { for p in 68 70 72; do cmd "note $p 100"; sleep 0.4; cmd "note $p 0"; sleep 0.3; done; sleep 0.5; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site \
    --dev-firmwares "$DEV" --fw choralroot --test-api --no-fm1 --rtp-port 15098 >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 5 '[ "$(status | get session)" = 2 ]' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
sleep 2
curl -s -o /dev/null -X POST --data '{"slot":1,"id":"felucca","activate":0}' "$U/api/load"
wait_for 15 'status | grep -q "\"id\":\"felucca\""'; sleep 2
play
f0=$(notes 1)
[ "$f0" = 0 ] && [ "$(mroute)" = "0,0,0" ] && ok "no routes: Felucca (slot 2) plays nothing of slot 1's" \
    || bad "no routes: Felucca's notes '$f0', routes '$(mroute)'"
edit 0 10   # slot 1 -> slot 2 (bit 1) and Out (8)
[ "$(mroute)" = "10,0,0" ] && ok "slot 1 routed to slot 2 and Out (routes $(mroute))" || bad "the route: '$(mroute)'"
q0=$(mo queued)
play
f1=$(notes 1); q1=$(mo queued)
[ "${f1:-0}" -gt 0 ] && ok "ChoralRoot's MIDI out played Felucca ($f1 notes)" || bad "routed: Felucca's notes '$f1'"
[ "${q1:-0}" -gt "${q0:-0}" ] && [ "$(mo filtered)" -gt 0 ] && ok "and went to Out ($((q1 - q0)) messages; its clock filtered)" \
    || bad "Out: queued $q0 -> $q1, filtered '$(mo filtered)'"
edit 1 1    # slot 2 -> slot 1: a cycle
[ "$(mroute)" = "10,0,0" ] && ok "a cycle (slot 2 back to slot 1) refused" || bad "the cycle: routes '$(mroute)'"
edit 0 0
play
f2=$(notes 1)
[ "$(mroute)" = "0,0,0" ] && [ "$f2" = "$f1" ] && ok "the route taken away: Felucca hears no more" \
    || bad "unrouted: routes '$(mroute)', Felucca's notes $f1 -> $f2"
cmd quit; wait $HOST
echo "mroute_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
