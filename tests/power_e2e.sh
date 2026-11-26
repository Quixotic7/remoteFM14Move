#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The power button as Move's own software has it (the owner's 2026-11-26; rfm1cs-abletonmove RFM_MOVE_ACT_POWEROFF)
# on the simulator, in the build image after make. Move Remote's test hook injects the button's SysEx report
# ("power 1" / "power 0"). A press: "Shut Down?" over the screen, the host running; Back closes it; a press, the power
# button again: closed; a press, a pad: closed; a press, the jog click: the host stops as for Exit (BYE(3), saved) and
# asks for the power off, which the simulator only logs; exit 0. Then a second run: the button held 2 s, no popup
# answered: the same power off by Move Remote's own hold.
#   tests/power_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "power_e2e"
T=$(mktemp -d /tmp/rfm14power.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root
mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17854; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; sleep 0.3; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; sleep 0.3; }
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.3; }
power() { cmd "power 1"; sleep 0.15; cmd "power 0"; sleep 0.4; }   # a short press (the core's debounce: 120 ms)
alive() { kill -0 "$HOST" 2>/dev/null; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]' && cp "$SIM/mr-screen.pbm" "$1"; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site \
        --fw felucca --test-api --no-fm1 >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' \
        || { bad "the host did not start"; tail -20 "$L"; exit 1; }
    sleep 1.5
}
O=build/test-out
start
press 40; sleep 0.6                                        # Track 4: the Mixer (a still screen)
shot $O/power_before.pbm
power
shot $O/power_asked.pbm
alive && ! cmp -s $O/power_before.pbm $O/power_asked.pbm && ! grep -q "Shut Down from" "$L" \
    && ok "a press: the screen changed (Shut Down?), still running" || bad "a press: no popup, or the host left"
press 51                                                   # Back: closed
shot $O/power_back.pbm
alive && ! cmp -s $O/power_asked.pbm $O/power_back.pbm && ok "Back: closed, still running" || bad "Back did not close it"
power; shot $O/power_asked2.pbm
power; shot $O/power_again.pbm
cmp -s $O/power_asked.pbm $O/power_asked2.pbm && ! cmp -s $O/power_asked.pbm $O/power_again.pbm && alive \
    && ok "a press: the same popup; the power button again: closed" || bad "the power button again"
power; hit 68; shot $O/power_pad.pbm
alive && ! cmp -s $O/power_asked.pbm $O/power_pad.pbm && ok "a pad: closed, still running" || bad "a pad did not close it"
sleep 2.5
alive && ! grep -q "stopping: power off" "$L" && ok "short presses: no 2 s power off" || bad "short presses powered off"
power; click                                               # the jog click: shut down
if wait_for 10 '! alive'; then
    wait "$HOST"; RC=$?
    grep -q "Shut Down from the power button" "$L" && grep -q "stopping: power off (asked)" "$L" \
        && grep -q "powering the Move off" "$L" && grep -q "power off asked (the simulator" "$L" \
        && grep -q "engine: stopped" "$L" && [ "$RC" = 0 ] \
        && ok "the jog click: the host stopped, then asked for the power off (exit $RC)" \
        || bad "the jog click: exit $RC, log: $(grep -E 'Shut Down|stopping:|power off|powering|engine: stopped' "$L" | tr '\n' ' ')"
    # (the order: the engine stopped, everything saved, before the power off)
    a=$(grep -n "engine: stopped" "$L" | head -1 | cut -d: -f1); b=$(grep -n "power off asked (the simulator" "$L" | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ] && ok "the engine stopped before the power off" \
        || bad "the order: engine stopped at ${a:-?}, power off at ${b:-?}"
else
    bad "the jog click: the host still running"; cmd quit; wait "$HOST"
fi
cp "$L" $O/power_host.log

# the hold: 2 s, the popup up (not answered): Move Remote's own power off, the host stopped first as well
L=$T/host2.log
start
cmd "power 1"
if wait_for 6 '! alive'; then
    wait "$HOST"; RC=$?
    grep -q "stopping: power off" "$L" && ! grep -q "power off (asked)" "$L" && grep -q "power off asked (the simulator" "$L" \
        && grep -q "engine: stopped" "$L" && [ "$RC" = 0 ] \
        && ok "held 2 s: the power off, the host stopped first (exit $RC)" \
        || bad "held 2 s: exit $RC, log: $(grep -E 'stopping:|power off|engine: stopped' "$L" | tr '\n' ' ')"
else
    bad "held 2 s: the host still running"; cmd "power 0"; cmd quit; wait "$HOST"
fi
cp "$L" $O/power_hold.log
echo "power_e2e: $checks checks, $fail failed (the host's logs: $O/power_host.log, $O/power_hold.log)"
[ $fail = 0 ]
