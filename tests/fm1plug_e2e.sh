#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The FM-1 on the Move's USB-A shows a plug on black (src/fm1link.c), not the firmware's screen: USB-A (~8 KB/s through
# the Move's bridge) can't carry it. A fake FM-1 (src/fm1sim.c) behind the simulated USB-A, in the build image.
#   tests/fm1plug_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14plug.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17733
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
U="http://127.0.0.1:$PORT"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
status() { curl -s "$U/api/status"; }
fm1() { status | sed 's/.*"fm1":\({[^}]*}\).*/\1/'; }
fake() { curl -s "$U/api/test/fm1"; }
get() { tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
ff() { fake | get "$1"; }
lf() { fm1 | get "$1"; }
unit() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"op\":\"$1\",\"a\":$2,\"b\":$3}" "$U/api/test/fm1"; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
wait_for() { _n=$(( $1 * 20 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.05; _n=$((_n - 1)); done; return 1; }

echo "fm1plug_e2e"
build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --test-api --fake-fm1 --no-fm1-wifi >>"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    || { bad "the host did not start"; cat "$T/host.log"; exit 1; }

wait_for 8 '[ "$(lf linked)" = true ] && [ "$(ff session)" = 1 ]' && ok "linked over USB-A" || bad "no session: $(fm1) / $(fake)"
wait_for 5 '[ "$(ff plug)" = 1 ] && [ "$(ff holding)" = 0 ]' && ok "the unit shows the plug on black ($(ff frames) frames)" \
    || bad "the plug screen: $(fake)"
f0=$(ff frames)
for i in 1 2 3 4 5 6 7 8 9 10; do unit enc 0 1 >/dev/null; sleep 0.1; done   # the slot's own screen changes
sleep 1.5
[ "$(ff plug)" = 1 ] && [ "$(ff frames)" = "$f0" ] && ok "the firmware's screen changes, the unit keeps the plug, no frame sent" \
    || bad "after the screen changed: plug $(ff plug), frames $f0 -> $(ff frames)"
[ "$(ff leds_match)" = 1 ] && ok "the LEDs still follow the slot" || bad "LEDs: $(fake)"

cmd quit
wait $HOST
if [ $fail != 0 ]; then echo "---- host.log"; tail -40 "$T/host.log"; fi
echo "fm1plug_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
