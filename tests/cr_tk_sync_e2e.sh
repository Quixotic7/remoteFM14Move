#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# ChoralRoot's takeover on RemoteFM1's clock (rfm1cs 0.17.0; the owner, 2026-11-26: "ChoralRoot takeover should always
# match the RemoteFM1 tempo and sync the playhead"), in the build image after make with a tk1 build of ChoralRoot that
# follows a host's clock among the development builds (make DEV_FIRMWARES=.../choralroot-<v>.rfm1; skipped without
# one). The host on the simulator, its mix captured (--capture), the screen reader on (tests/fake_tts.c): the takeover
# up, Dm7 in step 1 (the loop), ChoralRoot's metronome on (Shift + step 6); stopped, RemoteFM1's sequencer is not
# following anything; the takeover's Play: RemoteFM1's transport plays at its own tempo (120); ChoralRoot's Tempo knob
# turned while it plays: RemoteFM1's tempo changes (the takeover's request), ChoralRoot's tempo follows (said on a
# touch); Play again: both stop, and again. Then tests/tksync.c: every onset of the mix while it played (the clicks, the
# chord), against RemoteFM1's song position (/api/status seq_pos at its block count, polled): the same offset from the
# beat within one MIDI clock (4 ticks of 96 a beat) over the bars before the tempo change and after it.
#   tests/cr_tk_sync_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "cr_tk_sync_e2e"
CR=$(ls build/fw-dev/choralroot-*.rfm1m 2>/dev/null | sort -V | tail -1)
if [ -z "$CR" ] || ! nm -D "$CR" | grep -q rfm1m_tk1; then
    echo "  skip  no tk1 build of ChoralRoot in build/fw-dev (make DEV_FIRMWARES=.../choralroot-<v>.rfm1)"; exit 0
fi
[ -x build/tools/tksync ] || { echo "  FAIL  build/tools/tksync missing (make)"; exit 1; }
T=$(mktemp -d /tmp/rfm14crsync.XXXXXX)
FWS=$T/fws; DEV=$T/dev; CAP=$T/cap; mkdir -p "$FWS" "$DEV" "$CAP"
cp "${CR%.rfm1m}".* "$DEV/"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; CFG=$T/cfg; SAID=$T/said.log; PAIRS=$T/pairs; mkdir -p "$SIM" "$R" "$CFG"
echo "http://127.0.0.1:1" > "$R/catalog-url"
echo '{ "debounce_ms": 0 }' > "$CFG/tts.json"
PORT=17797; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
of() { printf '%s' "$1" | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
field() { of "$(status)" "$1"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
tk() { cmd "cc $1 127"; cmd "note 93 100"; cmd "note 93 0"; cmd "cc $1 0"; }   # Track n held + pad 2: slot n's takeover
mode() { field move_mode; }
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
last() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | tr '\n' '|' | cut -c1-200; }
M=0
FAKE_TTS_SHM=/rfm14-crsync-$$ FAKE_TTS_LOG=$SAID build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" \
    --firmwares "$FWS" --site build/site --dev-firmwares "$DEV" --fw choralroot --test-api --no-fm1 \
    --capture "$CAP" --capture-seconds 60 \
    --screen-reader on --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-crsync-$$ --tts-config "$CFG" >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
# the status every 0.2 s (it is made once a second, between blocks): its block count, song position and tempo
( while kill -0 $HOST 2>/dev/null; do s=$(status); b=$(of "$s" blocks); p=$(of "$s" seq_pos); t=$(of "$s" seq_bpm10)
      [ -n "$b" ] && [ -n "$p" ] && echo "$b $p $t" >> "$PAIRS"; sleep 0.2; done ) &
POLL=$!
wait_for 10 '[ "$(field seq_set10)" = 1200 ]' && sleep 2.5 || bad "no seq_set10 in the status"
tk 43
wait_for 4 '[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' && ok "Track 1 held + pad 2: ChoralRoot's takeover" \
    || bad "the takeover: mode '$(mode)' shown '$(field tk_shown)'"
sleep 1.6
cmd "note 93 100" "note 85 100"; cmd "note 69 100"; sleep 0.4    # MIN + m7 + D: Dm7
cmd "note 16 100" "note 16 0"; sleep 0.3                          # step 1: Dm7 in it (the loop)
cmd "note 69 0" "note 85 0" "note 93 0"; sleep 0.3
mark; cmd "cc 49 127"; cmd "note 21 127" "note 21 0"; cmd "cc 49 0"   # Shift + step 6: the metronome
wait_for 3 'said "Metronome"' && ok "Shift + step 6: ChoralRoot's metronome (\"$(last)\")" || bad "metronome: said '$(last)'"
sleep 1.5
s=$(status)
[ "$(of "$s" seq_playing)" = 0 ] && [ "$(of "$s" seq_following)" = 0 ] \
    && ok "stopped, the takeover up: RemoteFM1's sequencer follows no clock (its own leads)" \
    || bad "stopped: playing '$(of "$s" seq_playing)' following '$(of "$s" seq_following)'"
# ---- the takeover's Play: RemoteFM1's transport, at its tempo
cmd "cc 85 127" "cc 85 0"
wait_for 3 '[ "$(field seq_playing)" = 1 ]' && sleep 1.2 && s=$(status) && [ "$(of "$s" seq_following)" = 0 ] \
    && [ "$(of "$s" seq_bpm10)" = 1200 ] \
    && ok "Play: RemoteFM1's transport plays on its own clock, 120.0 BPM (not the firmware's)" \
    || bad "Play: playing '$(field seq_playing)' following '$(of "$s" seq_following)' bpm10 '$(of "$s" seq_bpm10)'"
sleep 4
# ---- ChoralRoot's Tempo knob (knob 8), turned while it plays: RemoteFM1's tempo
i=0; while [ $i -lt 16 ]; do cmd "cc 78 1"; i=$((i + 1)); done
sleep 1.5
s=$(status); T2=$(of "$s" seq_set10)
[ -n "$T2" ] && [ "$T2" -gt 1200 ] && [ "$(of "$s" seq_bpm10)" = "$T2" ] && [ "$(of "$s" seq_following)" = 0 ] \
    && ok "ChoralRoot's Tempo knob turned: RemoteFM1's tempo $((T2 / 10)) BPM (the takeover's request), its clock still" \
    || bad "the Tempo knob: RemoteFM1's tempo set '$T2' in use '$(of "$s" seq_bpm10)' following '$(of "$s" seq_following)'"
sleep 1.3; mark; cmd "note 7 127"; sleep 0.5; cmd "note 7 0"
wait_for 3 "said \"^Tempo $((T2 / 10))\"" && ok "knob 8 touched: ChoralRoot's tempo is RemoteFM1's (\"$(last)\")" \
    || bad "ChoralRoot's tempo: said '$(last)', RemoteFM1's $((T2 / 10))"
sleep 6
# ---- Play again: both stop; Play: both start again
cmd "cc 85 127" "cc 85 0"
wait_for 3 '[ "$(field seq_playing)" = 0 ]' && ok "Play again: RemoteFM1's transport stops" || bad "stop: seq_playing '$(field seq_playing)'"
sleep 1.5
cmd "cc 85 127" "cc 85 0"
wait_for 3 '[ "$(field seq_playing)" = 1 ]' && ok "Play: it starts again" || bad "restart: seq_playing '$(field seq_playing)'"
sleep 4
cmd "cc 85 127" "cc 85 0"
wait_for 3 '[ "$(field seq_playing)" = 0 ]' || bad "the last stop: seq_playing '$(field seq_playing)'"
sleep 1.2
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
kill $POLL 2>/dev/null; wait $POLL 2>/dev/null
rm -f "/dev/shm/rfm14-crsync-$$"
# ---- the beats against RemoteFM1's song position (the first play: before and after the tempo change)
from=$(awk '$2 >= 0 { print $1 * 128 + 44100 / 2; exit }' "$PAIRS")
to=$(awk 'BEGIN { p = 0 } $2 >= 0 { p = 1; b = $1 } p && $2 < 0 { print b * 128; exit }' "$PAIRS")
if [ -n "$from" ] && [ -n "$to" ] && [ -s "$CAP/mix.raw" ]; then
    build/tools/tksync "$CAP/mix.raw" "$PAIRS" "$from" "$to" > "$T/tksync.log"
    r=$(tail -1 "$T/tksync.log")
    n=$(echo "$r" | sed -n 's/^tksync: \([0-9]*\) onsets.*/\1/p')
    sp=$(echo "$r" | sed -n 's/.*(spread \([0-9.]*\) ticks.*/\1/p')
    beats=$(awk -v f="$from" -v t="$to" 'BEGIN { print int((t - f) / 44100 * 2) }')   # (at least 120 BPM)
    lo=$(grep -c "120.0 BPM" "$T/tksync.log"); hi=$(grep -c -v -e "120.0 BPM" -e "^tksync" "$T/tksync.log")
    [ -n "$n" ] && [ "$n" -ge $((beats * 3 / 4)) ] && [ "$lo" -ge 6 ] && [ "$hi" -ge 6 ] \
        && awk -v s="$sp" 'BEGIN { exit !(s <= 4.0) }' \
        && ok "ChoralRoot's beats on RemoteFM1's song position: ${r#tksync: } ($lo at 120 BPM, $hi after the change)" \
        || { bad "ChoralRoot's beats against RemoteFM1's song position (frames $from..$to, ~$beats beats): $r ($lo at 120, $hi after)"
             sed -n '1,400p' "$T/tksync.log" | grep -v "^tksync" | tail -30; }
else
    bad "nothing to measure: frames '$from'..'$to', mix $(wc -c < "$CAP/mix.raw" 2>/dev/null) bytes"
fi
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
mv "$T" "$T.done" 2>/dev/null
echo "cr_tk_sync_e2e: $checks checks, $fail failed (the run: $T.done)"
exit $fail
