#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Schwung's screen reader (src/announce.c, README "Screen reader") on the simulator, in the build image: the host with
# the screen reader on and tests/fake_tts.c as Schwung's speech program (the same shared memory; it logs each text it is
# asked to speak and answers with a tone): entering the tool, mode switches, a knob's value (Control mode's CC; the
# volume), the firmware view, a firmware selected and loaded from the Move, the slot played switched (from the page;
# Back + Track: each said once), the words mixed into the output, leaving (the exit gesture); the user's settings
# never restated otherwise. Then with Schwung's setting off (screen_reader_state.txt 0): no sidecar, nothing said.
#   tests/speech_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14speech.XXXXXX)
ROOT=$T/root; SIM=$T/sim; CFG=$T/cfg; PORT=17791; SAID=$T/said.log
mkdir -p "$SIM" "$ROOT" "$CFG"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
echo '{ "debounce_ms": 200 }' > "$CFG/tts.json"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
# a line said since mark (the log's line count when it was taken) matching an extended regexp
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
count() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -c "$1"; }
last() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | tr '\n' '|' | cut -c1-200; }
LOG=$T/host.log

echo "speech_e2e"
M=0
FAKE_TTS_SHM=/rfm14-speech-$$ FAKE_TTS_LOG=$SAID build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" \
    --firmwares build/fw --site build/site --fw felucca --no-fm1 --screen-reader on --tts-sidecar build/tools/fake-tts \
    --tts-shm /rfm14-speech-$$ --tts-config "$CFG" >>"$LOG" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' && wait_for 5 '[ "$(field session)" = 2 ]' \
    || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
grep -q "^UP enabled=1" "$SAID" 2>/dev/null && ok "Schwung's speech program started as the host's child, its settings published" \
    || bad "no sidecar: $(cat "$SAID" 2>/dev/null | head -3); $(grep 'screen reader' "$LOG" | head -3)"
wait_for 6 'said "RemoteFM1" && said "FM-1 mode"' && ok "entering: \"$(grep -m1 RemoteFM1 "$SAID")\"" \
    || bad "entering: said '$(last)'"
wait_for 10 '[ "$(slotf 0 id)" = felucca ]'; sleep 1
mark; press "cc 42"   # a tap on Track 2: MIDI mode
wait_for 3 'said "^MIDI mode"' && ok "Track 2 tapped: \"$(last)\"" || bad "MIDI mode: said '$(last)'"
mark; press "cc 41"   # Track 3: Control mode
wait_for 3 'said "^Control mode"' && ok "Track 3 tapped: \"$(last)\"" || bad "Control mode: said '$(last)'"
sleep 0.5
mark; cmd "cc 71 1"; sleep 0.05; cmd "cc 71 1"   # knob 1 turned: its CC's value
wait_for 3 'said "^CC [0-9]+: [0-9]+$"' && ok "a knob turned in Control mode: \"$(last)\"" || bad "Control knob: said '$(last)'"
mark; cmd "cc 79 127"   # the volume knob, a detent down: MASTER
wait_for 3 'said "^Volume [0-9]+%$"' && ok "the volume knob: \"$(last)\"" || bad "volume: said '$(last)'"
mark; press "cc 43"   # Track 1: FM-1 mode
wait_for 3 'said "^FM-1 mode"' && ok "Track 1 tapped: \"$(last)\"" || bad "FM-1 mode: said '$(last)'"
sleep 0.4
mark; cmd "note 4 127"   # knob 5 touched (the FM-1's K1): its name
wait_for 3 'said "."' && ok "FM-1 mode, knob 5 touched: \"$(last)\"" || bad "knob touch: said nothing"
cmd "note 4 0"; sleep 0.4
# the firmware view (Back), slot 2 (Track 2), All (step 2), the first pad (ChoralRoot, A-Z), jog press: loaded
mark; press "cc 51"
wait_for 3 'said "^Firmwares, Slot 1, Felucca"' && ok "Back: \"$(last)\"" || bad "the firmware view: said '$(last)'"
mark; press "cc 42"
wait_for 3 'said "Slot 2, empty"' && ok "Track 2 in the view: \"$(last)\"" || bad "slot 2: said '$(last)'"
press "note 17"; sleep 0.4
mark; press "note 92"
wait_for 3 'said "^ChoralRoot"' && ok "a pad: the firmware selected, \"$(last)\"" || bad "the selection: said '$(last)'"
mark; press "cc 3"
wait_for 15 'said "ChoralRoot loaded, slot 2"' && ok "jog press: \"$(last)\"" || bad "the load: said '$(last)'"
# the slot played: slot 1 again (Track 1 in the view), then the mode (Back), then slot 2 from the page
mark; press "cc 43"; sleep 0.5; press "cc 51"
wait_for 3 'said "FM-1 mode"' && ok "Track 1 in the view, Back: \"$(last)\"" || bad "back to the mode: said '$(last)'"
sleep 0.4
mark; curl -s -o /dev/null -X POST --data '{"op":16,"a":1,"b":0}' "$U/api/edit"
wait_for 3 'said "^Playing, Slot 2, ChoralRoot"' && sleep 0.8 && [ "$(count Playing)" = 1 ] \
    && ok "the page switched the slot played: \"$(last)\", once" || bad "the page's slot switch: said '$(last)'"
mark; cmd "cc 51 127"; sleep 0.15; press "cc 43"; cmd "cc 51 0"   # Back held + Track 1: slot 1 played
wait_for 3 'said "Playing, Slot 1, Felucca"' && sleep 0.8 && [ "$(count Playing)" = 1 ] \
    && ok "Back + Track 1 switched the slot played: \"$(last)\", once" || bad "Back + Track 1: said '$(last)'"
mx=$(field tts_mixed)
[ "${mx:-0}" -gt 1000 ] && [ "$(field tts_on)" = 1 ] && ok "the words mixed into the Move's output ($mx frames; $(field tts_said) utterances)" \
    || bad "mixed: '$mx' frames, tts_on '$(field tts_on)'"
# leaving: the exit gesture (Shift + the volume knob touched + jog click)
mark; cmd "cc 49 127"; sleep 0.05; cmd "note 8 127"; sleep 0.05; cmd "cc 3 127"
wait_for 10 '! kill -0 $HOST 2>/dev/null'
said "^Leaving RemoteFM1$" && ok "the exit gesture: \"Leaving RemoteFM1\" asked for before the end" || bad "leaving: said '$(last)'"
grep -q "^DOWN" "$SAID" && ok "the sidecar stopped with the host" || bad "the sidecar did not stop"
grep -q "SETTINGS CHANGED" "$SAID" && bad "the host changed the user's settings: $(grep -m1 'SETTINGS CHANGED' "$SAID")" \
    || ok "the user's speech settings only restated, never changed"
wait $HOST 2>/dev/null

# Schwung's setting off: no sidecar, nothing said
echo 0 > "$CFG/screen_reader_state.txt"
SAID2=$T/said-off.log
FAKE_TTS_SHM=/rfm14-speech-$$ FAKE_TTS_LOG=$SAID2 build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" \
    --firmwares build/fw --site build/site --fw felucca --no-fm1 --screen-reader auto --tts-sidecar build/tools/fake-tts \
    --tts-shm /rfm14-speech-$$ --tts-config "$CFG" >>"$T/host-off.log" 2>&1 &
HOST=$!
wait_for 10 '[ "$(field session)" = 2 ]'; sleep 1; press "cc 42"; sleep 0.6
[ ! -e "$SAID2" ] && grep -q "screen reader: off (Schwung's setting)" "$T/host-off.log" && [ "$(field tts_on)" = 0 ] \
    && ok "Schwung's screen reader off: no sidecar, nothing said" || bad "off: $(head -2 "$SAID2" 2>/dev/null); tts_on $(field tts_on)"
cmd quit; wait $HOST
rm -f "/dev/shm/rfm14-speech-$$"
echo "speech_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
