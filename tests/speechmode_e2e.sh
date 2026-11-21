#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# RemoteFM1's own Speech mode (rfm1cs 0.6.12, Global Settings: Off / On / Jog) on the simulator, in the build image,
# with tests/fake_tts.c as Schwung's speech program (as tests/speech_e2e.sh). Schwung's setting off (its sidecar too,
# as the real one loads screen_reader_state.txt 0): the sidecar runs, the default mode is Off, entering says nothing;
# Global Settings (rfm1cs 0.9.0: Track 4 held + step 2) set to On: "Speech on" and words after it, the sidecar's state file
# put back as it was; Jog: "Speech jog wheel", then nothing without a touch, a jog touch says the screen, words while
# held, nothing once let go. The mode is kept (the settings) across a restart. Schwung's setting on, a fresh root:
# the default On, entering said as before.
#   tests/speechmode_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14spm.XXXXXX)
ROOT=$T/root; SIM=$T/sim; CFG=$T/cfg; PORT=17809; SAID=$T/said.log; LOG=$T/host.log
mkdir -p "$SIM" "$ROOT" "$T/root2" "$CFG"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
echo "http://127.0.0.1:1" > "$T/root2/catalog-url"
echo '{ "debounce_ms": 100 }' > "$CFG/tts.json"
echo 0 > "$CFG/screen_reader_state.txt"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
# the words said since mark (the fake's own lines left out: UP, ENABLED, DOWN)
words() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -v -E '^(UP enabled=|ENABLED |DOWN$)'; }
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { words | grep -E -q "$1"; }
quiet() { [ -z "$(words)" ]; }
last() { words | tr '\n' '|' | cut -c1-200; }
start() {   # $1 the log said into, $2 the root, $3 the sidecar's own setting (FAKE_TTS_ENABLED)
    FAKE_TTS_SHM=/rfm14-spm-$$ FAKE_TTS_LOG=$1 FAKE_TTS_ENABLED=$3 FAKE_TTS_STATE=$CFG/screen_reader_state.txt \
        build/sim/remotefm1-host --root "$2" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site \
        --fw felucca --no-fm1 --screen-reader auto --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-spm-$$ \
        --tts-config "$CFG" >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 5 '[ "$(field session)" = 2 ]' || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
}
jog() { cmd "cc 14 $1"; sleep 0.3; }
click() { press "cc 3"; sleep 0.2; }

echo "speechmode_e2e"
M=0
start "$SAID" "$ROOT" 0
wait_for 5 'grep -q "^UP enabled=0" "$SAID" 2>/dev/null' && ok "Schwung's setting off: the sidecar runs all the same" \
    || bad "no sidecar: $(grep 'screen reader' "$LOG" | head -3)"
sleep 3
quiet && [ "$(field speech_mode)" = 0 ] && [ "$(field speech_open)" = 0 ] \
    && ok "the default Off (speech_mode 0, speech_open 0): entering said nothing" \
    || bad "entering, Off: said '$(last)', speech_mode '$(field speech_mode)', open '$(field speech_open)'"
mark; cmd "cc 40 127"; sleep 0.1; press "note 17"; cmd "cc 40 0"; sleep 0.6   # Track 4 held + step 2 (rfm1cs 0.9.0: the Shift shortcuts in every layout): Global Settings
quiet && ok "Off: Global Settings opened in silence" || bad "Off: said '$(last)'"
mark; click; jog 1   # the Speech row (the first): edit, one step up: On
wait_for 3 'said "^Speech on$"' && wait_for 3 '[ "$(field speech_mode)" = 1 ] && [ "$(field speech_open)" = 1 ]' \
    && ok "Global Settings, Speech set to On: \"$(last)\"" || bad "On: said '$(last)', speech_mode '$(field speech_mode)'"
mark; click
wait_for 3 'said "Done"' && ok "On: words come (\"$(last)\")" || bad "On, the edit closed: said '$(last)'"
wait_for 3 '[ "$(cat "$CFG/screen_reader_state.txt")" = 0 ]' && grep -q "^ENABLED 1" "$SAID" \
    && grep -q "screen_reader_state.txt put back" "$LOG" \
    && ok "the sidecar enabled by the request; Schwung's screen_reader_state.txt put back to 0" \
    || bad "the state file: '$(cat "$CFG/screen_reader_state.txt")'; $(grep 'ENABLED' "$SAID" | head -2)"
grep -q "SETTINGS CHANGED" "$SAID" && bad "the user's settings changed: $(grep -m1 'SETTINGS CHANGED' "$SAID")" \
    || ok "the user's engine, volume, speed and pitch only restated"
mark; click; sleep 0.4; jog 1   # edit again, up: Jog
wait_for 3 'said "^Speech jog wheel$"' && wait_for 3 '[ "$(field speech_mode)" = 2 ] && [ "$(field speech_open)" = 0 ]' \
    && ok "Speech set to Jog: \"Speech jog wheel\" said; speech_open 0" \
    || bad "Jog: said '$(last)', speech_mode '$(field speech_mode)', open '$(field speech_open)'"
sleep 0.5
mark; click; press "cc 51"; sleep 0.8   # Done, Back: the menu closed (the FM-1 layout under it)
quiet && ok "Jog, the jog not touched: the edit closed and the menu closed in silence (the FM-1 layout)" \
    || bad "Jog untouched: said '$(last)'"
mark; cmd "note 9 127"   # the jog touched
wait_for 3 'said "FM-1 layout"' && wait_for 3 '[ "$(field speech_open)" = 1 ]' && ok "a jog touch says the screen: \"$(last)\"" \
    || bad "the jog touch: said '$(last)', open '$(field speech_open)'"
sleep 0.4
mark; press "cc 41"   # Track 3 while held: slot 3 (empty) played, its FM-1 layout
wait_for 3 'said "^Track 3, FM-1 layout"' && ok "words while the jog is held: \"$(last)\"" || bad "held: said '$(last)'"
cmd "note 9 0"; sleep 0.3
mark; press "cc 42"; sleep 1   # let go, Track 2: slot 2 played
quiet && wait_for 3 '[ "$(field speech_open)" = 0 ]' && ok "the jog let go: Track 2 (slot 2 played) said nothing" || bad "let go: said '$(last)'"
cmd quit; wait $HOST

# the mode kept: the same root again (Schwung's setting still off)
mark; start "$SAID" "$ROOT" 0
sleep 2
[ "$(field speech_mode)" = 2 ] && quiet && ok "restarted: Jog kept (the settings), entering silent" \
    || bad "restarted: speech_mode '$(field speech_mode)', said '$(last)'"
cmd quit; wait $HOST
[ "$(cat "$CFG/screen_reader_state.txt")" = 0 ] && ok "Schwung's screen_reader_state.txt still 0 at the end" \
    || bad "the state file at the end: '$(cat "$CFG/screen_reader_state.txt")'"

# Schwung's setting on, a fresh root: the default On, as before 0.6.12
echo 1 > "$CFG/screen_reader_state.txt"
SAID2=$T/said-on.log; M=0
start "$SAID2" "$T/root2" 1
SAID=$SAID2
wait_for 6 'said "RemoteFM1" && said "FM-1 mode"' && [ "$(field speech_mode)" = 1 ] \
    && ok "Schwung's setting on: the default On, entering said (\"$(words | head -1)\")" \
    || bad "Schwung on: said '$(last)', speech_mode '$(field speech_mode)'"
cmd quit; wait $HOST
rm -f "/dev/shm/rfm14-spm-$$"
echo "speechmode_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
