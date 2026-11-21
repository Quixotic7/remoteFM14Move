#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Schwung's screen reader (src/announce.c, README "Screen reader") on the simulator, in the build image: the host with
# the screen reader on and tests/fake_tts.c as Schwung's speech program (the same shared memory; it logs each text it is
# asked to speak and answers with a tone): entering the tool, layout switches (rfm1cs 0.8.0: Track 1 held + a top-row
# pad, then a Track 1 tap), a knob's value (a note layout's TRIG page; the volume), the firmware view (a Track double click), a
# firmware selected and loaded from the Move, the slot played switched (from the page; a Track tap: each said once),
# the words mixed into the output, leaving (the exit gesture); the user's settings
# never restated otherwise. Then with Schwung's setting off (screen_reader_state.txt 0): the default Speech mode Off
# (rfm1cs 0.6.12), nothing said.
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
# (rfm1cs 0.8.0) Track 1 held + the top row's pad 5: slot 1's Drum layout (0.9.0: the sequencer on slot 1's track, Track 1),
# said as changed; a Track 2 tap (slot 2, empty: its FM-1 layout), then a Track 1 tap: slot 1 played in its Drum layout, named
# with its track
mark; cmd "cc 43 127"; sleep 0.2; press "note 96"; cmd "cc 43 0"
wait_for 3 'said "Layout: Drum"' && ok "Track 1 held + pad 5: \"$(last)\"" || bad "Track 1 held + pad 5: said '$(last)'"
sleep 0.5; press "cc 42"; sleep 0.5
mark; press "cc 43"
wait_for 3 'said "^Felucca, Drum layout, Track 1"' && ok "Track 2, then Track 1 tapped: \"$(last)\"" || bad "Drum layout: said '$(last)'"
sleep 0.5
mark; cmd "cc 43 127"   # (rfm1cs 0.9.0) Track 1 held, nothing else: the one menu, its layout and track named
wait_for 3 'said "^Felucca, layouts, Drum, Track 1$"' && ok "Track 1 held: \"$(last)\"" || bad "Track 1 held: said '$(last)'"
cmd "cc 43 0"; sleep 0.5
mark; cmd "cc 71 1"; sleep 0.05; cmd "cc 71 1"   # knob 1 turned (rfm1cs 0.9.0: a note layout is the sequencer, its knobs the TRIG page first: Micro)
wait_for 3 'said "^Micro: \+2$"' && ok "a knob turned in a note layout (the TRIG page): \"$(last)\"" || bad "the note layout's knob: said '$(last)'"
mark; cmd "cc 79 127"   # the volume knob, a detent down: MASTER
wait_for 3 'said "^Volume [0-9]+%$"' && ok "the volume knob: \"$(last)\"" || bad "volume: said '$(last)'"
mark; cmd "cc 43 127"; sleep 0.2; press "note 92"; cmd "cc 43 0"   # Track 1 held + pad 1: the FM-1 layout
wait_for 3 'said "Layout: FM-1"' && ok "Track 1 held + pad 1: \"$(last)\"" || bad "FM-1 layout: said '$(last)'"
sleep 0.4
mark; cmd "note 4 127"   # knob 5 touched (the FM-1's K1): its name
wait_for 3 'said "."' && ok "FM-1 mode, knob 5 touched: \"$(last)\"" || bad "knob touch: said nothing"
cmd "note 4 0"; sleep 0.4
# the firmware view (a Track 1 double click), slot 2's (a Track 2 double click: its tap leaves the view), All (step 2),
# the first pad (ChoralRoot, A-Z), jog press: loaded
sleep 0.4; mark; press "cc 43"; press "cc 43"
wait_for 3 'said "Firmwares, Slot 1, Felucca"' && ok "Track 1 double click: \"$(last)\"" || bad "the firmware view: said '$(last)'"
sleep 0.5; mark; press "cc 42"; press "cc 42"
wait_for 3 'said "Slot 2, empty"' && ok "Track 2 double click: \"$(last)\"" || bad "slot 2: said '$(last)'"
press "note 17"; sleep 0.4
mark; press "note 92"
wait_for 3 'said "^ChoralRoot"' && ok "a pad: the firmware selected, \"$(last)\"" || bad "the selection: said '$(last)'"
mark; press "cc 3"
wait_for 15 'said "ChoralRoot loaded, slot 2"' && ok "jog press: \"$(last)\"" || bad "the load: said '$(last)'"
# the slot played: slot 1 again (a Track 1 tap leaves the view), its layout said; then slot 2 from the page
mark; press "cc 43"
wait_for 3 'said "Felucca, FM-1 layout"' && ok "Track 1 in the view: \"$(last)\"" || bad "back to the layout: said '$(last)'"
sleep 0.4
mark; curl -s -o /dev/null -X POST --data '{"op":16,"a":1,"b":0}' "$U/api/edit"
wait_for 3 'said "Playing, Slot 2, ChoralRoot"' && sleep 0.8 && [ "$(count Playing)" = 1 ] \
    && ok "the page switched the slot played: \"$(last)\", once" || bad "the page's slot switch: said '$(last)'"
mark; press "cc 43"   # a Track 1 tap: slot 1 played
wait_for 3 'said "Felucca, FM-1 layout"' && sleep 0.8 && [ "$(count "Felucca, FM-1 layout")" = 1 ] \
    && ok "a Track 1 tap switched the slot played: \"$(last)\", once" || bad "Track 1: said '$(last)'"
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

# Schwung's setting off (rfm1cs 0.6.12: the default Speech mode Off, a fresh root: none chosen): the sidecar runs,
# nothing said (tests/speechmode_e2e.sh: the modes)
echo 0 > "$CFG/screen_reader_state.txt"
SAID2=$T/said-off.log
mkdir -p "$T/root-off"; echo "http://127.0.0.1:1" > "$T/root-off/catalog-url"
FAKE_TTS_SHM=/rfm14-speech-$$ FAKE_TTS_LOG=$SAID2 build/sim/remotefm1-host --root "$T/root-off" --port $PORT --sim-dir "$SIM" \
    --firmwares build/fw --site build/site --fw felucca --no-fm1 --screen-reader auto --tts-sidecar build/tools/fake-tts \
    --tts-shm /rfm14-speech-$$ --tts-config "$CFG" >>"$T/host-off.log" 2>&1 &
HOST=$!
wait_for 10 '[ "$(field session)" = 2 ]'; sleep 1; press "cc 42"; sleep 0.6
[ "$(grep -v -c '^UP enabled=' "$SAID2" 2>/dev/null)" = 0 ] && grep -q "screen reader: Schwung's setting off" "$T/host-off.log" \
    && [ "$(field speech_mode)" = 0 ] && [ "$(field speech_open)" = 0 ] \
    && ok "Schwung's screen reader off: the default Speech mode Off, nothing said" \
    || bad "off: $(head -3 "$SAID2" 2>/dev/null | tr '\n' '|'); speech_mode $(field speech_mode)"
cmd quit; wait $HOST

# Schwung 1.9's setting: shadow_config.json beside config/ says on (no screen_reader_state.txt): the host starts the
# speech program and sends the user's own settings (eSpeak, volume 65, speed 1, pitch 95), as the shim would
mv "$CFG/screen_reader_state.txt" "$T/state-off.txt"
printf '{\n  "screen_reader_enabled": true,\n  "screen_reader_engine": "espeak",\n  "screen_reader_pitch": 95,\n  "screen_reader_speed": 1,\n  "screen_reader_volume": 65,\n  "tts_debounce_ms": 50\n}\n' > "$CFG/../shadow_config.json"
SAID3=$T/said-19.log
FAKE_TTS_SHM=/rfm14-speech-$$ FAKE_TTS_LOG=$SAID3 build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" \
    --firmwares build/fw --site build/site --fw felucca --no-fm1 --screen-reader auto --tts-sidecar build/tools/fake-tts \
    --tts-shm /rfm14-speech-$$ --tts-config "$CFG" >>"$T/host-19.log" 2>&1 &
HOST=$!
wait_for 10 '[ "$(field session)" = 2 ]'
wait_for 6 'grep -q "RemoteFM1" "$SAID3" 2>/dev/null' && grep -q "screen reader: ready: eSpeak, volume 65, speed 1.00, pitch 95" "$T/host-19.log" \
    && ok "Schwung 1.9's shadow_config.json on: spoken with the user's settings (eSpeak, volume 65, speed 1, pitch 95)" \
    || bad "shadow_config.json on: $(grep 'screen reader' "$T/host-19.log" | head -3); said: $(head -3 "$SAID3" 2>/dev/null)"
cmd quit; wait $HOST
rm -f "/dev/shm/rfm14-speech-$$" "$CFG/../shadow_config.json"
echo "speech_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
