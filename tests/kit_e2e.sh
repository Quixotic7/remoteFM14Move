#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The kit (src/kit.c; rfm1cs-abletonmove's stage 3a, README "The kit") on the simulator, in the build image after make,
# through the simulator's command pipe (the Move's pads, steps, knobs, the jog, Sample) and the status:
#   - Track 4 held + its pad 3: the Kit view; a new project's tracks 13..16 routed to the kit;
#   - Files: the places, Samples (--samples-dir: a folder of the test's, its folders first), a folder entered, a sound
#     heard as the jog lands on it (the preview), its click: onto the chosen pad (loaded, in memory);
#   - the pad played: a voice; a step entered and Play: the sequencer plays the pad by itself; knob 1: its pitch;
#   - a pad held + Sample (the sampler's Manual mode): a take recorded into pad 2, the pad let go ends it, the take on
#     the pad;
#   - the output has the kit (the host's mix, --capture);
#   - a second start: the project's kit back (both pads loaded from their paths, the routing).
#   tests/kit_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "kit_e2e"
T=$(mktemp -d /tmp/rfm14kit.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; SMP=$T/Samples; LIB=$SMP/RemoteFM1; CAP=$T/cap
mkdir -p "$SIM" "$R" "$SMP/Drums" "$LIB" "$CAP"
echo "http://127.0.0.1:1" > "$R/catalog-url"
build/tools/ainchk make "$SMP/Drums/Tone.wav" >/dev/null || { echo "  FAIL  ainchk"; exit 1; }
cp "$SMP/Drums/Tone.wav" "$SMP/Drums/Zap.wav"
PORT=17839; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.05; _i=$((_i + 1)); done; }   # n detents, 1 / 127
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.15; }
set_() { jog 6 127; jog 1 1; jog "$1" 1; click; if [ "$2" -ge 0 ]; then jog "$2" 1; else jog $((-$2)) 127; fi; click; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 --takes-dir "$LIB" --samples-dir "$SMP" "$@" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && [ -n "$(field kit_cur)" ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }

# ---------------------------------------------------------------- the Kit view, Files, a sound onto a pad
start --capture "$CAP" --capture-seconds 120 || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # Track 4 held + its pad 3
wait_for 3 '[ "$(field kit_shown)" = 1 ]' && [ "$(field kit_route)" = 61440 ] && [ "$(field kit_cur)" = 0 ] \
    && ok "Track 4 held + pad 3: the Kit view (pad 1 chosen), tracks 13..16 routed to the kit" \
    || bad "the Kit view: shown '$(field kit_shown)' route '$(field kit_route)' pad '$(field kit_cur)'"
sleep 1.1; shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_rest.pbm
jog 1 127; click; jog 2 1; click; jog 1 1   # the tabs, Files, its places (Takes chosen)
jog 1 1; click                               # Samples
wait_for 3 '[ "$(field kit_place)" = 2 ] && [ "$(field kit_total)" = 2 ]' \
    && ok "Files, Samples: its two folders (Drums, RemoteFM1) listed" \
    || bad "Samples: place '$(field kit_place)' total '$(field kit_total)' rows answered '$(field kit_rows)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_samples.pbm
click                                        # Drums
wait_for 3 '[ "$(field kit_total)" = 2 ]' || bad "Drums: total '$(field kit_total)'"
jog 1 1                                      # Zap: heard
wait_for 3 '[ "$(field kit_prev)" = 1 ]' && ok "a sound heard as the jog lands on it (the preview)" \
    || bad "the preview: '$(field kit_prev)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_files.pbm
jog 1 127; click                             # Tone onto pad 1
wait_for 5 '[ "$(field kit_pads)" = 1 ]' && [ "$(field kit_bytes)" = $((88200 * 4)) ] \
    && ok "the click: Tone onto pad 1 (in memory: $(field kit_bytes) bytes)" \
    || bad "the load: pads '$(field kit_pads)' bytes '$(field kit_bytes)' loads '$(field kit_loads)'"

# ---------------------------------------------------------------- the pad, a step, Play, a knob
sleep 1.2; n0=$(field kit_notes)   # (the status: a few times a second)
cmd "note 68 110"; sleep 0.1; cmd "note 68 0"
wait_for 2 '[ "$(field kit_voices)" -ge 1 ]' && wait_for 2 '[ "$(field kit_notes)" -ge $((n0 + 2)) ]' \
    && ok "pad 1 played: a voice (a one shot: on after the pad's release)" \
    || bad "pad 1: voices '$(field kit_voices)' notes '$(field kit_notes)' (from $n0)"
cmd "note 16 127"; sleep 0.1; cmd "note 16 0"; sleep 0.2   # step 1: pad 1's note
n1=$(field kit_notes)
press 85; sleep 2.2; press 85; sleep 0.3                    # Play, two seconds, Stop
n2=$(field kit_notes)
[ "${n2:-0}" -ge $((n1 + 2)) ] && ok "a step and Play: the sequencer plays pad 1 ($((n2 - n1)) notes)" \
    || bad "the sequencer's notes: $n1 -> $n2"
cmd "cc 71 6"; sleep 0.2
wait_for 2 '[ "$(field kit_pitch)" = 300 ]' && ok "knob 1: pad 1's pitch +3" || bad "knob 1: pitch '$(field kit_pitch)'"
cmd "cc 71 122"; sleep 0.2                                  # (back to 0)

# ---------------------------------------------------------------- a pad held + Sample: a take into pad 2
press 118                                                   # (the sampler app: Manual mode)
wait_for 3 '[ "$(field smp_shown)" = 1 ]' || bad "the sampler: shown '$(field smp_shown)'"
set_ 1 2
wait_for 3 '[ "$(field rec_mode)" = 3 ]' || bad "Manual: mode '$(field rec_mode)'"
press 51; sleep 0.3
wait_for 2 '[ "$(field kit_shown)" = 1 ]' || bad "back on the kit: '$(field kit_shown)'"
cmd "note 69 110"; sleep 0.2; press 118; sleep 1.5; cmd "note 69 0"
wait_for 5 '[ "$(field rec_takes)" = 1 ]' && wait_for 5 '[ "$(field kit_pads)" = 3 ]' && [ "$(field kit_recs)" = 1 ] \
    && ok "pad 2 held + Sample: a take recorded into it, the pad let go ends it, the take on pad 2" \
    || bad "the take into pad 2: takes '$(field rec_takes)' pads '$(field kit_pads)' recs '$(field kit_recs)' rec '$(field kit_rec)'"
sleep 2.5   # (the project's autosave)
stop
mix=$(build/tools/ainchk rawpeak "$CAP/mix.raw"); xp=$(echo "$mix" | sed -n 's/.* peak \([0-9]*\) .*/\1/p')
[ "${xp:-0}" -gt 1000 ] && ok "the output has the kit (the mix's peak $xp)" || bad "the kit in the mix: $mix"

# ---------------------------------------------------------------- a second start: the project's kit back
start || { bad "the host did not start (2)"; tail -20 "$L"; exit 1; }
wait_for 8 '[ "$(field kit_pads)" = 3 ]' && [ "$(field kit_route)" = 61440 ] \
    && ok "a second start: the project's kit back (pads 1 and 2 loaded from their paths, the routing)" \
    || bad "the kit after a start: pads '$(field kit_pads)' route '$(field kit_route)' loads '$(field kit_loads)'"
stop
cp "$L" build/test-out/kit_host.log
echo "kit_e2e: $checks checks, $fail failed (the host's log: build/test-out/kit_host.log; screens build/test-out/kit_*.pbm)"
[ $fail = 0 ]
