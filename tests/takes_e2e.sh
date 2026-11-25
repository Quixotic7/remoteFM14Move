#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The take library (src/takes.c) as the sampler's Takes and Tools tabs (rfm1cs 0.15.0 stage 2; README "The take
# library") on the simulator, in the build image after make, through the simulator's command pipe (the Move's buttons,
# knobs, the jog, its screen) and the status (the app's screens, the library's answers, what plays). Felucca in slot 1,
# the takes in a sample library outside the folder (--takes-dir):
#   - a Manual take of Felucca (Track 1 held + Sample): named "Track 1 Felucca"; Trim comes up by itself on it;
#     knob 1 and knob 2 move the start and the end, the jog to Keep, its click: the file exactly the kept frames
#     (sample for sample, 132 frames faded at each cut end); the Record tab again;
#   - the Takes tab: its row on the screen; Undo: the file as it was, byte for byte;
#   - the Tools tab on that take: Normalize (the peak -0.3 dBFS: 31656), Undo; Reverse (the frames reversed), Undo;
#   - Delete held + the jog's click: the take gone from its day, in the hidden trash; Undo: back;
#   - a take's actions (the click), Play (the click): it plays (the status's position moves), the click again stops it;
#   - Capture with the song stopped: the take's actions come up (Play chosen) and it plays by itself;
#   - a second run, silent (nothing played but a take): Play while the master records: the output has the take (the
#     host's mix, --capture), the master's take does not.
#   tests/takes_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
echo "takes_e2e"
T=$(mktemp -d /tmp/rfm14takes.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; LIB=$T/Samples/RemoteFM1; CAP=$T/cap; mkdir -p "$SIM" "$R" "$T/Samples" "$CAP"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17837; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
held() { cmd "cc $1 127"; sleep 0.3; press 118; sleep 0.1; cmd "cc $1 0"; sleep 0.3; }   # Track (CC 43..40) held + Sample
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; SCR=$(tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01); }
lit() { printf '%s' "$SCR" | tr -cd 1 | wc -c | tr -d ' '; }
rows_lit() { printf '%s' "$SCR" | cut -c2049- | tr -cd 1 | wc -c | tr -d ' '; }   # (below the tabs line: y 16..63)
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.05; _i=$((_i + 1)); done; }   # n detents, 1 / 127
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.15; }
set_() { jog 6 127; jog 1 1; jog "$1" 1; click; if [ "$2" -ge 0 ]; then jog "$2" 1; else jog $((-$2)) 127; fi; click; }
tab() { jog 14 127; click; jog 3 127; jog "$1" 1; click; jog 1 1; sleep 0.3; }   # the tabs, a tab picked, its first row
frames() { build/tools/ainchk info "$1" | sed -n 's/^frames \([0-9]*\) .*/\1/p'; }
peak() { build/tools/ainchk info "$1" | sed -n 's/.* peak \([0-9]*\)$/\1/p'; }
newest() { ls -t "$1"/*/*.wav 2>/dev/null | head -1; }
takes() { field rec_takes; }
edits() { field tl_edits; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 "$@" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && [ -n "$(field smp_tab)" ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
note_layout() { cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"; wait_for 4 '[ "$(field move_mode)" = 3 ]' || bad "the note layout"; sleep 1.3; }

# ---------------------------------------------------------------- a Manual take: Trim by itself, the knobs, Keep
start --takes-dir "$LIB" || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5
note_layout
press 118
wait_for 3 '[ "$(field smp_shown)" = 1 ]' || bad "the app: shown '$(field smp_shown)'"
held 43
set_ 1 2   # Mode: Bars -> Manual
wait_for 3 '[ "$(field rec_src)" = 1 ] && [ "$(field rec_mode)" = 3 ]' || bad "slot 1, Manual: src '$(field rec_src)' mode '$(field rec_mode)'"
cmd "note 68 110"; sleep 0.2
press 118; sleep 1.6; cmd "note 68 0"; sleep 0.3; press 118
wait_for 5 '[ "$(takes)" = 1 ]'
F=$(newest "$LIB"); n=$(frames "$F")
wait_for 4 '[ "$(field smp_scr)" = 2 ] && [ "$(field smp_cur_frames)" = "$n" ]' && [ "$(field smp_t_start)" = 0 ] \
    && [ "$(field smp_t_end)" = "$n" ] && [ "$(field smp_tab)" = 0 ] && basename "$F" | grep -q " Track 1 Felucca " \
    && ok "Manual take '$(basename "$F")' ($n frames): Trim comes up by itself on it (start 0, end $n), over the Record tab" \
    || bad "the automatic Trim: scr '$(field smp_scr)', cur frames '$(field smp_cur_frames)' (file $n), start '$(field smp_t_start)' end '$(field smp_t_end)', '$F'; library: rows $(field tl_rows) saved $(field tl_saved) tabs $(field smp_tab)"
sleep 1.2; shot; cp "$SIM/mr-screen.pbm" build/test-out/takes_trim.pbm
[ "$(lit)" -gt 300 ] && ok "Trim on the screen ($(lit) pixels, build/test-out/takes_trim.pbm)" || bad "Trim's screen: $(lit) pixels"
cp "$F" "$T/orig.wav"
st=$((n / 256))
cmd "cc 71 20"; sleep 0.1; cmd "cc 72 118"; sleep 0.2   # knob 1: +20 steps, knob 2: -10
ws=$((20 * st)); we=$((n - 10 * st))
wait_for 3 '[ "$(field smp_t_start)" = "$ws" ] && [ "$(field smp_t_end)" = "$we" ]' \
    && ok "knob 1: the start 20 steps on ($ws), knob 2: the end 10 back ($we; a step 1/256 of the take: $st)" \
    || bad "the knobs: start '$(field smp_t_start)' (want $ws) end '$(field smp_t_end)' (want $we)"
shot; cp "$SIM/mr-screen.pbm" build/test-out/takes_trim_zoom.pbm
jog 1 1   # (the jog starts on Play; the knobs leave it there) Keep
wait_for 2 '[ "$(field smp_t_i)" = 3 ]' || bad "the jog to Keep: '$(field smp_t_i)'"
e0=$(edits); click
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_ok)" = 1 ]'
nn=$(frames "$F")
r=$(build/tools/ainchk trimmed "$F" "$T/orig.wav" "$ws"); rc=$?
[ "$nn" = $((we - ws)) ] && [ "$rc" = 0 ] && echo "$r" | grep -q exact && [ "$(field smp_scr)" = 0 ] && [ "$(field smp_tab)" = 0 ] \
    && ok "Keep: the file exactly the kept frames ($nn = $we - $ws; $r); back to the Record tab" \
    || bad "Keep: $nn frames (want $((we - ws))): $r; scr '$(field smp_scr)' tab '$(field smp_tab)'"

# ---------------------------------------------------------------- the Takes tab; Undo
tab 1
wait_for 3 '[ "$(field smp_tab)" = 1 ] && [ "$(field smp_total)" = 1 ]' && shot && cp "$SIM/mr-screen.pbm" build/test-out/takes_list.pbm
[ "$(field smp_tab)" = 1 ] && [ "$(field smp_total)" = 1 ] && [ "$(rows_lit)" -gt 300 ] \
    && ok "the Takes tab: one take, its row on the screen ($(rows_lit) pixels under the tabs, build/test-out/takes_list.pbm)" \
    || bad "the Takes tab: tab '$(field smp_tab)' total '$(field smp_total)' rows $(rows_lit) pixels"
[ "$(field tl_undo)" = 1 ] || bad "an undo after Keep: '$(field tl_undo)'"
e0=$(edits); press 56
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_op)" = 7 ] && [ "$(field tl_ok)" = 1 ]'
build/tools/ainchk same "$F" "$T/orig.wav" >/dev/null && [ "$(field tl_undo)" = 0 ] \
    && ok "Undo (in Takes): the file as it was before the trim, byte for byte; no undo left" \
    || bad "Undo: $(build/tools/ainchk same "$F" "$T/orig.wav"); undo '$(field tl_undo)'"

# ---------------------------------------------------------------- Tools: Normalize, Reverse
tab 2
wait_for 3 '[ "$(field smp_tab)" = 2 ] && [ "$(field smp_ti)" = 0 ] && [ "$(field smp_cur_frames)" = "$n" ]' \
    || bad "the Tools tab: tab '$(field smp_tab)' ti '$(field smp_ti)' cur frames '$(field smp_cur_frames)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/takes_tools.pbm
jog 1 1; e0=$(edits); click
wait_for 6 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_ok)" = 1 ]'
p=$(peak "$F")
[ "$p" = 31656 ] && [ "$(frames "$F")" = "$n" ] && ok "Normalize: the peak $p (-0.3 dBFS; it was $(peak "$T/orig.wav")), the length kept" \
    || bad "Normalize: peak $p, $(frames "$F") frames"
sleep 1.2; e0=$(edits); press 56
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ]'
build/tools/ainchk same "$F" "$T/orig.wav" >/dev/null && ok "Undo: as it was" || bad "Undo after Normalize: $(build/tools/ainchk same "$F" "$T/orig.wav")"
jog 1 1; e0=$(edits); click
wait_for 6 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_ok)" = 1 ]'
r=$(build/tools/ainchk reversed "$F" "$T/orig.wav") && ok "Reverse: $r" || bad "Reverse: $r"
sleep 1.2; e0=$(edits); press 56
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ]'
build/tools/ainchk same "$F" "$T/orig.wav" >/dev/null && ok "Undo: as it was" || bad "Undo after Reverse"

# ---------------------------------------------------------------- Delete held + the click; Undo
tab 1
wait_for 3 '[ "$(field smp_tab)" = 1 ] && [ "$(field smp_total)" = 1 ]' || bad "Takes again: total '$(field smp_total)'"
e0=$(edits); cmd "cc 119 127"; sleep 0.1; click; cmd "cc 119 0"
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_op)" = 1 ] && [ "$(field tl_ok)" = 1 ]'
tr_=$(ls "$LIB/.trash" 2>/dev/null | grep -c Felucca)
[ ! -f "$F" ] && [ "$tr_" = 1 ] && wait_for 3 '[ "$(field smp_total)" = 0 ]' \
    && ok "Delete held + the click: the take gone from its day, in the hidden trash, the list empty" \
    || bad "Delete: file there? $([ -f "$F" ] && echo yes || echo no), trash $tr_, total '$(field smp_total)'"
sleep 1.2; e0=$(edits); press 56
wait_for 5 '[ "$(edits)" = $((e0 + 1)) ] && [ "$(field tl_ok)" = 1 ]'
[ -f "$F" ] && build/tools/ainchk same "$F" "$T/orig.wav" >/dev/null && wait_for 3 '[ "$(field smp_total)" = 1 ]' \
    && ok "Undo: the take back where it was, the same file" || bad "Undo of the delete: '$F' $(ls "$(dirname "$F")")"

# ---------------------------------------------------------------- a take's actions, Play
sleep 1.2; click
wait_for 3 '[ "$(field smp_scr)" = 1 ] && [ "$(field smp_ai)" = 0 ]' || bad "the take's actions: scr '$(field smp_scr)'"
id=$(field smp_cur)
click
wait_for 3 '[ "$(field tl_play)" = "$id" ]' && p1=$(field tl_pos) && sleep 1.1 && p2=$(field tl_pos) && [ "$p2" -gt "$p1" ] \
    && shot && cp "$SIM/mr-screen.pbm" build/test-out/takes_playing.pbm \
    && ok "Play (two clicks from the list): the take plays (its frame $p1 -> $p2; build/test-out/takes_playing.pbm)" \
    || bad "Play: playing '$(field tl_play)' (want $id), position '$(field tl_pos)'"
click
wait_for 3 '[ "$(field tl_play)" = 0 ]' && ok "Play again: it stops" || bad "the stop: playing '$(field tl_play)'"
press 51
wait_for 2 '[ "$(field smp_scr)" = 0 ] && [ "$(field smp_tab)" = 1 ]' || bad "Back: the list ('$(field smp_scr)')"
t0=$(takes); press 118   # (the owner's 2026-11-25) Sample in Takes: the chosen take plays, nothing records
wait_for 3 '[ "$(field tl_play)" = "$id" ]' && [ "$(field rec_state)" = 0 ] && press 118 && wait_for 3 '[ "$(field tl_play)" = 0 ]' \
    && [ "$(takes)" = "$t0" ] && ok "Sample in the Takes tab: the chosen take plays, again: it stops; no take recorded" \
    || bad "Sample in Takes: playing '$(field tl_play)' (want $id), recorder '$(field rec_state)', takes $(takes) (were $t0)"
press 51   # Back in Takes: the tabs (the jog on them), the app still shown; again: closed
wait_for 2 '[ "$(field smp_shown)" = 1 ] && [ "$(field smp_tab)" = 1 ]' && press 51 && wait_for 2 '[ "$(field smp_shown)" = 0 ]' \
    && ok "Back in the Takes tab: the tabs first, then the app closes" || bad "Back in Takes: shown '$(field smp_shown)' tab '$(field smp_tab)'"
press 118
wait_for 3 '[ "$(field smp_shown)" = 1 ]' || bad "the app again"

# ---------------------------------------------------------------- Capture: the preview
tab 0
set_ 1 -3   # Mode: Manual -> Capture
wait_for 3 '[ "$(field rec_mode)" = 0 ]' || bad "Capture: mode '$(field rec_mode)'"
sleep 2.5; cmd "note 68 110"; sleep 1.0; cmd "note 68 0"; sleep 0.3
press 118
wait_for 5 '[ "$(takes)" = 2 ]'
wait_for 4 '[ "$(field smp_scr)" = 1 ] && [ "$(field smp_ai)" = 0 ] && [ "$(field tl_play)" != 0 ] && [ "$(field tl_play)" = "$(field smp_cur)" ]' \
    && [ "$(field seq_playing)" = 0 ] \
    && ok "Capture, the song stopped: the take's actions come up (Play chosen) and it plays by itself" \
    || bad "the preview: scr '$(field smp_scr)' ai '$(field smp_ai)' playing '$(field tl_play)' cur '$(field smp_cur)'"
sleep 1.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/takes_preview.pbm
press 51
wait_for 2 '[ "$(field smp_scr)" = 0 ] && [ "$(field smp_tab)" = 0 ] && [ "$(field tl_play)" = 0 ]' \
    && ok "Back: the Record tab it came over, the play stopped" || bad "Back from the preview: scr '$(field smp_scr)' tab '$(field smp_tab)' play '$(field tl_play)'"
stop
[ "$RC" = 0 ] && ok "the host quits (the library's threads stopped)" || bad "quit: rc $RC"

# ---------------------------------------------------------------- silent: Play while the master records
R=$T/root2; LIB2=$T/Samples2/RemoteFM1; day=$(date +%Y-%m-%d); md=$(date +%m%d)
mkdir -p "$R" "$LIB2/$day"; echo "http://127.0.0.1:1" > "$R/catalog-url"
cp "$T/orig.wav" "$LIB2/$day/$md-000100 Track 1 Felucca 1.5s.wav"
start --takes-dir "$LIB2" --capture "$CAP" --capture-seconds 120 || { bad "the host did not start (2)"; tail -20 "$L"; exit 1; }
sleep 2.5
note_layout
held 40   # Track 4 held + Sample: the master
set_ 1 2   # Mode: Bars -> Manual
wait_for 3 '[ "$(field rec_src)" = 0 ] && [ "$(field rec_mode)" = 3 ]' || bad "the master, Manual: src '$(field rec_src)' mode '$(field rec_mode)'"
tab 1
wait_for 3 '[ "$(field smp_total)" = 1 ]' || bad "the take there: total '$(field smp_total)'"
click
wait_for 3 '[ "$(field smp_scr)" = 1 ]' || bad "its actions"
id=$(field smp_cur); press 51; tab 0
press 118   # Record: Sample starts the master's take; then the take is played on the Move as the page does it (outside
            # Record Sample plays; a take running: the app shows only it)
wait_for 3 '[ "$(field rec_state)" = 2 ]' || bad "the master records: state '$(field rec_state)'"
curl -s -X POST -H 'Content-Type: application/json' -d "{\"id\":$id}" "$U/api/takes/play" >/dev/null
wait_for 3 '[ "$(field tl_play)" != 0 ]' || bad "Play: '$(field tl_play)'"
wait_for 5 '[ "$(field tl_play)" = 0 ]'
sleep 0.3; press 118
wait_for 5 '[ "$(takes)" = 1 ]'
M=$(ls -t "$LIB2/$day"/*Master*.wav 2>/dev/null | head -1); mp=$(peak "$M"); mf=$(frames "$M")
stop
mix=$(build/tools/ainchk rawpeak "$CAP/mix.raw"); xp=$(echo "$mix" | sed -n 's/.* peak \([0-9]*\) .*/\1/p'); xl=$(echo "$mix" | sed -n 's/.* loud \([0-9]*\)$/\1/p')
op=$(peak "$T/orig.wav")
[ -n "$M" ] && [ "${mf:-0}" -gt 44100 ] && [ "${mp:-99999}" -lt 50 ] && [ "${xp:-0}" -gt 1000 ] && [ "${xp:-0}" -le "$op" ] \
    && ok "a take played while the master recorded: the output has it (the mix: peak $xp, $xl loud frames; the take's peak $op), the master's take does not ('$(basename "$M")', $mf frames, peak $mp)" \
    || bad "never recorded: the master's take '$M' ($mf frames) peak $mp; the mix: $mix (the take's peak $op)"
cp "$L" build/test-out/takes_host.log
echo "takes_e2e: $checks checks, $fail failed (the host's log: build/test-out/takes_host.log)"
[ $fail = 0 ]
