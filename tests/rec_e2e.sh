#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The recorder (src/recorder.c) as the sampler app (rfm1cs 0.15.0, RFM_MOVE_CAP_SAMPLER; README "The sampler") on the
# simulator, in the build image after make, through the simulator's command pipe (the Move's buttons, the jog, its
# screen) and the status (the recorder's state and settings, Sample's light, the app's messages). Felucca in slot 1,
# the Move's input from a WAV (--sim-audio-in: tests/ainchk.c make), the takes in a sample library outside the folder
# (--takes-dir, as /data/UserData/UserLibrary/Samples/RemoteFM1 on the Move):
#   - a note layout (Track 1 held + pad 3), Sample: the app on the screen, Sample's light white, the defaults (Master,
#     Bars, Open); audio/rec a link to the takes (the firmwares' audio/ shows them);
#   - Track 1 held + Sample: slot 1 the source; Manual: a take of Felucca played, the WAV in a folder of the day, named
#     after the firmware, its length the time between the presses, not silent; "Saved", Sample green, red recording;
#   - Bars, 1 bar, the sequencer playing: the WAV exactly one bar of frames at the tempo (120 BPM: 88200);
#   - Capture after playing (stopped): what was just played, "Captured";
#   - Threshold on the Input (-6 dB: the input's clicks, not its tone), 1 bar: the input sample for sample, starting
#     50 ms before a click; the Input level (-6 dB) applied to a Manual take of the input;
#   - Stems: a file per present stream (Master, Felucca, Input), the same length;
#   - Track 2 held + Sample: slot 2 the source; Track 4 held + Sample: the master;
#   - Back with a take running: the app closes, the take goes on (Sample red), Sample brings the app back, Sample
#     ends it: the file whole; the take count; quitting during a take: its file finished;
#   - without --takes-dir (no sample library here): the takes in audio/rec itself; a real audio/rec folder with
#     --takes-dir elsewhere: left as it is, said once in the log.
#   tests/rec_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
echo "rec_e2e"
T=$(mktemp -d /tmp/rfm14rec.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; LIB=$T/Samples/RemoteFM1; mkdir -p "$SIM" "$R" "$T/Samples"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17836; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
build/tools/ainchk make "$T/in.wav" || { echo "  FAIL  ainchk make"; exit 1; }
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
held() { cmd "cc $1 127"; sleep 0.3; press 118; sleep 0.1; cmd "cc $1 0"; sleep 0.3; }   # Track (CC 43..40) held + Sample
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; SCR=$(tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01); }
lit() { printf '%s' "$SCR" | tr -cd 1 | wc -c | tr -d ' '; }
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.05; _i=$((_i + 1)); done; }   # n detents, 1 / 127
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.1; }
# a setting: the jog to row $1 (0 Source, 1 Mode, 2 Length, 3 Threshold, 4 Input level: up past the first row is the
# tabs, one down the first row), its click, $2 detents (+ / -), a click
set_() { jog 6 127; jog 1 1; jog "$1" 1; click; if [ "$2" -ge 0 ]; then jog "$2" 1; else jog $((-$2)) 127; fi; click; }
sample() { field move_sample; }
white() { _c=$(sample); [ "$(echo "$_c" | cut -c1-2)" = "$(echo "$_c" | cut -c3-4)" ] && [ "$(echo "$_c" | cut -c3-4)" = "$(echo "$_c" | cut -c5-6)" ] && [ "$(echo "$_c" | cut -c1-2)" != 00 ]; }
red() { _c=$(sample); [ "$(echo "$_c" | cut -c3-6)" = 0000 ] && [ "$(echo "$_c" | cut -c1-2)" != 00 ]; }
wavs() { find "$1" -name '*.wav' 2>/dev/null | sort; }   # (one a line: the names have spaces)
newest() { ls -t "$1"/*/*.wav 2>/dev/null | head -1; }
frames() { build/tools/ainchk info "$1" | sed -n 's/^frames \([0-9]*\) .*/\1/p'; }
peak() { build/tools/ainchk info "$1" | sed -n 's/.* peak \([0-9]*\)$/\1/p'; }
takes() { field rec_takes; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 --sim-audio-in "$T/in.wav" "$@" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && [ -n "$(field rec_state)" ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }

# ---------------------------------------------------------------- the takes in a sample library: audio/rec a link
start --takes-dir "$LIB" || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5   # (the firmware's splash and the session's own popups gone)
[ -L "$R/audio/rec" ] && [ "$(cd "$R/audio/rec" && pwd -P)" = "$(cd "$LIB" && pwd -P)" ] && [ "$(field rec_link)" = 1 ] \
    && [ "$(field takes_dir)" = "$LIB" ] && grep -q "the recorder's takes go to $LIB (a folder a day), audio/rec links to them" "$L" \
    && ok "--takes-dir outside the folder: the takes go there, audio/rec a link to it (the carts' audio/ follows it: unit)" \
    || bad "the takes' folder: '$(field takes_dir)', link $(field rec_link), $(ls -l "$R/audio" | tr '\n' ' ')"
info "the status: $(status | wc -c | tr -d ' ') bytes"
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"   # Track 1 held + pad 3: Scale (a note layout, the sequencer)
wait_for 4 '[ "$(field move_mode)" = 3 ]' || bad "the note layout: mode '$(field move_mode)'"
sleep 1.3
shot; s0=$SCR
press 118
wait_for 3 '[ "$(field smp_shown)" = 1 ]' && sleep 0.3 && shot && cp "$SIM/mr-screen.pbm" build/test-out/rec_open.pbm
[ "$(field smp_shown)" = 1 ] && [ "$SCR" != "$s0" ] && [ "$(lit)" -gt 80 ] && wait_for 3 white \
    && [ "$(field rec_src)" = 0 ] && [ "$(field rec_mode)" = 1 ] && [ "$(field rec_len)" = 0 ] \
    && ok "Sample: the sampler app on the screen ($(lit) pixels, build/test-out/rec_open.pbm), Sample white ($(sample)), the defaults Master, Bars, Open" \
    || bad "Sample: shown '$(field smp_shown)', light '$(sample)', src $(field rec_src) mode $(field rec_mode) len $(field rec_len)"

# ---------------------------------------------------------------- Manual: Felucca played
held 43
set_ 1 2   # Mode: Bars -> Manual
wait_for 3 '[ "$(field rec_src)" = 1 ] && [ "$(field rec_mode)" = 3 ]' \
    && ok "Track 1 held + Sample: slot 1 the source; the jog's Mode row, its click, two detents, a click: Manual" \
    || bad "slot 1, Manual: src '$(field rec_src)' mode '$(field rec_mode)'"
g0=$(field smp_greens); m0=$(field smp_msgs)
cmd "note 68 110"; sleep 0.2
press 118; t0=$(date +%s.%N)
wait_for 3 '[ "$(field rec_state)" = 2 ]' && wait_for 2 red && ok "Sample: recording (Sample red: $(sample))" \
    || bad "recording: state '$(field rec_state)', light '$(sample)'"
sleep 1.5; cmd "note 68 0"; sleep 0.3
press 118; t1=$(date +%s.%N)
wait_for 5 '[ "$(takes)" = 1 ]'
F=$(newest "$LIB"); n=$(frames "$F"); p=$(peak "$F")
want=$(echo "$t0 $t1" | awk '{printf "%d", ($2 - $1) * 44100}')
day=$(date +%Y-%m-%d)
[ -n "$F" ] && [ "$(dirname "$F")" = "$LIB/$day" ] && echo "$(basename "$F")" | grep -qi "felucca" \
    && [ "$n" -gt $((want - 13230)) ] && [ "$n" -lt $((want + 13230)) ] && [ "$p" -gt 1000 ] \
    && ok "Manual: '$(basename "$F")' in $day/, $n frames ($(echo "$n" | awk '{printf "%.2f", $1 / 44100}') s; the presses $(echo "$want" | awk '{printf "%.2f", $1 / 44100}') s apart), peak $p" \
    || bad "the Manual take: '$F' ($n frames, want ~$want; peak $p); $(wavs "$T/Samples" | tr '\n' ' ')"
[ -f "$R/audio/rec/$day/$(basename "$F")" ] && ok "the take through audio/rec: audio/rec/$day/$(basename "$F")" \
    || bad "audio/rec does not show the take"
wait_for 3 '[ "$(field smp_greens)" -gt "$g0" ] && [ "$(field smp_msgs)" -gt "$m0" ] && [ "$(field smp_msg)" = 1 ]' \
    && wait_for 3 white \
    && ok "the saved message (SMP_EV_SAVED), Sample green for a moment, then white again" \
    || bad "the save's message / light: greens $g0 -> $(field smp_greens), msgs $m0 -> $(field smp_msgs) ($(field smp_msg)), light $(sample)"

# ---------------------------------------------------------------- Bars, 1 bar, the sequencer playing
set_ 1 -2   # Mode: Manual -> Bars
set_ 2 1    # Length: Open -> 1
wait_for 3 '[ "$(field rec_mode)" = 1 ] && [ "$(field rec_len)" = 1 ]' || bad "Bars, 1: mode '$(field rec_mode)' len '$(field rec_len)'"
press 85
wait_for 3 '[ "$(field seq_playing)" = 1 ]' || bad "Play: seq_playing '$(field seq_playing)'"
cmd "note 68 110"; sleep 0.5
press 118
wait_for 3 '[ "$(field rec_state)" = 1 ] || [ "$(field rec_state)" = 2 ]' && info "armed / recording: state $(field rec_state)"
wait_for 8 '[ "$(takes)" = 2 ]'
cmd "note 68 0"
press 85
F=$(newest "$LIB"); n=$(frames "$F")
bpm=$(basename "$F" | sed -n 's/.* \([0-9.]*\)bpm 1b\.wav$/\1/p')
want=$(echo "${bpm:-0}" | awk '{ if ($1 > 0) printf "%d", 4 * 60 / $1 * 44100 + 0.5; else print 0 }')
[ -n "$bpm" ] && [ "$n" -ge $((want - 1)) ] && [ "$n" -le $((want + 1)) ] && [ "$(peak "$F")" -gt 1000 ] \
    && ok "Bars, 1 bar, playing: '$(basename "$F")', $n frames: one bar at $bpm BPM ($want)" \
    || bad "the bar: '$F', $n frames, bpm '$bpm' (want $want)"
wait_for 3 '[ "$(field seq_playing)" = 0 ]' || bad "Play again: seq_playing '$(field seq_playing)'"

# ---------------------------------------------------------------- Capture, stopped: what was just played
set_ 1 -1   # Mode: Bars -> Capture
wait_for 3 '[ "$(field rec_mode)" = 0 ]' || bad "Capture: mode '$(field rec_mode)'"
m0=$(field smp_msgs); sleep 2.5
cmd "note 68 110"; sleep 1.0; cmd "note 68 0"; sleep 0.3
press 118
wait_for 5 '[ "$(takes)" = 3 ]'
F=$(newest "$LIB"); n=$(frames "$F")
[ "$n" -gt 44100 ] && [ "$n" -lt $((44100 * 4)) ] && [ "$(peak "$F")" -gt 1000 ] && wait_for 3 '[ "$(field smp_msgs)" -gt "$m0" ] && [ "$(field smp_msg)" = 1 ]' \
    && ok "Capture after playing: '$(basename "$F")', $n frames, peak $(peak "$F") (back to the silence before the note)" \
    || bad "the capture: '$F', $n frames, peak $(peak "$F"), msgs $m0 -> $(field smp_msgs)"

# ---------------------------------------------------------------- Threshold on the Input
set_ 0 3    # Source: Slot 1 -> Input
set_ 1 2    # Mode: Capture -> Threshold
set_ 3 18   # Threshold: -24 -> -6 dB (the input's clicks reach 20000, its tone 8000)
wait_for 3 '[ "$(field rec_src)" = 4 ] && [ "$(field rec_mode)" = 2 ] && [ "$(field rec_thr)" -gt 16000 ] && [ "$(field rec_thr)" -lt 17000 ]' \
    && ok "Input, Threshold, -6 dB (peak $(field rec_thr)), Length 1" \
    || bad "the threshold's settings: src '$(field rec_src)' mode '$(field rec_mode)' thr '$(field rec_thr)' len '$(field rec_len)'"
u0=$(field ain_under)
press 118
wait_for 8 '[ "$(takes)" = 4 ]'
F=$(newest "$LIB"); n=$(frames "$F")
u1=$(field ain_under)
if r=$(build/tools/ainchk gaps "$F" "$T/in.wav"); then
    o=$(echo "$r" | sed -n 's/.*from frame \([0-9]*\) on.*/\1/p'); off=$(( (22050 - o % 22050) % 22050 ))
    z=$(echo "$r" | sed -n 's/.* \([0-9]*\) silent frames$/\1/p')
    [ "$n" -ge 88199 ] && [ "$n" -le 88201 ] && [ "$off" -ge 2205 ] && [ "$off" -le $((2205 + 128)) ] && [ "$z" -le 512 ] \
        && basename "$F" | grep -q "Input" \
        && ok "Threshold: '$(basename "$F")', one bar ($n frames) of the input, from $off frames before a click (50 ms + the block), sample for sample but $z silent frames (the input path's under-runs: $u0 -> $u1 blocks)" \
        || bad "the threshold take: $n frames, $off frames before a click, $z silent ($r)"
else bad "the threshold take is not the input: $r ('$F'; build/test-out/rec_threshold.wav)"; cp "$F" build/test-out/rec_threshold.wav; cp "$T/in.wav" build/test-out/rec_in.wav; fi
set_ 4 -6   # Input level: 0 -> -6 dB
set_ 1 1    # Mode: Threshold -> Manual
wait_for 3 '[ "$(field rec_in_db)" = -6 ] && [ "$(field rec_mode)" = 3 ]' || bad "Input level: '$(field rec_in_db)', mode '$(field rec_mode)'"
press 118; sleep 1.2; press 118
wait_for 5 '[ "$(takes)" = 5 ]'
F=$(newest "$LIB"); p=$(peak "$F")
[ "$p" -ge 9900 ] && [ "$p" -le 10150 ] && ok "the Input level, -6 dB: the input's 20000 clicks at $p" || bad "the Input level: peak $p ('$F')"
set_ 4 6

# ---------------------------------------------------------------- Stems
set_ 0 1    # Source: Input -> Stems
wait_for 3 '[ "$(field rec_src)" = -1 ]' || bad "Stems: src '$(field rec_src)'"
cmd "note 68 110"; press 118; sleep 1.2; cmd "note 68 0"; press 118
wait_for 5 '[ "$(takes)" = 6 ]'
sleep 0.5
F=$(newest "$LIB"); stamp=$(basename "$F" | cut -c1-11)
set -- "$LIB/$day/$stamp"*.wav
lens=$(for f in "$@"; do frames "$f"; done | sort -u | tr '\n' ' ')
names=$(for f in "$@"; do basename "$f" | cut -c13- | sed 's/ [0-9.]*s\.wav$//'; done | sort | tr '\n' ',')
[ $# = 3 ] && [ "$(echo $lens | wc -w | tr -d ' ')" = 1 ] && [ "$names" = "Felucca,Input,Master," ] \
    && ok "Stems: one take, a file per present stream ($names), each $lens frames" \
    || bad "Stems: $# files ($names), lengths $lens"

# ---------------------------------------------------------------- Track 2 / Track 4 held + Sample
press 51; wait_for 3 '[ "$(field smp_shown)" = 0 ]' || bad "Back: shown '$(field smp_shown)'"
held 42
wait_for 3 '[ "$(field smp_shown)" = 1 ] && [ "$(field rec_src)" = 2 ]' && ok "Track 2 held + Sample: the app, slot 2 the source" \
    || bad "Track 2 + Sample: shown '$(field smp_shown)' src '$(field rec_src)'"
held 40
wait_for 3 '[ "$(field rec_src)" = 0 ]' && [ "$(field rec_state)" = 0 ] && ok "Track 4 held + Sample: the master (nothing recorded)" \
    || bad "Track 4 + Sample: src '$(field rec_src)' state '$(field rec_state)'"

# ---------------------------------------------------------------- Back with a take running
cmd "note 68 110"
press 118; t0=$(date +%s.%N)
wait_for 3 '[ "$(field rec_state)" = 2 ]'
press 51
wait_for 3 '[ "$(field smp_shown)" = 0 ]' && sleep 1.2 && [ "$(field rec_state)" = 2 ] && red \
    && ok "Back during a take: the app closes, the take goes on (Sample red)" \
    || bad "Back during a take: shown '$(field smp_shown)' state '$(field rec_state)' light '$(sample)'"
press 118
wait_for 3 '[ "$(field smp_shown)" = 1 ]' && [ "$(field rec_state)" = 2 ] && ok "Sample: the app again, the take still running" \
    || bad "Sample after Back: shown '$(field smp_shown)' state '$(field rec_state)'"
press 118; t1=$(date +%s.%N); cmd "note 68 0"
wait_for 5 '[ "$(takes)" = 7 ]'
F=$(newest "$LIB"); n=$(frames "$F")
want=$(echo "$t0 $t1" | awk '{printf "%d", ($2 - $1) * 44100}')
[ -n "$n" ] && [ "$n" -gt $((want - 13230)) ] && [ "$n" -lt $((want + 13230)) ] && basename "$F" | grep -q Master \
    && ok "...Sample ends it: '$(basename "$F")' whole, $n frames (the presses $(echo "$want" | awk '{printf "%.2f", $1 / 44100}') s apart)" \
    || bad "the take over Back: '$F', $n frames (want ~$want)"
nw=$(wavs "$LIB" | wc -l | tr -d ' ')
[ "$(takes)" = 7 ] && [ "$nw" = 9 ] && ok "the take count: 7 takes (9 files: the stems take's 3)" || bad "the count: $(takes) takes, $nw files"

# ---------------------------------------------------------------- quitting during a take
press 118; wait_for 3 '[ "$(field rec_state)" = 2 ]'; sleep 1
stop
F=$(newest "$LIB"); n=$(frames "$F")
[ "$RC" = 0 ] && [ "$(wavs "$LIB" | wc -l | tr -d ' ')" = 10 ] && [ -n "$n" ] && [ "$n" -gt 30000 ] && ! ls "$LIB/$day" | grep -q '\.part$' \
    && grep -q "the recorder stopped (7 take(s) saved)" "$L" \
    && ok "quit during a take: its file finished ('$(basename "$F")', $n frames), no .part left" \
    || bad "quit during a take: rc $RC, $(wavs "$LIB" | wc -l) files, '$F' $n frames; $(ls "$LIB/$day" | tr '\n' ' ')"

# ---------------------------------------------------------------- no sample library: audio/rec itself
R2=$T/root2; mkdir -p "$R2"; echo "http://127.0.0.1:1" > "$R2/catalog-url"; R0=$R; R=$R2
start || { bad "the host did not start (root2)"; tail -20 "$L"; exit 1; }
[ -d "$R2/audio/rec" ] && [ ! -L "$R2/audio/rec" ] && [ "$(field takes_dir)" = "$R2/audio/rec" ] && [ "$(field rec_link)" = 0 ] \
    && ok "no sample library, no --takes-dir: the takes go to audio/rec itself (a folder, no link)" \
    || bad "the default folder: '$(field takes_dir)', link $(field rec_link)"
stop
# ---------------------------------------------------------------- a real audio/rec folder stays
R3=$T/root3; mkdir -p "$R3/audio/rec"; echo "http://127.0.0.1:1" > "$R3/catalog-url"; echo mine > "$R3/audio/rec/mine.txt"; R=$R3
start --takes-dir "$T/Samples3/RemoteFM1" || { bad "the host did not start (root3)"; tail -20 "$L"; exit 1; }
[ -d "$R3/audio/rec" ] && [ ! -L "$R3/audio/rec" ] && [ "$(cat "$R3/audio/rec/mine.txt")" = mine ] && [ "$(field rec_link)" = -1 ] \
    && [ "$(grep -c "$R3/audio/rec is not a link to the takes" "$L")" = 1 ] && [ -d "$T/Samples3/RemoteFM1" ] \
    && ok "a real audio/rec folder with the takes elsewhere: left as it is (its file kept), said once in the log" \
    || bad "the real folder: link $(field rec_link), $(ls -la "$R3/audio" | tr '\n' ' ')"
stop
R=$R0
echo "rec_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
