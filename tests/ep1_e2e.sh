#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Engine parameters (ep1: engine-params/SPEC.md; FM1VST docs/RFM1.md "Engine parameters (ep1)"; rfm1cs 0.14.0's ENGINE
# knob page), in the build image after make with an ep1 build of ChoralRoot among the development builds (make
# DEV_FIRMWARES=.../choralroot-<v>.rfm1, FM1VST's core-glue/choralroot built WANTS ep1: build/fw-dev). Skipped when there
# is none. The host on the simulator, ChoralRoot in slot 1 (its audio and input captured):
#   - its record read at boot: the description reaches the core (2 layers, the chord on FM6, the bass off);
#   - the page's FM-1 panel (the WebSocket): OPT held + PRESETS two detents (FM6 -> FM TONE -> VA), ALGORITHM a detent
#     (the bass on): the layers follow (the chord VA on channel 1, the bass VA on channel 2);
#   - Track 1 held + pad 3 (the sequencer), the jog two detents: TRIG -> FM-1 -> ENGINE, the core watches the VA's first
#     page (OSC 1) of the chord layer; the jog's click: the page list, four detents: FILTER watched (the knobs follow the
#     highlight); the click: closed;
#   - knob 1 turned three ticks: Cutoff's words change in the record (the firmware's own words: the watched cells);
#     knob 4 (Env amt, hundredths, a whole number a tick): a tick +1.00, Shift + a tick +0.01;
#   - Track 1 held + step 2: track 2 (MIDI channel 2), the jog to ENGINE: the bass layer watched, on the VA's FILTER (the
#     page last shown on that engine); its Cutoff turned, the chord's untouched;
#   - track 1 again, its FILTER page shown: OPT + PRESETS back a detent (VA -> FM TONE) while the page shows: the core
#     follows to FM TONE's FILTER (the page by name);
#   - no late blocks; ChoralRoot's blocks are what tools/fwrun renders from the host's log of its input (FWI_EP among
#     them, before the notes of their block), bit for bit.
#   tests/ep1_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "ep1_e2e"
CR=$(ls build/fw-dev/choralroot-*.rfm1m 2>/dev/null | sort -V | tail -1)
if [ -z "$CR" ] || ! nm -D "$CR" | grep -q rfm1m_ep1; then
    echo "  skip  no ep1 build of ChoralRoot in build/fw-dev (make DEV_FIRMWARES=.../choralroot-<v>.rfm1)"; exit 0
fi
T=$(mktemp -d /tmp/rfm14ep1.XXXXXX)
FWS=$T/fws; DEV=$T/dev; mkdir -p "$FWS" "$DEV"
cp "${CR%.rfm1m}".* "$DEV/"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; CAP=$T/cap; mkdir -p "$SIM" "$R" "$CAP"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17793; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
ep() { status | grep -o '"ep1":{[^}]*}' | head -1; }
epf() { ep | grep -o "\"$1\":[^,]*" | head -1 | cut -d: -f2; }
layers() { ep | grep -o '"layers":\[[][0-9,-]*\]' | sed 's/"layers"://'; }
cell() { ep | sed 's/.*"cells":\[\[//; s/\]\]}$//' | sed 's/\],\[/\n/g' | sed -n "$(($1 + 1))p"; }   # k: pid,"name","text"
ctext() { cell "$1" | sed 's/.*,"\([^"]*\)"$/\1/'; }
cname() { cell "$1" | sed 's/^[0-9]*,"\([^"]*\)",.*/\1/'; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.12; _i=$((_i + 1)); done; }   # n detents, 1 / 127
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
lit() { tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | sed -n "$(($1 + 1)),$(($2 + 1))p" | tr -cd 1 | wc -c; }
build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site \
    --dev-firmwares "$DEV" --fw choralroot --test-api --no-fm1 --capture "$CAP" --capture-seconds 120 >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
# ---- the record at boot
wait_for 10 '[ -n "$(epf desc)" ]' && [ "$(epf desc)" -gt 1000 ] && [ "$(layers)" = "[[0,0],[-1,1]]" ] \
    && ok "$(basename "$CR"): its description ($(epf desc) bytes) reached the core; the chord on FM6 (channel 1), the bass off" \
    || bad "the record at boot: desc '$(epf desc)' layers '$(layers)'"
sleep 2.5   # (the firmware's splash gone: its knobs are taken)
# ---- the FM-1 panel (the page's WebSocket): OPT (GLO) held + PRESETS two detents, ALGORITHM one: VA, the bass on
INFO=$(curl -s "$U/api/info")
GLO=$(echo "$INFO" | grep -o '"buttons":\[[]["A-Za-z0-9 +,./-]*\]' | sed 's/"buttons":\[\[//; s/\]\]$//' | sed 's/\],\[/\n/g' | sed -n 6p | cut -d, -f2)
encid() { echo "$INFO" | grep -o '"enc":\[[][0-9,-]*\]' | sed 's/"enc":\[\[//; s/\]\]$//' | sed 's/\],\[/\n/g' | sed -n "$(($1 + 1))p"; }
PRE=$(encid 2 | cut -d, -f1); PRED=$(encid 2 | cut -d, -f2); ALG=$(encid 1 | cut -d, -f1); ALGD=$(encid 1 | cut -d, -f2)
[ "$PRED" = 0 ] && PRED=1; [ "$ALGD" = 0 ] && ALGD=1
build/tools/wsprobe $PORT 2400 --op "3:$GLO:1@200" --op "4:$PRE:$PRED@450" --op "4:$PRE:$PRED@700" --op "3:$GLO:0@1100" \
    --op "4:$ALG:$ALGD@1700" >/dev/null
wait_for 4 '[ "$(layers)" = "[[2,0],[2,1]]" ]' \
    && ok "OPT + PRESETS two detents and ALGORITHM (the FM-1 panel): the chord on VA, the bass on VA, channels 1 and 2" \
    || bad "the engine picker: layers '$(layers)' (GLO $GLO, PRESETS $PRE/$PRED, ALGO $ALG/$ALGD)"
sleep 1.5
mkdir -p build/test-out
# ---- the sequencer, the jog to ENGINE: the chord layer's VA, its first page watched
cmd "cc 43 127"; sleep 0.4; cmd "note 94 100"; sleep 0.08; cmd "note 94 0"; cmd "cc 43 0"
wait_for 4 '[ "$(field move_mode)" = 3 ]' && ok "Track 1 held + pad 3: the sequencer (mode 3)" || bad "mode '$(field move_mode)'"
sleep 1.4
w0=$(epf watches)
jog 2 1
wait_for 3 '[ "$(epf wpage)" = 40 ] && [ "$(epf wlayer)" = 0 ]' && [ "$(cname 0)" = Wave ] \
    && ok "the jog two detents: ENGINE, the chord's VA, page 40 (OSC 1: \"$(cname 0)\" $(ctext 0)) watched (EP_WATCH $w0 -> $(epf watches))" \
    || bad "ENGINE: wpage '$(epf wpage)' wlayer '$(epf wlayer)' cell 1 '$(cell 0)'"
sleep 0.2; shot; cp "$SIM/mr-screen.pbm" build/test-out/ep1_arrive.pbm
[ "$(lit 0 63)" -gt 150 ] && ok "arriving: the page's popup on the Move's screen (build/test-out/ep1_arrive.pbm)" \
    || bad "arriving: $(lit 0 63) pixels lit"
sleep 1.3
# ---- the page list: the jog's click, four detents (the knobs follow the highlight: FILTER watched), the click
press 3; sleep 0.3
jog 4 1
wait_for 3 '[ "$(epf wpage)" = 44 ] && [ "$(cname 0)" = Cutoff ]' \
    && ok "the page list: four detents, the knobs follow the highlight at once (FILTER watched: \"$(cname 0)\" $(ctext 0))" \
    || bad "the list: wpage '$(epf wpage)' cell 1 '$(cell 0)'"
shot; cp "$SIM/mr-screen.pbm" build/test-out/ep1_list.pbm
press 3; sleep 0.4; shot
[ "$(epf wpage)" = 44 ] && ok "the click: the list closed on FILTER" || bad "closed: wpage '$(epf wpage)'"
# ---- knob 1 (Cutoff): three ticks; knob 4 (Env amt): a tick, then Shift + a tick
c0=$(ctext 0); s0=$(epf sets)
cmd "note 0 127"; sleep 0.05; cmd "cc 71 1"; sleep 0.05; cmd "cc 71 1"; sleep 0.05; cmd "cc 71 1"; sleep 0.3
shot; cp "$SIM/mr-screen.pbm" build/test-out/ep1_knob.pbm; cmd "note 0 0"
wait_for 3 '[ "$(ctext 0)" != "$c0" ]' && [ "$(epf sets)" -gt "$s0" ] && [ "$(epf refused)" = 0 ] \
    && ok "knob 1 turned: Cutoff $c0 -> $(ctext 0) in the firmware (sets $s0 -> $(epf sets), none refused)" \
    || bad "knob 1: Cutoff '$c0' -> '$(ctext 0)', sets $s0 -> '$(epf sets)', refused '$(epf refused)'"
[ "$(lit 0 63)" -gt 150 ] && ok "the turn's popup on the Move's screen (build/test-out/ep1_knob.pbm)" || bad "the turn's popup: $(lit 0 63) lit"
sleep 1.2
e0=$(ctext 3)
cmd "note 3 127"; sleep 0.05; cmd "cc 74 1"
wait_for 3 '[ "$(ctext 3)" != "$e0" ]'; e1=$(ctext 3)
cmd "cc 49 127"; sleep 0.05; cmd "cc 74 1"
wait_for 3 '[ "$(ctext 3)" != "$e1" ]'; cmd "cc 49 0"; cmd "note 3 0"
e2=$(ctext 3)
d1=$(awk -v a="$e0" -v b="$e1" 'BEGIN { printf "%.2f", b - a }'); d2=$(awk -v a="$e1" -v b="$e2" 'BEGIN { printf "%.2f", b - a }')
[ "$(cname 3)" = "Env amt" ] && [ "$d1" = 1.00 ] && [ "$d2" = 0.01 ] \
    && ok "knob 4 (Env amt): a tick $e0 -> $e1 (a whole number), Shift + a tick -> $e2 (a hundredth)" \
    || bad "knob 4: '$(cname 3)' $e0 -> $e1 -> $e2 ($d1, $d2)"
sleep 1.2
cf=$(ctext 0)
# ---- track 2 (Track 1 held + step 2): MIDI channel 2, the bass layer
cmd "cc 43 127"; sleep 0.3; cmd "note 17 100"; sleep 0.08; cmd "note 17 0"; cmd "cc 43 0"; sleep 0.6
jog 2 1
wait_for 3 '[ "$(epf wlayer)" = 1 ] && [ "$(epf wpage)" = 44 ]' \
    && ok "track 2 (channel 2): ENGINE on the bass layer (Auto by channel), on the VA's FILTER (the page last shown on that engine)" \
    || bad "track 2: wlayer '$(epf wlayer)' wpage '$(epf wpage)'"
sleep 1.3
b0=$(ctext 0)
cmd "note 0 127"; sleep 0.05; cmd "cc 71 127"; sleep 0.05; cmd "cc 71 127"; sleep 0.4; cmd "note 0 0"
wait_for 3 '[ "$(ctext 0)" != "$b0" ]' && [ "$(epf wlayer)" = 1 ] \
    && ok "the bass's Cutoff turned down: $b0 -> $(ctext 0)" || bad "the bass's Cutoff: '$b0' -> '$(ctext 0)'"
sleep 1.2
# ---- track 1 again: its FILTER page; the engine switched under it (OPT + PRESETS back: FM TONE)
cmd "cc 43 127"; sleep 0.3; cmd "note 16 100"; sleep 0.08; cmd "note 16 0"; cmd "cc 43 0"; sleep 0.6
wait_for 3 '[ "$(epf wlayer)" = 0 ] && [ "$(epf wpage)" = 44 ]' && [ "$(ctext 0)" = "$cf" ] \
    && ok "track 1 again: back on its FILTER page (remembered), the chord's Cutoff as left ($cf: the bass's turn did not touch it)" \
    || bad "track 1: wlayer '$(epf wlayer)' wpage '$(epf wpage)' Cutoff '$(ctext 0)' (was $cf)"
sleep 1.2
build/tools/wsprobe $PORT 1600 --op "3:$GLO:1@200" --op "4:$PRE:$((0 - PRED))@450" --op "3:$GLO:0@900" >/dev/null
wait_for 4 '[ "$(layers)" = "[[1,0],[2,1]]" ]' && wait_for 3 '[ "$(epf wpage)" = 25 ]' \
    && ok "the chord's engine switched on the FM-1 while ENGINE shows: FM TONE's FILTER (page 25, by name) watched (\"$(cname 0)\")" \
    || bad "the engine switch: layers '$(layers)' wpage '$(epf wpage)'"
sleep 0.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/ep1_engine_changed.pbm
sleep 1.5
# ---- a few notes (the replay's), then quit
cmd "note 68 100"; sleep 0.4; cmd "note 68 0"; sleep 0.4
late=$(field late)
echo "  info  ep1: $(ep)"
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
[ "$late" = 0 ] && ok "no late blocks" || bad "$late late blocks"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
# ---- the replay: the host's input log of slot 1, FWI_EP (13) and FWI_EP_WATCH (14) among it
log=$CAP/inputs-slot1.log; raw=$CAP/raw-slot1.raw
[ -s "$log" ] || log=$CAP/inputs.log
[ -s "$raw" ] || raw=$CAP/raw.raw
nep=$(od -An -tu1 -w12 -v "$log" | awk '$5 == 13' | wc -l); nw=$(od -An -tu1 -w12 -v "$log" | awk '$5 == 14' | wc -l)
[ "$nep" -ge 3 ] && [ "$nw" -gt 2 ] && ok "the input log holds the SETs ($nep FWI_EP) and the watches ($nw FWI_EP_WATCH)" \
    || bad "the input log: $nep FWI_EP, $nw FWI_EP_WATCH"
N=$(( $(stat -c %s "$raw") / 4 ))
RFM14_CPU2=0 build/tools/fwrun "$CR" --replay "$log" --frames $N --pcm "$T/replay.raw" >"$T/replay.txt" 2>&1
cmp -s "$T/replay.raw" "$raw" \
    && ok "ChoralRoot in the host = replayed offline from its input log with the SETs, bit for bit ($N frames)" \
    || bad "the replay differs ($(cmp "$T/replay.raw" "$raw" 2>&1 | head -1))"
echo "ep1_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -30 "$T/host.log"; }
mv "$T" "$T.done" 2>/dev/null
exit $fail
