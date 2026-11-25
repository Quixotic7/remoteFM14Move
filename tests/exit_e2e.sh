#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Global settings' Exit (the owner's 2026-11-25; rfm1cs-abletonmove RFM_MOVE_CAP_EXIT) on the simulator, in the build
# image after make: Shift + step 2, the jog to the last row, a click asks (the host keeps running), Back: not leaving;
# a click, a second click: the host leaves through Move Remote's QUIT ("stopping: the host asked") and exits 0.
#   tests/exit_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "exit_e2e"
T=$(mktemp -d /tmp/rfm14exit.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root
mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17853; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.05; _i=$((_i + 1)); done; }   # n detents, 1 / 127
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.3; }
alive() { kill -0 "$HOST" 2>/dev/null; }

build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
    --test-api --no-fm1 >>"$L" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' \
    || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 1.5
press 40; sleep 0.6                                        # Track 4: the Mixer (out of the FM-1 layout)
cmd "cc 49 127"; hit 17; cmd "cc 49 0"; sleep 0.3          # Shift + step 2: Global settings
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
shot && cp "$SIM/mr-screen.pbm" build/test-out/exit_global.pbm
jog 12 1; sleep 0.2                                        # the last row: Exit
shot && cp "$SIM/mr-screen.pbm" build/test-out/exit_row.pbm
click                                                      # asks
sleep 0.5
shot && cp "$SIM/mr-screen.pbm" build/test-out/exit_asked.pbm
alive && ! grep -q "Exit from Global settings" "$L" && ok "one click on Exit: asked, still running" \
    || bad "one click: the host left (or said Exit)"
press 51; sleep 0.3                                        # Back: not leaving
click; sleep 0.3                                           # asks again
alive && ! grep -q "Exit from Global settings" "$L" && ok "Back, then a click: asked again, still running" \
    || bad "after Back: the host left"
click                                                      # the second click: leave
if wait_for 10 '! alive'; then
    wait "$HOST"; RC=$?
    grep -q "Exit from Global settings" "$L" && grep -q "stopping: the host asked" "$L" && [ "$RC" = 0 ] \
        && ok "the second click: the host left through Move Remote's QUIT (exit $RC)" \
        || bad "the host left: exit $RC, log: $(grep -E 'Exit from|stopping:' "$L" | tr '\n' ' ')"
else
    bad "the second click: the host still running"; cmd quit; wait "$HOST"
fi
cp "$L" build/test-out/exit_host.log
echo "exit_e2e: $checks checks, $fail failed (the host's log: build/test-out/exit_host.log)"
[ $fail = 0 ]
