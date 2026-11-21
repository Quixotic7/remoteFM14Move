#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# ChoralRoot's own takeover, "ChoralRoot" (tk1: FM1VST docs/RFM1-TAKEOVER.md; ChoralRoot's firmware/src/cr_move.c), in the
# build image after make with a tk1 build of ChoralRoot among the development builds (make DEV_FIRMWARES=
# .../choralroot-<v>.rfm1, FM1VST's core-glue/choralroot: build/fw-dev). Skipped when there is none (no fixture is kept
# in git: the .rfm1 is 1.6 MB; tkdemo, tests/tk_e2e.sh, is the takeover's own fixture). The host on the simulator, the
# screen reader on (tests/fake_tts.c): ChoralRoot in slot 1, its record read (claims 0x5F); Track 1 opens it (said
# "ChoralRoot, takeover"), its home screen (the header bar, the 16-step strip); the pads its colours (the Keys layout, the chord keys,
# the memories); MIN + m7 + D: the pads lit, "Chord D minor seven" said; memory 6 tapped with them held: saved (white,
# then cyan; "Memory 6, D minor seven"); a step tapped: the chord in it (the strip); Menu held: the picker (its pads),
# Scale picked; a knob touched: its full-screen popup; volume touched + Back: FM-1 mode, Track 1: back as it was left.
#   tests/cr_tk_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "cr_tk_e2e"
CR=$(ls build/fw-dev/choralroot-*.rfm1m 2>/dev/null | sort -V | tail -1)
if [ -z "$CR" ] || ! nm -D "$CR" | grep -q rfm1m_tk1; then
    echo "  skip  no tk1 build of ChoralRoot in build/fw-dev (make DEV_FIRMWARES=.../choralroot-<v>.rfm1)"; exit 0
fi
T=$(mktemp -d /tmp/rfm14crtk.XXXXXX)
FWS=$T/fws; DEV=$T/dev; mkdir -p "$FWS" "$DEV"
cp "${CR%.rfm1m}".* "$DEV/"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; CFG=$T/cfg; SAID=$T/said.log; mkdir -p "$SIM" "$R" "$CFG"
echo "http://127.0.0.1:1" > "$R/catalog-url"
echo '{ "debounce_ms": 0 }' > "$CFG/tts.json"
PORT=17796; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotrec() { status | sed 's/.*"slots":\[//; s/\],"threads".*//; s/null/{"id":"-"}/g' | awk -v RS='[{]"id":' -v n="$1" 'NR == n + 2 { print "\"id\":" $0 }'; }
sfield() { slotrec "$1" | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
padrgb() { field move_pads | cut -c$(($1 * 6 + 1))-$(($1 * 6 + 6)); }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
rows() { tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | sed -n "$(($1 + 1)),$(($2 + 1))p"; }
lit() { rows "$1" "$2" | tr -cd 1 | wc -c; }
mode() { field move_mode; }
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
last() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | tr '\n' '|' | cut -c1-200; }
# the takeover's home view before a chord (proposal 3): a dash in the middle (24 x 5 from row 28), nothing else
home() { [ "$(lit 28 32)" = 120 ] && [ "$(lit 0 27)" = 0 ] && [ "$(lit 33 63)" = 0 ]; }
M=0
FAKE_TTS_SHM=/rfm14-crtk-$$ FAKE_TTS_LOG=$SAID build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" \
    --firmwares "$FWS" --site build/site --dev-firmwares "$DEV" --fw choralroot --test-api --no-fm1 \
    --screen-reader on --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-crtk-$$ --tts-config "$CFG" >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
wait_for 10 '[ "$(sfield 0 tk)" = 95 ]' && [ "$(sfield 0 tk_name)" = ChoralRoot ] && [ "$(mode)" = 0 ] \
    && ok "$(basename "$CR") in slot 1: its record read (claims 0x5F, \"ChoralRoot\"), FM-1 mode" \
    || bad "ChoralRoot's takeover: claims '$(sfield 0 tk)' name '$(sfield 0 tk_name)' mode '$(mode)'"
sleep 2.5   # (the firmware's splash and the session's own popups gone)
mkdir -p build/test-out
# ---- Track 1: the takeover
mark; press 43
wait_for 4 '[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' && ok "Track 1: the takeover (mode 4, slot 1 shown)" \
    || bad "Track 1: mode '$(mode)' shown '$(field tk_shown)'"
wait_for 3 'said "^ChoralRoot"' && ok "entering said: \"$(last)\"" || bad "entering: said '$(last)'"
sleep 1.6   # (its name popup gone)
shot; cp "$SIM/mr-screen.pbm" build/test-out/crtk_home.pbm
home && ok "the Move's screen is the takeover's home view (no chord yet: a dash)" \
    || bad "not the takeover's home: rows 28-32 $(lit 28 32), 0-27 $(lit 0 27), 33-63 $(lit 33 63) lit (build/test-out/crtk_home.pbm)"
[ "$(padrgb 0)" = ffd228 ] && [ "$(padrgb 1)" = 515151 ] && [ "$(padrgb 25)" = 361047 ] && [ "$(padrgb 17)" = 472100 ] \
    && [ "$(padrgb 21)" = 031e22 ] && ok "the pads: Keys (C yellow, D grey), MIN violet, m7 orange, memory 6 empty (dim)" \
    || bad "the pads: C '$(padrgb 0)' D '$(padrgb 1)' MIN '$(padrgb 25)' m7 '$(padrgb 17)' memory 6 '$(padrgb 21)'"
# ---- MIN + m7 held, D: Dm7; memory 6 tapped meanwhile: saved
mark; cmd "note 93 100" "note 85 100"; cmd "note 69 100"
wait_for 3 '[ "$(padrgb 25)" = ffffff ] && [ "$(padrgb 1)" = ffffff ]' && ok "MIN, m7 and D held: lit white" \
    || bad "held: MIN '$(padrgb 25)' D '$(padrgb 1)'"
wait_for 3 'said "^Chord D minor seven"' && ok "said: \"$(last)\"" || bad "the chord: said '$(last)'"
mark; cmd "note 89 100" "note 89 0"
wait_for 3 'said "^Memory 6, D minor seven"' && ok "memory 6 tapped with D held: saved, said \"$(last)\"" \
    || bad "memory 6: said '$(last)'"
cmd "note 69 0" "note 85 0" "note 93 0"
wait_for 3 '[ "$(padrgb 21)" = 13abbf ]' && ok "memory 6: cyan, it holds a chord" || bad "memory 6's LED '$(padrgb 21)'"
sleep 1.3
# ---- a step tapped: the last chord goes in
cmd "note 16 100" "note 16 0"
sleep 0.4; mark; cmd "note 16 100"; sleep 0.7; shot; cp "$SIM/mr-screen.pbm" build/test-out/crtk_step.pbm; cmd "note 16 0"
[ "$(lit 17 48)" -gt 150 ] && [ "$(lit 50 63)" = 0 ] && ! said "empty" \
    && ok "step 1 tapped: Dm7 in it (held: the step view, its chord big, \"$(last)\")" \
    || bad "step 1 held: rows 17-48 $(lit 17 48), 50-63 $(lit 50 63) lit, said '$(last)' (build/test-out/crtk_step.pbm)"
# ---- Menu held: the picker; Scale picked
mark; cmd "cc 50 127"; sleep 0.5
wait_for 3 '[ "$(padrgb 24)" = ffffff ] && [ "$(padrgb 25)" = 727272 ]' \
    && ok "Menu held: the picker (Keys now white, Scale the previous)" || bad "picker: '$(padrgb 24)' '$(padrgb 25)'"
cmd "note 93 100" "note 93 0" "cc 50 0"
wait_for 3 'said "^Layout Scale"' && wait_for 3 '[ "$(padrgb 8)" = ffd228 ] && [ "$(padrgb 1)" = 515151 ]' \
    && ok "Menu + Scale: the key's degrees, the tonic yellow each octave (\"$(last)\")" \
    || bad "Scale: said '$(last)', pad 8 '$(padrgb 8)'"
# ---- a knob touched: its popup, full screen
sleep 1.3; mark; cmd "note 0 127"; sleep 0.2; shot; cp "$SIM/mr-screen.pbm" build/test-out/crtk_knob.pbm
[ "$(lit 0 0)" -lt 128 ] && [ "$(lit 20 52)" -gt 100 ] && said "^Voicing" \
    && ok "knob 1 touched: \"$(last)\", its popup full screen" || bad "knob 1: row 0 $(lit 0 0), said '$(last)'"
cmd "note 0 0"; sleep 1.3
# ---- volume touched + Back: FM-1 mode; Track 1: as it was left
cmd "note 8 127"; cmd "cc 51 127"; cmd "cc 51 0"; cmd "note 8 0"
wait_for 4 '[ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ]' && ok "volume touched + Back: FM-1 mode" \
    || bad "volume + Back: mode '$(mode)' shown '$(field tk_shown)'"
press 43
wait_for 4 '[ "$(mode)" = 4 ]' && wait_for 3 '[ "$(padrgb 8)" = ffd228 ] && [ "$(padrgb 21)" = 13abbf ]' \
    && ok "Track 1 again: the takeover as left (Scale, memory 6)" || bad "re-entered: pad 8 '$(padrgb 8)' memory 6 '$(padrgb 21)'"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
rm -f "/dev/shm/rfm14-crtk-$$"
mv "$T" "$T.done" 2>/dev/null
echo "cr_tk_e2e: $checks checks, $fail failed"
exit $fail
