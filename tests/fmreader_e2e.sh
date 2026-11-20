#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The FM-1 screen reader protocol (src/fmreader.c, charlesvestal/fm1-reader) on the simulator, in the build image, with
# Schwung's screen reader on (tests/fake_tts.c as its speech program, as tests/speech_e2e.sh): a catalog firmware that
# has the protocol (dist/move-catalog: FoMni-1, else AMB-1, LOOM-8, ..) in slot 1 answers RemoteFM1's
# HELLO (its name and link in the status), its words file read from <root>/words/ (a test's own, kept there beforehand
# under the firmware's file name: no network in the test) and used, its first screen said; a knob turned: the
# firmware's words only (not the core's readout too), and none of the protocol's frames in the page's MIDI out (the
# firmware's notes are). Felucca (no protocol) played from slot 2: no answer within 3 s, BYE to the firmware left, the
# core's words for a knob as before. Back to slot 1: its screen said again. Then the words file neither kept nor
# reachable (--ca-dir: nothing trusted): "failed", the screen in the common rules only.
#   tests/fmreader_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
echo "fmreader_e2e"
FW=""
for id in fomni amb loom pond samsara; do
    f=$(ls dist/move-catalog/$id-[0-9]*.rfm1m 2>/dev/null | sort -V | tail -1)
    [ -n "$f" ] && LC_ALL=C grep -qa 'words/[0-9a-f]*\.json' "$f" && { FW=$f; break; }
done
if [ -z "$FW" ]; then echo "  skip  no catalog firmware with the screen reader protocol (dist/move-catalog: make catalog)"; exit 0; fi
FWID=$(basename "$FW" | sed 's/-[0-9].*//')
LINK=$(LC_ALL=C grep -ao 'https://[!-~]*words/[0-9a-f]*\.json' "$FW" | head -1)
WF=${LINK##*/}
T=$(mktemp -d /tmp/rfm14fmr.XXXXXX)
ROOT=$T/root; SIM=$T/sim; CFG=$T/cfg; FWS=$T/fws; PORT=17803; SAID=$T/said.log; LOG=$T/host.log
mkdir -p "$SIM" "$ROOT/words" "$CFG" "$FWS" "$T/noca"
cp "${FW%.rfm1m}".* build/fw/felucca-1.8.2.* "$FWS/"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
echo '{ "debounce_ms": 100 }' > "$CFG/tts.json"
printf '{"format":1,"words":[["TEMPO","tempo zz"],["VOICE #","voice zz #"],["PAGE","page zz"],["SCALE","scale zz"]]}' \
    > "$ROOT/words/$WF"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); L=$(wc -l < "$LOG"); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
last() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | tr '\n' '|' | cut -c1-260; }
logged() { tail -n +$((L + 1)) "$LOG" | grep -q "$1"; }
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":$3}" "$U/api/load"; }
turn() { cmd "cc $1 1"; sleep 0.05; cmd "cc $1 1"; sleep 0.05; cmd "cc $1 1"; }   # three detents up
start() {   # what is said into $1, then the host's own options
    _s=$1; shift
    FAKE_TTS_SHM=/rfm14-fmr-$$ FAKE_TTS_LOG=$_s build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" \
        --firmwares "$FWS" --site build/site --fw "$FWID" --no-fm1 --midi-out --screen-reader on \
        --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-fmr-$$ --tts-config "$CFG" "$@" >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 5 '[ "$(field session)" = 2 ]' || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
}
M=0; L=0
start "$SAID"
wait_for 8 '[ "$(field fmr_slot)" = 0 ]' && [ -n "$(field fmr_name)" ] \
    && ok "$FWID answered HELLO: \"$(field fmr_name)\", words $(field fmr_link)" \
    || bad "no answer from $FWID: fmr_slot '$(field fmr_slot)'; $(grep 'fm1-reader' "$LOG" | tail -2)"
[ "$(field fmr_link)" = "$LINK" ] && wait_for 3 '[ "$(field fmr_words)" = loaded ]' \
    && grep -q "fm1-reader: words $WF: 4 entries (kept)" "$LOG" && ok "its words file read from <root>/words/$WF (kept: not fetched)" \
    || bad "the words file: '$(field fmr_words)', link '$(field fmr_link)'; $(grep 'fm1-reader: words' "$LOG" | tail -1)"
wait_for 6 'said "zz"' && [ "$(field fmr_said)" -gt 0 ] && said "^RemoteFM1\. FM-1 mode, Slot 1, " \
    && ok "its first screen said in its words, after RemoteFM1's own ($(field fmr_said) SAYs): \"$(last | cut -c1-120)..\"" \
    || bad "the first screen: said '$(last)', fmr_said '$(field fmr_said)'"
sleep 1.5
mark; build/tools/wsprobe $PORT 2500 > "$T/ws.txt" 2>&1 &
WS=$!
sleep 0.5; cmd "note 4 127"; sleep 0.3; turn 75; sleep 0.3; cmd "note 4 0"; sleep 1   # knob 5 (the FM-1's K1)
if wait_for 3 'said "zz"' && ! said "^[A-Za-z0-9 ]+: [-+]?[0-9]"; then ok "K1 turned: the firmware's words only: \"$(last)\""
else bad "K1 turned: said '$(last)' (the core's readout too, or no SAY)"; fi
wait $WS
mo=$(grep "midi out" "$T/ws.txt" | sed 's/.*bytes://')
echo "$mo" | grep -q "F0 7D 53 52" && bad "the protocol's frames reached the page's MIDI out: $mo" \
    || ok "the page's MIDI out without the protocol's frames ($(echo "$mo" | wc -w) bytes: the firmware's own MIDI)"

# Felucca (no protocol) played from slot 2: no answer, BYE to slot 1, the core's knob words as before
mark; load 1 felucca 1
wait_for 15 '[ "$(field active)" = 1 ] && logged "fm1-reader: slot 2: no answer"' && [ "$(field fmr_slot)" = -1 ] \
    && logged "fm1-reader: slot 1: BYE" && ok "felucca in slot 2, played: BYE to slot 1, no answer within 3 s (fmr_slot -1)" \
    || bad "felucca: active '$(field active)', fmr_slot '$(field fmr_slot)'; $(tail -n +$((L + 1)) "$LOG" | grep 'fm1-reader' | head -3)"
sleep 0.5; mark; turn 75
wait_for 3 'said "."' && ! said "zz" && ok "a knob turned on felucca: the core's words, \"$(last)\"" || bad "felucca's knob: said '$(last)'"
sleep 0.5
mark; curl -s -o /dev/null -X POST --data '{"op":16,"a":0,"b":0}' "$U/api/edit"
wait_for 6 'said "zz" && [ "$(field fmr_slot)" = 0 ]' && ok "slot 1 played again: its screen said again: \"$(last | cut -c1-100)..\"" \
    || bad "slot 1 again: said '$(last)', fmr_slot '$(field fmr_slot)'"
cmd quit; wait $HOST

# the words file neither kept nor reachable: failed, the common rules only
mv "$ROOT/words" "$T/words-kept"
SAID=$T/said2.log; M=0; L=$(wc -l < "$LOG")
start "$SAID" --ca-dir "$T/noca"
wait_for 25 '[ "$(field fmr_words)" = failed ]' && [ ! -e "$ROOT/words/$WF" ] && [ ! -e "$ROOT/words/$WF.part" ] \
    && ok "the words file not reachable: fmr_words failed, nothing kept ($(tail -n +$((L + 1)) "$LOG" | grep -m1 'fm1-reader: https' | cut -c1-110))" \
    || bad "unreachable: fmr_words '$(field fmr_words)'; $(tail -n +$((L + 1)) "$LOG" | grep 'fm1-reader' | tail -2)"
wait_for 8 '[ "$(field fmr_said)" -gt 0 ]' && sleep 0.5 && ! said "zz" && said "[Tt]empo [0-9]|[Vv]oice [0-9]" \
    && ok "said in the common rules only: \"$(last | cut -c1-120)..\"" || bad "without words: said '$(last)'"
cmd quit; wait $HOST
rm -f "/dev/shm/rfm14-fmr-$$"
echo "fmreader_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
