#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The kit (src/kit.c; rfm1cs-abletonmove's stage 3a, README "The kit") on the simulator, in the build image after make,
# through the simulator's command pipe (the Move's pads, steps, knobs, the jog, Sample) and the status:
#   - Track 4 held + its pad 3: the Kit view; a new project's tracks 13..16 routed to the kit;
#   - Files: the root (2026-11-26: My Samples, the Move Library: --core-library a folder of the test's, its folder
#     without sounds hidden; a sound there kept: copied into the project's folder, Files again on it there), My Samples
#     (--samples-dir: a folder of the test's, its folders first), a folder entered; (round A)
#     the jog at rest on a sound puts it on the chosen pad, heard (the pad's voice, the output's peak), the jog back:
#     the other, its click keeps it (loaded, in memory) and closes Files; Files again: on the pad's sound
#     (KIT_FIND: a take's, in My Samples' RemoteFM1);
#   - the pad played: a voice; a step entered and Play: the sequencer plays the pad by itself; (pages v2) Pad Sample's
#     knob 3: its Transpose;
#   - a pad held + Sample (the sampler's Manual mode): a take recorded into pad 2, the pad let go ends it, the take on
#     the pad;
#   - (stage 3b; pages v2) Warp Repitch, Sync on the Warp page's knob 2: the pad's rate fitted to its steps at the tempo,
#     again when the tempo halves;
#   - a pad's Send 1 (pages v2: Pad Mix's knob 5) into audio FX 1 (freeverb: its return sounds);
#   - the Mixer's fourth strip, the kit's (knob 4: its level);
#   - Shift + the click on a folder: All To Pads (its three sounds on pads 1..3), Undo: the kit back;
#   - Save (Kit 1: its folder, kit.json), a new project (an empty kit), Load: Kit 1 there;
#   - (stage 3c; the owner's 2026-11-26 Copy) Copy held, the jog's turn: Mix, pad 1 + pad 2: done at once (a new
#     file in the project's folder, Kits/Projects/NN, on pad 2), Undo; Copy held + the jog + only the source: nothing,
#     its next press on Copy; Join (its length the two's) (the owner's follow-up: the pad's tools gone, their Normalize with
#     them); a take's To Kit then Slice: its hits, Equal 8, Shift + pad 1, a slice heard, the click: 8 slices on pads
#     1..8 (one file in memory once), Undo;
#   - the sampler's Kit source and To Kit: a take of the kit alone, onto the next empty pad;
#   - the output has the kit (the host's mix, --capture);
#   - (round A) a take recorded into pad 4 with silence first: Trim silence moves its start to just before the sound;
#     (pages v2: Toggle gone) the Loop engine (the page's set): held, looping past the sound's end, let go, off;
#   - a second start: the project's kit back (its pads loaded from their paths, the routing, its name).
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
SIM=$T/sim; R=$T/root; SMP=$T/Samples; LIB=$SMP/RemoteFM1; CAP=$T/cap; CORE=$T/CoreLibrary
mkdir -p "$SIM" "$R" "$SMP/Drums" "$SMP/Kit808" "$LIB" "$CAP" "$CORE/Drums/Kicks" "$CORE/Keys/Bells" "$CORE/Presets"
echo "http://127.0.0.1:1" > "$R/catalog-url"
build/tools/ainchk make "$SMP/Drums/Tone.wav" >/dev/null || { echo "  FAIL  ainchk"; exit 1; }
cp "$SMP/Drums/Tone.wav" "$SMP/Drums/Zap.wav"
for f in c b a; do cp "$SMP/Drums/Tone.wav" "$SMP/Kit808/$f.wav"; done   # (3b: a folder for All To Pads)
# (2026-11-26: the Move Library: two categories, a sound in each kind's folder (a WAV, a FLAC), a broken FLAC beside the
# good one (its "unsupported" row), a folder without sounds)
cp "$SMP/Drums/Tone.wav" "$CORE/Drums/Kicks/Hit.wav"; cp tests/flac/tone.flac "$CORE/Keys/Bells/Bell.flac"
echo x > "$CORE/Keys/Bells/Junk.flac"; echo '{}' > "$CORE/Presets/Kit.ablpreset"
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
        --test-api --no-fm1 --takes-dir "$LIB" --samples-dir "$SMP" --core-library "$CORE" --fx-dir build/fxmods "$@" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && [ -n "$(field kit_cur)" ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
edit() { curl -s -o /dev/null -X POST --data "{\"op\":$1,\"a\":$2,\"b\":$3}" "$U/api/edit"; }   # (the page's ops)
twice() { awk -v x="$1" 'BEGIN { print x * 2 }'; }
near() { awk -v a="$1" -v b="$2" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(b != 0 && d / b < 0.01) }'; }

# ---------------------------------------------------------------- the Kit view, Files, a sound onto a pad
start --capture "$CAP" --capture-seconds 120 || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # Track 4 held + its pad 3
wait_for 3 '[ "$(field kit_shown)" = 1 ]' && [ "$(field kit_route)" = 61440 ] && [ "$(field kit_cur)" = 0 ] \
    && ok "Track 4 held + pad 3: the Kit view (pad 1 chosen), tracks 13..16 routed to the kit" \
    || bad "the Kit view: shown '$(field kit_shown)' route '$(field kit_route)' pad '$(field kit_cur)'"
sleep 1.1; shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_rest.pbm
jog 1 1; click                               # (pages) Pad Sample; the click: the browser's root (My Samples chosen)
jog 1 1; click                               # (2026-11-26) the Move Library
wait_for 5 '[ "$(field kit_place)" = 4 ] && [ "$(field kit_total)" = 2 ]' \
    && ok "the root's Move Library: its categories with sounds (Drums, Keys; Presets hidden)" \
    || bad "the Move Library: place '$(field kit_place)' total '$(field kit_total)'"
[ "$(field cl_sounds)" = 2 ] && [ "$(field cl_unsupported)" = 1 ] && [ "$(field cl_root)" = "$CORE" ] \
    && grep -q "kit: core library $CORE (root: .): 2 sounds in 4 folders (1 .wav, 1 .flac\|kit: core library $CORE (root: .): 2 sounds in 4 folders (1 .flac, 1 .wav" "$L" \
    && ok "the index in the status (kit_corelib: 2 sounds, 1 unsupported) and the log ($(grep 'kit: core library' "$L" | tail -1 | cut -c1-150))" \
    || bad "the index: sounds '$(field cl_sounds)' unsupported '$(field cl_unsupported)' root '$(field cl_root)' $(grep 'kit: core library' "$L" | tail -1)"
jog 1 1; click                               # Keys: Bells
wait_for 3 '[ "$(field kit_total)" = 1 ]' || bad "Keys: total '$(field kit_total)'"
click                                        # Bells: Bell (the FLAC), "1 .flac (unsupported)" (Junk)
wait_for 3 '[ "$(field kit_total)" = 2 ] && [ "$(field kit_depth)" = 2 ]' \
    && ok "Keys, Bells: nested categories, the FLAC and the broken one's unsupported row" \
    || bad "Bells: total '$(field kit_total)' depth '$(field kit_depth)'"
jog 1 1; sleep 0.6; jog 1 127                # (the dead row: nothing; back on Bell: on pad 1)
wait_for 5 '[ "$(field kit_file)" = Bell.flac ] && [ "$(field kit_frames)" = 12000 ]' \
    && ok "the jog at rest on a FLAC (Bell): on pad 1 (12000 frames), the unsupported row passed over" \
    || bad "Bell.flac: file '$(field kit_file)' frames '$(field kit_frames)'"
sleep 0.3; hit 68; wait_for 3 '[ "$(field peak)" -gt 1000 ]' && ok "pad 1 pressed: the FLAC heard (the mix's peak $(field peak))" \
    || bad "the FLAC heard: peak '$(field peak)'"
press 51; press 51; press 51; sleep 0.2      # (Back: Keys, the place's top, the root)
jog 1 127; click                             # My Samples
wait_for 3 '[ "$(field kit_place)" = 2 ] && [ "$(field kit_total)" = 3 ]' \
    && ok "Files, Samples: its folders (Drums, Kit808, RemoteFM1) listed" \
    || bad "Samples: place '$(field kit_place)' total '$(field kit_total)' rows answered '$(field kit_rows)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_samples.pbm
click                                        # Drums
wait_for 3 '[ "$(field kit_total)" = 2 ]' || bad "Drums: total '$(field kit_total)'"
sleep 1.2; v0=$(field kit_notes)
jog 1 1                                      # Zap: at rest, on pad 1, heard (round A)
wait_for 5 '[ "$(field kit_file)" = Zap.wav ] && [ "$(field kit_pads)" = 1 ]' \
    && sleep 0.5 && [ "$(field kit_notes)" = "$v0" ] \
    && ok "the jog at rest on Zap: on pad 1 at once, not played (the pad plays it; notes $(field kit_notes), from $v0)" \
    || bad "the landing: file '$(field kit_file)' pads '$(field kit_pads)' notes '$(field kit_notes)' (from $v0) prev '$(field kit_prev)'"
hit 68; wait_for 3 '[ "$(field peak)" -gt 1000 ]' && ok "pad 1 pressed: Zap heard (the mix's peak $(field peak))" \
    || bad "the landing heard: peak '$(field peak)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_files.pbm
l0=$(field kit_loads)
cmd "cc 14 127"; cmd "cc 14 1"; cmd "cc 14 127"             # (a quick turn and back, 60 ms apart: Tone, one load)
wait_for 5 '[ "$(field kit_file)" = Tone.wav ]' && sleep 0.5 && [ "$(field kit_loads)" = $((l0 + 1)) ] \
    && ok "a quick turn back to Tone: one load ($(field kit_loads), from $l0)" \
    || bad "the turn back: file '$(field kit_file)' loads '$(field kit_loads)' (from $l0)"
click                                        # kept: Files closed (the page)
wait_for 5 '[ "$(field kit_tab)" = 0 ] && [ "$(field kit_pads)" = 1 ]' && [ "$(field kit_bytes)" = $((88200 * 4)) ] \
    && [ "$(field kit_loads)" = $((l0 + 1)) ] \
    && ok "the click keeps Tone on pad 1 (in memory: $(field kit_bytes) bytes), Files closed" \
    || bad "the click: tab '$(field kit_tab)' pads '$(field kit_pads)' bytes '$(field kit_bytes)' loads '$(field kit_loads)'"

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
cmd "cc 73 6"; sleep 0.2                                    # (pages v2: Pad Sample's knob 3, Transpose)
wait_for 2 '[ "$(field kit_pitch)" = 300 ]' && ok "Pad Sample's knob 3: pad 1's Transpose +3" || bad "knob 3: pitch '$(field kit_pitch)'"
cmd "cc 73 122"; sleep 0.2                                  # (back to 0)

# ---------------------------------------------------------------- (stage 3b) Stretch: the Stretch page's knob 1, the tempo
cmd "note 68 100"; sleep 0.1; cmd "note 68 0"; sleep 0.2      # (pad 1 chosen)
edit 30 33 1; sleep 0.2                                     # (pages v2: pad 1's Warp Repitch, the page's set)
jog 3 1; cmd "cc 72 12"; sleep 0.3                          # (the Warp page's Sync) 4 steps (3 ticks a step): 4 x 125 ms at 120 BPM
wait_for 3 '[ "$(field kit_stretch)" = 4 ]'
r1=$(field kit_rate)
[ "$(field kit_stretch)" = 4 ] && near "$r1" 4 && ok "the Stretch page's knob 1: Stretch 4 steps, Tone (88200 frames) at $r1 a frame (half a second)" \
    || bad "Stretch: '$(field kit_stretch)' rate '$r1' page '$(field kit_page)'"
cmd "cc 49 127"; hit 20; cmd "cc 49 0"; sleep 0.2            # Shift + step 5: Tempo
cmd "cc 71 68"; sleep 0.3; press 51; sleep 0.3               # knob 1: -60 BPM
wait_for 3 'near "$(twice "$(field kit_rate)")" "$r1"' \
    && ok "the tempo halved (60 BPM): the stretch re-fitted ($r1 -> $(field kit_rate))" \
    || bad "the tempo: rate '$r1' -> '$(field kit_rate)'"
cmd "cc 49 127"; hit 20; cmd "cc 49 0"; sleep 0.2; cmd "cc 71 60"; sleep 0.3; press 51; sleep 0.3   # (120 again)
cmd "cc 72 116"; sleep 0.3                                  # (Sync Off)
wait_for 3 '[ "$(field kit_stretch)" = 0 ]' || bad "Sync Off: '$(field kit_stretch)'"
jog 3 127; edit 30 33 0; sleep 0.2                          # (Pad Sample again, Warp Off)

# ---------------------------------------------------------------- a pad's send into audio FX 1
edit 20 4 2                                                 # (freeverb on audio FX 1)
wait_for 5 '[ "$(field afx0)" = freeverb ]' || bad "freeverb: '$(field afx0)'"
cmd "note 68 100"; sleep 0.1; cmd "note 68 0"; sleep 2.2
s0=$(field rpeak0)
jog 5 1; cmd "cc 75 60"; sleep 0.3                          # (pages v2) Pad Mix, knob 5: Send 1 60 %
cmd "note 68 100"; sleep 0.1; cmd "note 68 0"; sleep 2.2
s1=$(field rpeak0)
[ "${s0:-1}" = 0 ] && [ "${s1:-0}" -gt 500 ] && ok "Pad Mix's knob 5: pad 1's Send 1 into freeverb (its return $s0 -> $s1)" \
    || bad "the send: return $s0 -> $s1"
cmd "cc 75 68"; sleep 0.2; jog 5 127; sleep 1.1            # (Send 1 0; Pad Sample again)

# ---------------------------------------------------------------- the Mixer's fourth strip
m0=$(field kit_mix)
cmd "cc 40 127"; cmd "cc 40 0"; sleep 0.6                    # Track 4: Kit -> the Mixer
wait_for 3 '[ "$(field kit_shown)" = 0 ]' || bad "the Mixer: kit shown '$(field kit_shown)'"
cmd "cc 74 116"; sleep 0.4                                  # knob 4: the kit's level 12 detents down (rfm1cs
                                                            # 2026-11-26: 0.5 dB a detent, 0 -> -6 dB: 100 -> 50)
wait_for 3 '[ "$(field kit_level)" = 50 ]' && [ "$(field kit_mix)" -gt "${m0:-0}" ] \
    && ok "the Mixer: knob 4 the kit's strip (its level 100 -> 50, MIX slot 3 to the host)" \
    || bad "the kit's strip: level '$(field kit_level)' mix '$(field kit_mix)' (from $m0)"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_mixer.pbm
cmd "cc 74 50"; sleep 0.3
cmd "cc 40 127"; cmd "cc 40 0"; sleep 0.6; cmd "cc 40 127"; cmd "cc 40 0"; sleep 0.6   # Routing, the Kit
wait_for 3 '[ "$(field kit_shown)" = 1 ]' || bad "back to the Kit: '$(field kit_shown)'"

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

# ---------------------------------------------------------------- All To Pads; Undo
f0=$(field kit_finds)
click                                                       # (Files: on pad 2's take, round A)
wait_for 5 '[ "$(field kit_finds)" = $((f0 + 1)) ] && [ "$(field kit_place)" = 2 ]' \
    && ok "Files again: on pad 2's sound (KIT_FIND: the take, in My Samples' RemoteFM1)" \
    || bad "Files on the pad's sound: finds '$(field kit_finds)' (from $f0) place '$(field kit_place)'"
press 51; press 51; sleep 0.3                               # (Back: RemoteFM1, My Samples' top)
wait_for 3 '[ "$(field kit_total)" = 3 ]' || bad "My Samples again: '$(field kit_total)'"
jog 1 127                                                   # Kit808 (from RemoteFM1)
cmd "cc 49 127"; click; cmd "cc 49 0"; sleep 0.2            # Shift + the click: its actions
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_folder.pbm
click                                                       # All To Pads
wait_for 5 '[ "$(field kit_pads)" = 7 ]' && ok "Shift + the click on Kit808: All To Pads (a, b, c on pads 1..3, the rest emptied)" \
    || bad "All To Pads: pads '$(field kit_pads)'"
press 56
wait_for 5 '[ "$(field kit_pads)" = 3 ]' && ok "Undo: the kit back (Tone, the take)" || bad "Undo: pads '$(field kit_pads)'"

# ---------------------------------------------------------------- Save; a new project; Load
press 51; press 51; sleep 0.2; jog 1 127; click; jog 1 1  # (pages) the browser left; Sample Kit's click: Kits, Save
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_kittab.pbm
click
wait_for 8 '[ "$(field kit_saved)" = 1 ]' && [ "$(field kit_saved_name)" = "Kit 1" ] && [ -s "$LIB/Kits/Kit 1/kit.json" ] \
    && grep -q '"file": "Tone.wav"' "$LIB/Kits/Kit 1/kit.json" && [ -s "$LIB/Kits/Kit 1/Tone.wav" ] \
    && ok "Save: Kit 1 (Samples/RemoteFM1/Kits/Kit 1: its sounds and kit.json; \"Saved / Kit 1\")" \
    || bad "Save: saved '$(field kit_saved)' '$(field kit_saved_name)': $(ls "$LIB/Kits" "$LIB/Kits/Kit 1" 2>&1 | tr '\n' ' ')"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_saved.pbm
sleep 2.5                                                   # (the project's autosave)
cmd "cc 40 127"; sleep 0.4; hit 96; cmd "cc 40 0"; sleep 0.5   # Track 4 held + pad 5: the projects
hit 28                                                      # step 13: a new project
wait_for 15 '[ "$(field proj_cur)" = 1 ] && [ "$(field pfw_loading)" = 0 ]' && wait_for 5 '[ "$(field kit_pads)" = 0 ]' \
    && ok "a new project: an empty kit" || bad "the new project: cur '$(field proj_cur)' pads '$(field kit_pads)'"
press 51; sleep 0.3
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.5   # the Kit again
wait_for 3 '[ "$(field kit_shown)" = 1 ]' || bad "the Kit view: '$(field kit_shown)'"
jog 1 127; click                                            # Load: Files' Kits
wait_for 3 '[ "$(field kit_place)" = 3 ] && [ "$(field kit_total)" = 1 ]' || bad "Kits: place '$(field kit_place)' total '$(field kit_total)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_kits.pbm
click
wait_for 8 '[ "$(field kit_kits)" = 1 ] && [ "$(field kit_pads)" = 3 ]' && [ "$(field kit_name)" = "Kit 1" ] \
    && ok "Load: Kit 1 in the new project (its two sounds from its folder, its name)" \
    || bad "Load: kits '$(field kit_kits)' pads '$(field kit_pads)' name '$(field kit_name)'"

# ---------------------------------------------------------------- (stage 3c) mix and join, a pad's own edit, slicing
frames() { build/tools/ainchk info "$1" | sed -n 's/^frames \([0-9]*\) .*/\1/p'; }
peak() { build/tools/ainchk info "$1" | sed -n 's/.* peak \([0-9]*\)$/\1/p'; }
ED="$LIB/Kits/Projects/$(printf %02d $(($(field proj_cur) + 1)))"   # (round C: the project's own folder)
m0=$(field kit_made)
cmd "cc 60 127"; jog 1 1; hit 68; hit 69; cmd "cc 60 0"     # Copy held, the jog: Mix, pad 1, pad 2: done
wait_for 8 '[ "$(field kit_file)" = "Mix 1+2.wav" ]' && [ -s "$ED/Mix 1+2.wav" ] && [ "$(field kit_cur)" = 1 ] \
    && [ "$(peak "$ED/Mix 1+2.wav")" -le 32767 ] \
    && ok "Copy held, the jog's Mix, pad 1, pad 2: at once a new file on pad 2 (Kits/Projects/NN/Mix 1+2.wav, peak $(peak "$ED/Mix 1+2.wav"))" \
    || bad "Mix: file '$(field kit_file)' cur '$(field kit_cur)' made '$(field kit_made)' (from $m0): $(ls "$ED" 2>&1 | tr '\n' ' ')"
sleep 1.2; press 56
wait_for 5 '[ "$(field kit_file)" != "Mix 1+2.wav" ] && [ -n "$(field kit_file)" ]' \
    && ok "Undo: pad 2 back ($(field kit_file))" || bad "Undo of the mix: '$(field kit_file)'"
f2=$(field kit_frames)
cmd "cc 60 127"; jog 2 1; hit 68
wait_for 3 '[ "$(field kit_mg)" = 1 ]' || bad "the Copy modes, the source: '$(field kit_mg)'"
sleep 0.2; shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_merge.pbm
cmd "cc 60 0"                                                # (Copy let go with only the source: nothing)
wait_for 3 '[ "$(field kit_mg)" = 0 ]' && sleep 0.5 && [ "$(field kit_made)" = $((m0 + 1)) ] && ok "Copy let go with only the source: nothing done" \
    || bad "Copy let go: mg '$(field kit_mg)' made '$(field kit_made)'"
cmd "cc 60 127"; jog 2 1; hit 68; hit 69; cmd "cc 60 0"     # Join (the press on Copy again: two detents)
wait_for 8 '[ "$(field kit_file)" = "Join 1+2.wav" ]' && [ "$(frames "$ED/Join 1+2.wav")" = $((88200 + f2)) ] \
    && ok "Join: pad 1 then pad 2 on pad 2 (88200 + $f2 = $(frames "$ED/Join 1+2.wav") frames)" \
    || bad "Join: file '$(field kit_file)' frames $(frames "$ED/Join 1+2.wav" 2>/dev/null) (want $((88200 + f2)))"
sleep 1.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_joined.pbm
# a take's To Kit, then Slice: the sampler's Takes, the take's actions, To Kit (the seventh), Slice
tk=$(ls -t "$LIB"/2*/*.wav 2>/dev/null | head -1); tf=$(frames "$tk")
press 118; sleep 0.4
jog 14 127; click; jog 3 127; jog 1 1; click; jog 1 1; sleep 0.4   # the Takes tab, its first row
click; wait_for 3 '[ "$(field smp_scr)" = 1 ]' || bad "the take's actions: scr '$(field smp_scr)'"
jog 6 1; click
wait_for 3 '[ "$(field kit_shown)" = 1 ]' || bad "To Kit: the Kit view '$(field kit_shown)'"
jog 1 1; click                                               # Slice
wait_for 8 '[ "$(field kit_sl_on)" = 1 ] && [ "$(field kit_sl_n)" -ge 1 ]' \
    && ok "To Kit, Slice: the take's cuts by its hits ($(field kit_sl_n) slices)" \
    || bad "Slice: on '$(field kit_sl_on)' n '$(field kit_sl_n)' answers '$(field kit_slices)'"
jog 2 1                                                      # Equal 8
wait_for 8 '[ "$(field kit_sl_n)" = 8 ]' || bad "Equal 8: n '$(field kit_sl_n)'"
cmd "cc 49 127"; hit 68; cmd "cc 49 0"; sleep 0.2             # Shift + pad 1: from pad 1
p0=$(field kit_slplay); hit 70                               # pad 3: slice 3 heard
wait_for 3 '[ "$(field kit_slplay)" = $((p0 + 1)) ]' && [ "$(field kit_sl_first)" = 0 ] \
    && ok "Equal 8; Shift + pad 1 the first pad; pad 3 plays slice 3 (a kit voice on the kit's bus)" \
    || bad "the slices' pads: play '$(field kit_slplay)' (from $p0) first '$(field kit_sl_first)'"
sleep 0.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/kit_slice.pbm
click
wait_for 8 '[ "$(field kit_pads)" = 255 ] && [ "$(field kit_sl_on)" = 0 ]' && [ "$(field kit_bytes)" = $((tf * 4)) ] \
    && [ "$(field kit_file)" = "$(basename "$tk")" ] && [ "$(field kit_start)" = 0 ] && [ "$(field kit_end)" -lt 4200 ] \
    && ok "the click: 8 slices on pads 1..8, one file in memory once ($(field kit_bytes) bytes), pad 1 its first region" \
    || bad "the slices onto pads: pads '$(field kit_pads)' bytes '$(field kit_bytes)' (want $((tf * 4))) file '$(field kit_file)' end '$(field kit_end)'"
sleep 1.2; press 56
wait_for 5 '[ "$(field kit_pads)" = 3 ]' && [ "$(field kit_name)" = "Kit 1" ] && ok "Undo: the kit back (pads 1 and 2, Kit 1)" \
    || bad "Undo of the slices: pads '$(field kit_pads)' name '$(field kit_name)'"

# ---------------------------------------------------------------- the sampler: Source Kit, To Kit
t0=$(field rec_takes)
edit 26 0 7; edit 26 6 2; edit 26 1 3; sleep 0.3            # (the page's Sampler window: Kit, To Kit, Manual)
edit 27 0 0; sleep 0.3                                      # record
cmd "note 68 110"; sleep 0.8; cmd "note 68 0"; sleep 0.3
edit 27 0 0                                                 # .. stop: the take
wait_for 8 '[ "$(field rec_takes)" = $((t0 + 1)) ]' && wait_for 8 '[ "$(field kit_pads)" = 7 ] && [ "$(field kit_to)" = 1 ]' \
    && ok "Source Kit, To Kit: the take onto the next empty pad (pad 3)" \
    || bad "To Kit: takes '$(field rec_takes)' (from $t0) pads '$(field kit_pads)' to '$(field kit_to)'"
tk=$(ls -t "$LIB"/*/*Kit*.wav 2>/dev/null | head -1)
info=$(build/tools/ainchk info "$tk" 2>/dev/null); pk=$(echo "$info" | sed -n 's/.* peak \([0-9]*\).*/\1/p')
[ -n "$tk" ] && [ "${pk:-0}" -gt 1000 ] && ok "the take is the kit's output alone ($(basename "$tk"): $info)" \
    || bad "the Kit take: '$tk' $info"
# ---------------------------------------------------------------- (round A) Trim silence; Toggle
press 51; sleep 0.3
wait_for 3 '[ "$(field kit_shown)" = 1 ]' || bad "back on the kit: '$(field kit_shown)'"
edit 26 0 0; sleep 0.3                                      # (the sampler's Source: Master again)
t1=$(field kit_trims)
cmd "note 71 110"; sleep 0.2; press 118; sleep 1.0          # pad 4 held + Sample: a second of silence
edit 28 0 110; sleep 0.8; edit 28 0 0; sleep 0.3; cmd "note 71 0"   # .. then pad 1's Tone
wait_for 8 '[ "$(field kit_trims)" = $((t1 + 1)) ] && [ "$(field kit_pads)" = 15 ]' && [ "$(field kit_cur)" = 3 ] \
    && [ "$(field kit_lead)" -gt 30000 ] && [ "$(field kit_start)" -gt 0 ] && [ "$(field kit_trim)" = 1 ] \
    && ok "a take into pad 4, silence first: Trim silence's start at frame $(field kit_lead) (Start $(field kit_start))" \
    || bad "Trim silence: trims '$(field kit_trims)' (from $t1) pads '$(field kit_pads)' cur '$(field kit_cur)' lead '$(field kit_lead)' start '$(field kit_start)'"
edit 30 3 2; sleep 0.3                                      # (pages v2: pad 1's engine Loop, the page's set)
cmd "cc 49 127"; hit 68; cmd "cc 49 0"; sleep 0.3           # (Shift + pad 1: chosen)
[ "$(field kit_mode)" = 2 ] || bad "Loop: mode '$(field kit_mode)'"
cmd "note 68 110"; sleep 2.6                                # held: past Tone's 1.84 s (it loops)
v1=$(field kit_voices)
cmd "note 68 0"                                             # let go: off
wait_for 2 '[ "$(field kit_voices)" = 0 ]' && [ "${v1:-0}" -ge 1 ] \
    && ok "the Loop engine: held, still sounding after 2.6 s ($v1), let go: off" \
    || bad "Loop: voices after 2.6 s '$v1', now '$(field kit_voices)'"
edit 30 3 0; sleep 0.3
sleep 2.5   # (the project's autosave)
stop
mix=$(build/tools/ainchk rawpeak "$CAP/mix.raw"); xp=$(echo "$mix" | sed -n 's/.* peak \([0-9]*\) .*/\1/p')
[ "${xp:-0}" -gt 1000 ] && ok "the output has the kit (the mix's peak $xp)" || bad "the kit in the mix: $mix"

# ---------------------------------------------------------------- a second start: the project's kit back
start || { bad "the host did not start (2)"; tail -20 "$L"; exit 1; }
wait_for 8 '[ "$(field kit_pads)" = 15 ]' && [ "$(field kit_route)" = 61440 ] && [ "$(field kit_name)" = "Kit 1" ] \
    && ok "a second start: the project's kit back (pads 1..4 loaded from their paths, the routing, its name)" \
    || bad "the kit after a start: pads '$(field kit_pads)' route '$(field kit_route)' name '$(field kit_name)' loads '$(field kit_loads)'"
# (2026-11-26) a Move Library sound onto pad 5, kept: copied into the project's folder; Files again on it there
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # the Kit view
wait_for 3 '[ "$(field kit_shown)" = 1 ]' || bad "the Kit view (2): '$(field kit_shown)'"
cmd "cc 49 127"; hit 76; cmd "cc 49 0"; sleep 0.2           # (Shift + pad 5: chosen)
jog 1 1; click; sleep 0.3                                   # (Pad Sample; the browser: the root)
jog 1 1; click                                              # the Move Library
wait_for 5 '[ "$(field kit_place)" = 4 ] && [ "$(field kit_total)" = 2 ]' || bad "the Move Library (2): place '$(field kit_place)' total '$(field kit_total)'"
jog 1 1; click; wait_for 3 '[ "$(field kit_total)" = 1 ]' || bad "Keys (2): total '$(field kit_total)'"
click; wait_for 3 '[ "$(field kit_total)" = 2 ]' || bad "Bells (2): total '$(field kit_total)'"
jog 1 1; sleep 0.6; jog 1 127                               # Bell: on pad 5
wait_for 5 '[ "$(field kit_file)" = Bell.flac ]' || bad "Bell: file '$(field kit_file)' cur '$(field kit_cur)'"
click                                                       # kept: copied into the project's folder, as it is
wait_for 5 'ls "$LIB"/Kits/Projects/*/Bell.flac >/dev/null 2>&1' \
    && cmp -s tests/flac/tone.flac "$(ls "$LIB"/Kits/Projects/*/Bell.flac | head -1)" \
    && ok "a Move Library FLAC kept: copied into the project's folder as it is ($(ls "$LIB"/Kits/Projects/*/Bell.flac 2>/dev/null | sed "s|$T/||"))" \
    || bad "the Move Library's copy-in: cur '$(field kit_cur)' owned '$(field kit_owned)' $(grep 'kept where' "$L" | tail -1)"
f0=$(field kit_finds)
click                                                       # Files again: on Bell, in the Move Library
wait_for 5 '[ "$(field kit_finds)" = $((f0 + 1)) ] && [ "$(field kit_place)" = 4 ]' \
    && ok "Files again: on the pad's sound in the Move Library (KIT_FIND through its copy)" \
    || bad "Files on Bell: finds '$(field kit_finds)' (from $f0) place '$(field kit_place)'"
press 51; press 51; press 51; sleep 0.3                     # (Back: out of Files)
# (2026-11-26) pad 5's pages v2 settings from the page (a = pad * 64 + setting): Warp Grains (33), the Loop's fade 30
# (22), Morph 70 (39), the filter envelope -40 (40); saved, a third start: the project's own copy ("@/") and each back
edit 30 289 2; edit 30 278 30; edit 30 295 70; edit 30 296 -40; sleep 0.3
padv() { curl -s "$U/api/kit" | awk -v p="$1" -v i="$2" 'BEGIN { RS = "\"n\":" } NR == p + 2 { sub(/.*"v":\[/, ""); sub(/\].*/, ""); split($0, a, ","); print a[i + 1] }'; }
[ "$(padv 4 33)/$(padv 4 22)/$(padv 4 39)/$(padv 4 40)" = 2/30/70/-40 ] || bad "pad 5's settings: $(padv 4 33)/$(padv 4 22)/$(padv 4 39)/$(padv 4 40)"
sleep 2.5   # (the project's autosave)
stop
start || { bad "the host did not start (3)"; tail -20 "$L"; exit 1; }
wait_for 8 '[ "$(field kit_pads)" -ge 15 ]' || bad "the kit after the third start: pads '$(field kit_pads)'"
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # the Kit view
cmd "cc 49 127"; hit 76; cmd "cc 49 0"; sleep 0.3           # (Shift + pad 5: chosen)
wait_for 5 '[ "$(field kit_cur)" = 4 ] && [ "$(field kit_own)" = 1 ] && [ "$(field kit_file)" = Bell.flac ] && [ "$(field kit_frames)" = 12000 ]' \
    && [ "$(padv 4 33)/$(padv 4 22)/$(padv 4 39)/$(padv 4 40)" = 2/30/70/-40 ] \
    && ok "a third start: pad 5's sound from the project's own copy (\"@/\" Bell.flac, read as FLAC), its Warp, Loop fade, Morph, filter envelope kept" \
    || bad "pad 5 after a start: cur '$(field kit_cur)' own '$(field kit_own)' file '$(field kit_file)' settings $(padv 4 33)/$(padv 4 22)/$(padv 4 39)/$(padv 4 40)"
# (2026-11-26, the owner's: "After closing app and relaunching ... I would like it to always point at the original
# sample location") pad 5 plays its copy and still knows its original: Files opens on Bell in the Move Library
f0=$(field kit_finds)
[ "$(field kit_orig)" = 1 ] || bad "pad 5's original place after a start: orig '$(field kit_orig)'"
jog 1 1; click                                              # (Pad Sample at rest; the click: Files on the pad's sound)
wait_for 5 '[ "$(field kit_finds)" = $((f0 + 1)) ] && [ "$(field kit_place)" = 4 ] && [ "$(field kit_depth)" = 2 ]' \
    && ok "after a relaunch: Files on pad 5's original (Bell in the Move Library), not the project's copy" \
    || bad "Files on the original after a start: finds '$(field kit_finds)' (from $f0) place '$(field kit_place)' depth '$(field kit_depth)'"
press 51; press 51; press 51; sleep 0.3                     # (Back: out of Files)
# (2026-11-26, the owner's: "none of this was playing until I stopped playback and started again") playing: pad 5's
# steps (track 14), then pad 1's (track 13), each heard on the next pass, Play pressed once
press 85; sleep 0.5
cmd "cc 49 127"; hit 76; cmd "cc 49 0"; sleep 0.2           # (Shift + pad 5: chosen)
cmd "note 16 127"; sleep 0.1; cmd "note 16 0"; cmd "note 24 127"; sleep 0.1; cmd "note 24 0"; sleep 0.2
cmd "cc 49 127"; hit 68; cmd "cc 49 0"; sleep 0.2           # (Shift + pad 1: chosen)
cmd "note 20 127"; sleep 0.1; cmd "note 20 0"; cmd "note 28 127"; sleep 0.1; cmd "note 28 0"; sleep 2.2
n0=$(field kit_notes); sleep 2.0; n1=$(field kit_notes)
[ "$(field seq_playing)" = 1 ] && [ $((n1 - n0)) -ge 6 ] \
    && ok "playing: pad 5's and then pad 1's new steps sound on the next pass ($((n1 - n0)) notes in a bar)" \
    || bad "the new steps while playing: playing '$(field seq_playing)' notes $n0 -> $n1"
press 85; sleep 0.3
# (2026-11-26) Export Samples: a kit track's settings (Shift + step 3), the jog's fifth row, the click
cmd "cc 49 127"; hit 18; cmd "cc 49 0"; sleep 0.3           # (Shift + step 3: track 13's settings)
jog 4 1; x0=$(field kit_exports); click
wait_for 5 '[ "$(field kit_exports)" = $((x0 + 1)) ] && [ "$(field kit_exported)" -ge 5 ]' \
    && ls "$SMP/Kit 1" | grep -q "^01 " && [ -f "$SMP/Kit 1/05 Bell.flac" ] && cmp -s "$SMP/Kit 1/05 Bell.flac" tests/flac/tone.flac \
    && ok "Export Samples: $(field kit_exported) files into My Samples/Kit 1 ($(ls "$SMP/Kit 1" | tr '\n' ',' | sed 's/,$//'))" \
    || bad "Export Samples: exports '$(field kit_exports)' (from $x0) files '$(field kit_exported)' $(ls "$SMP" | tr '\n' ' ') $(grep 'export' "$L" | tail -2)"
click; wait_for 5 '[ "$(field kit_exports)" = $((x0 + 2)) ]' && sleep 0.5 && [ -d "$SMP/Kit 1 2" ] \
    && ok "Export Samples again: a new folder (Kit 1 2), nothing overwritten" || bad "the second export: $(ls "$SMP" | tr '\n' ' ')"
press 51; sleep 0.2
# (2026-11-26) a saved kit keeps its pads' original places: Save (Kit 2: pad 5's "orig" the Move Library's Bell), Load
# it, a relaunch: Files on pad 5 opens on Bell in the Move Library, not on Kit 2's copy
kpost() { curl -s -o /dev/null -X POST --data "$1" "$U/api/kit"; }   # (the page's Kit tab: Save, Load)
kpost '{"op":"save"}'
wait_for 8 '[ "$(field kit_saved_name)" = "Kit 2" ]' && [ -s "$LIB/Kits/Kit 2/kit.json" ] \
    && grep -q '"file": "Bell.flac", "orig": "[^"]*/Bell.flac"' "$LIB/Kits/Kit 2/kit.json" \
    && ! grep '"file": "Bell.flac"' "$LIB/Kits/Kit 2/kit.json" | grep -q '"orig": "[^"]*Kits/' \
    && ok "Save: Kit 2's kit.json names pad 5's original place ($(sed -n 's/.*"file": "Bell.flac", "orig": "\([^"]*\)".*/\1/p' "$LIB/Kits/Kit 2/kit.json" | sed "s|$T/||"))" \
    || bad "Save (Kit 2): name '$(field kit_saved_name)' $(grep -o '"file": "Bell.flac"[^,]*,[^,]*' "$LIB/Kits/Kit 2/kit.json" 2>&1)"
k0=$(field kit_kits)
kid=$(curl -s "$U/api/kit/kits" | tr '{' '\n' | sed -n 's/^"id":\([0-9]*\),"name":"Kit 2".*/\1/p')
kpost "{\"op\":\"open\",\"id\":${kid:-0}}"
wait_for 8 '[ "$(field kit_kits)" = $((k0 + 1)) ] && [ "$(field kit_name)" = "Kit 2" ]' \
    || bad "Load Kit 2 (id '$kid'): kits '$(field kit_kits)' (from $k0) name '$(field kit_name)'"
sleep 2.5   # (the project's autosave)
stop
start || { bad "the host did not start (4)"; tail -20 "$L"; exit 1; }
wait_for 8 '[ "$(field kit_pads)" -ge 15 ]' && [ "$(field kit_name)" = "Kit 2" ] || bad "Kit 2 after a start: pads '$(field kit_pads)' name '$(field kit_name)'"
cmd "cc 40 127"; sleep 0.4; hit 94; cmd "cc 40 0"; sleep 0.3   # the Kit view
cmd "cc 49 127"; hit 76; cmd "cc 49 0"; sleep 0.3           # (Shift + pad 5: chosen)
f0=$(field kit_finds)
wait_for 5 '[ "$(field kit_cur)" = 4 ] && [ "$(field kit_orig)" = 1 ] && [ "$(field kit_file)" = Bell.flac ]' \
    || bad "pad 5 of Kit 2 after a start: cur '$(field kit_cur)' orig '$(field kit_orig)' file '$(field kit_file)'"
jog 1 1; click                                              # (Pad Sample at rest; the click: Files on the pad's sound)
wait_for 5 '[ "$(field kit_finds)" = $((f0 + 1)) ] && [ "$(field kit_place)" = 4 ] && [ "$(field kit_depth)" = 2 ]' \
    && ok "a saved kit loaded, then a relaunch: Files on pad 5's original (Bell in the Move Library), not the kit's copy" \
    || bad "Files on Kit 2's pad 5: finds '$(field kit_finds)' (from $f0) place '$(field kit_place)' depth '$(field kit_depth)'"
press 51; press 51; press 51; sleep 0.3
stop
cp "$L" build/test-out/kit_host.log
echo "kit_e2e: $checks checks, $fail failed (the host's log: build/test-out/kit_host.log; screens build/test-out/kit_*.pbm)"
[ $fail = 0 ]
