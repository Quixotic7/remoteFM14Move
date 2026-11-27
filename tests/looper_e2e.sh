#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The looper (src/looper.c; rfm1cs-abletonmove's looper stage 1, docs/design/LOOPER-SPEC.md) on the simulator, in the
# build image after make, through the simulator's command pipe (the Move's pads, steps, knobs, buttons) and the status:
#   - Track 4 held + its pad 4: the Looper view; the round (Track 4's taps) reaches it from the Kit;
#   - Felucca played (a held note, the test API), Play, a tap on slot 1-1 (the top row's first pad): armed, recording
#     from the bar; the red pad tapped: it ends at the next bar and plays at once; its WAV in the project's own folder
#     (Kits/Projects/NN/Loops/1-1 <bpm>bpm <n>b.wav), whole bars long;
#   - the note let go: the loop still sounds (the mix's peak, the looper's lane);
#   - step 16: every lane stops at the bar; step 1 (scene 1): lane 1 plays again at the bar; a double click on step 16:
#     now;
#   - Track 4 held + the volume knob in the Looper view: the Loops strip's level (the Mixer's fifth);
#   - the sampler offers its Loops tab (and the Looper source);
#   - the project saved and duplicated (Copy + project 1, an empty pad): the copy's folder holds Loops/ too;
#   - a second start: the project's loop back in slot 1-1 (loaded from its file).
#   tests/looper_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "looper_e2e"
T=$(mktemp -d /tmp/rfm14loop.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; SMP=$T/Samples; LIB=$SMP/RemoteFM1; CAP=$T/cap
mkdir -p "$SIM" "$R" "$SMP" "$LIB" "$CAP"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17849; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
arr() { status | sed -n "s/.*\"$1\":\[\([^]]*\)\].*/\1/p" | head -1; }   # a status array, as "a,b,c,d"
nth() { echo "$1" | cut -d, -f"$2"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
midi() { curl -s -o /dev/null -X POST --data "{\"track\":0,\"b0\":$1,\"b1\":$2,\"b2\":$3}" "$U/api/test/midi"; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 --takes-dir "$LIB" --samples-dir "$SMP" --fx-dir build/fxmods "$@" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && [ -n "$(field loop_cur)" ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }

# ---------------------------------------------------------------- the Looper view
start --capture "$CAP" --capture-seconds 120 || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5
cmd "cc 40 127"; sleep 0.4; hit 95; cmd "cc 40 0"; sleep 0.3   # Track 4 held + its pad 4
wait_for 3 '[ "$(field loop_shown)" = 1 ]' && ok "Track 4 held + pad 4: the Looper view" \
    || bad "the Looper view: shown '$(field loop_shown)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/loop_rest.pbm
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # (the Kit view)
press 40; sleep 0.5                                            # (Track 4's tap from the Kit: the Looper)
wait_for 3 '[ "$(field loop_shown)" = 1 ]' && ok "Track 4's round: Kit, then Looper" \
    || bad "the round to the Looper: shown '$(field loop_shown)' kit '$(field kit_shown)'"
tabs=$(field smp_tabs)
[ -n "$tabs" ] && [ $((tabs & 8)) = 8 ] && ok "the sampler offers its Loops tab (tabs $tabs)" || bad "the Loops tab offered: tabs '$tabs'"

# ---------------------------------------------------------------- a recording at the bar, it plays at once
midi 144 60 110; midi 144 67 100; sleep 0.5                    # (Felucca sounding: the Played source)
press 85; sleep 0.3                                            # Play
wait_for 3 '[ "$(field seq_playing)" = 1 ]' || bad "Play: seq_playing '$(field seq_playing)'"
hit 92                                                         # (slot 1-1: the top row's first pad)
wait_for 4 '[ "$(field loop_rec)" = 0 ] && [ "$(field loop_rec_state)" = 2 ]' \
    && ok "a tap on slot 1-1: recording from the bar (state $(field loop_rec_state))" \
    || bad "the recording: rec '$(field loop_rec)' state '$(field loop_rec_state)' recs '$(field loop_recs)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/loop_recording.pbm
wait_for 6 '[ "$(field loop_rec_bar)" -ge 2 ]' || bad "the second bar: bar '$(field loop_rec_bar)'"
hit 92                                                         # (the red pad: it ends at the next bar)
wait_for 6 '[ "$(field loop_done)" -ge 1 ] && [ "$(nth "$(arr loop_play)" 1)" = 0 ]' \
    && ok "the red pad: it ended at the bar and plays at once (lane 1 plays scene 1)" \
    || bad "the end: done '$(field loop_done)' play '$(arr loop_play)' rec '$(field loop_rec)' slots '$(field loop_slots)'"
P1="$LIB/Kits/Projects/$(printf %02d $(($(field proj_cur) + 1)))"
F=$(ls "$P1/Loops/" 2>/dev/null | grep '^1-1 .*bpm .*b\.wav$' | head -1)
if [ -n "$F" ]; then
    bars=$(echo "$F" | sed -n 's/.* \([0-9]*\)b\.wav$/\1/p'); bpm=$(echo "$F" | sed -n 's/.* \([0-9]*\)bpm .*/\1/p')
    size=$(wc -c < "$P1/Loops/$F" | tr -d ' ')
    want=$(awk -v b="$bars" -v t="$bpm" 'BEGIN { printf "%d", 44 + 4 * int(b * 4 * 60 / t * 44100 + 0.5) }')
    d=$((size - want)); [ $d -lt 0 ] && d=$((-d))
    [ "$bars" -ge 2 ] && [ $d -le 8 ] && [ "$(field loop_file)" = "$F" ] \
        && ok "its WAV in the project's folder: Loops/$F ($bars bars, $size bytes: whole bars at $bpm BPM)" \
        || bad "the WAV: $F bars '$bars' size $size want $want file '$(field loop_file)'"
else
    bad "no WAV in $P1/Loops: $(ls -R "$LIB/Kits" 2>&1 | tr '\n' ' ')"
fi
midi 128 60 0; midi 128 67 0; sleep 1.5                        # (Felucca let go: the loop alone)
wait_for 4 '[ "$(field loop_peak)" -gt 500 ] && [ "$(field peak)" -gt 500 ]' \
    && ok "the note let go: the loop sounds through the mix (lane peak $(field loop_peak), the mix's $(field peak))" \
    || bad "the loop heard: lane peak '$(field loop_peak)' mix '$(field peak)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/loop_playing.pbm

# ---------------------------------------------------------------- step 16, a scene, the strip
hit 31                                                         # (step 16: stop all, at the bar)
wait_for 2 '[ "$(nth "$(arr loop_next)" 1)" = 8 ] || [ "$(nth "$(arr loop_play)" 1)" = -1 ]' \
    && ok "step 16: lane 1's stop waits for the bar (next '$(nth "$(arr loop_next)" 1)'; a bar line close by: already stopped)" \
    || bad "step 16: next '$(arr loop_next)' play '$(arr loop_play)'"
wait_for 6 '[ "$(nth "$(arr loop_play)" 1)" = -1 ]' && ok "at the bar: stopped" || bad "stop all: play '$(arr loop_play)'"
hit 16                                                         # (step 1: scene 1, at the bar)
wait_for 6 '[ "$(nth "$(arr loop_play)" 1)" = 0 ]' && ok "step 1: scene 1 plays again" || bad "scene 1: play '$(arr loop_play)'"
cmd "note 31 100"; cmd "note 31 0"; cmd "note 31 100"; cmd "note 31 0"   # (a double click: now)
wait_for 1 '[ "$(nth "$(arr loop_play)" 1)" = -1 ]' && ok "step 16 double clicked: stopped now" \
    || bad "the double click: play '$(arr loop_play)' next '$(arr loop_next)'"
hit 92; wait_for 6 '[ "$(nth "$(arr loop_play)" 1)" = 0 ]' || bad "the loop again: play '$(arr loop_play)'"
m0=$(field loop_mix)
cmd "cc 40 127"; sleep 0.4; cmd "cc 79 120"; sleep 0.1; cmd "cc 79 120"; sleep 0.1; cmd "cc 40 0"; sleep 0.3
wait_for 3 '[ "$(field loop_mix)" -lt "$m0" ]' && ok "Track 4 held + the volume knob: the Loops strip ($m0 -> $(field loop_mix))" \
    || bad "the strip's level: $m0 -> '$(field loop_mix)'"
press 85; sleep 0.5                                            # (stopped: the loop silent, kept)

# ---------------------------------------------------------------- the project: saved, duplicated, a second start
sleep 2.5                                                      # (the project's autosave)
cmd "cc 40 127"; sleep 0.4; hit 96; cmd "cc 40 0"; sleep 0.4   # (Track 4 held + pad 5: the projects)
wait_for 3 '[ "$(field projects)" = 1 ]' || bad "the projects view: '$(field projects)'"
cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.3; hit 94; sleep 0.2; cmd "cc 60 0"
P3="$LIB/Kits/Projects/03"
wait_for 8 '[ -n "$(ls "$P3/Loops/" 2>/dev/null)" ]' && cmp -s "$P1/Loops/$F" "$P3/Loops/$F" \
    && ok "Copy + project 1, then an empty pad: project 3's folder has Loops/$F too" \
    || bad "the duplicate's Loops: $(ls -R "$LIB/Kits/Projects" 2>&1 | tr '\n' ' ')"
press 51; sleep 0.3
stop
start || { bad "the host did not start (2)"; tail -20 "$L"; exit 1; }
wait_for 8 '[ "$(field loop_slots)" = 1 ] && [ "$(field loop_bytes)" -gt 0 ]' \
    && ok "a second start: the project's loop back in slot 1-1 (loaded: $(field loop_bytes) bytes)" \
    || bad "after a start: slots '$(field loop_slots)' bytes '$(field loop_bytes)' loads '$(field loop_loads)'"
stop
cp "$L" build/test-out/looper_host.log
echo "looper_e2e: $checks checks, $fail failed (the host's log: build/test-out/looper_host.log; screens build/test-out/loop_*.pbm)"
[ $fail = 0 ]
