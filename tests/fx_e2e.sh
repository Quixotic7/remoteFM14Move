#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The firmware slots, the Schwung FX and the Mix & FX back end on the simulator, end to end (in the build image, after
# make): the test modules in build/fxmods (Schwung's arp, chord and freeverb; crashfx; hogfx) as the installed Schwung
# modules, the page's ops through POST /api/edit (the same path as the WebSocket's), the Move's own controls through the
# simulator's FIFO, sequencer MIDI through --test-api.
#   tests/fx_e2e.sh
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14fx.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17740
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
mix() { curl -s "$U/api/mix"; }
settle() { sleep 1.3; }   # (the status is made once a second)
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.12; }
tap() { press "$1"; sleep 0.25; }   # a Track tap (rfm1cs 0.8.0: pressed again within 300 ms it would be a double click)
edit() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"op\":$1,\"a\":$2,\"b\":$3}" "$U/api/edit"; }
midi() { curl -s -o /dev/null -X POST --data "{\"track\":$1,\"b0\":$2,\"b1\":$3,\"b2\":$4}" "$U/api/test/midi"; }
load() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":$3}" "$U/api/load"; }
wait_for() {
    _n=$(( $1 * 10 )); shift
    while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1
}
start() {
    build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
        --fx-dir build/fxmods --test-api "$@" >>"$T/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' && sleep 1
}
stop() { cmd quit; wait $HOST; RC=$?; }
# the projects view on the Move (rfm1cs 0.8.0: Track 1 double click, the firmware browser on slot 1), and its FX setup
# (Session / Menu)
view() { press "cc 43"; press "cc 43"; sleep 0.3; }
# the mix JSON without what moves by itself (CPU, counters, versions)
mixstate() { mix | sed -E 's/,"fm1":\{[^}]*\}//' | sed -E 's/"(ver|store_ver|cpu|run|busy|notes|err|sounding|active|fxopen|page|focus|fxpages|mixpar|pg)":("[^"]*"|[0-9.]+),?//g'; }
# an audio FX slot's part of the mix JSON (0..2), its values (comma-separated), one of them (slot, index), its page
afx() { mix | sed -E 's/.*"afx":\[//' | awk -v s="$1" 'BEGIN { RS = "\"mod\":" } NR == s + 2 { print }'; }
afxvals() { afx "$1" | sed -E 's/.*"vals":\[([^]]*)\].*/\1/'; }
afxval() { afxvals "$1" | cut -d, -f$(($2 + 1)); }
afxpg() { afx "$1" | sed -E 's/.*"pg":([0-9]+).*/\1/'; }
mixf() { mix | sed -E "s/.*\"$1\":([0-9]+).*/\\1/"; }

echo "fx_e2e"
start
[ "$(field fx_mods)" = 7 ] && curl -s "$U/api/fx" | grep -q '"id":"arp"' && curl -s "$U/api/fx" | grep -q '"key":"room_size"' \
    && ok "the installed modules listed: $(curl -s "$U/api/fx" | grep -o '"name":"[^"]*","abbrev' | sed 's/"name":"\([^"]*\)".*/\1/' | tr '\n' ' ')" \
    || bad "/api/fx: $(curl -s "$U/api/fx" | head -c 300)"

# ---- slots from the page: a load into slot 2 that makes it the active one, a switch back, an unload
[ "$(load 1 choralroot 1)" = 202 ] && wait_for 10 '[ "$(field id1)" = choralroot ] && [ "$(field active)" = 1 ]' \
    && [ "$(field core_active)" = 1 ] && ok "the page's chip: ChoralRoot into slot 2, then active (the host and the core: slot $(field core_active))" \
    || bad "load into slot 2: id1 '$(field id1)', active $(field active), core $(field core_active)"
[ "$(edit 16 0 0)" = 202 ] && wait_for 3 '[ "$(field active)" = 0 ] && [ "$(field core_active)" = 0 ]' \
    && ok "the page's chip: slot 1 active again (FW_ACTIVE into the core)" || bad "switch: active $(field active), core $(field core_active)"
[ "$(load 2 felucca 0)" = 202 ] && wait_for 10 '[ "$(field id2)" = felucca ]' && [ "$(field active)" = 0 ] \
    && ok "Felucca into slot 3, not made active" || bad "slot 3: '$(field id2)', active $(field active)"
edit 17 2 0 >/dev/null
wait_for 5 '[ -z "$(field id2)" ]' && [ "$(mix | grep -o '{"on":[01]' | tr -d '{"on:' | tr -d '\n')" = 110 ] \
    && ok "the page's unload: slot 3 empty (the core told: the mix shows it empty)" || bad "unload: id2 '$(field id2)'"
# the Move switches by itself (rfm1cs 0.8.0: a Track 2 tap plays slot 2): the page's state follows
tap "cc 42"
wait_for 3 '[ "$(field active)" = 1 ] && mix | grep -q "\"active\":1"' && ok "the Move's Track 2: slot 2 active, the mix JSON follows" \
    || bad "the Move's switch: active $(field active)"
edit 16 0 0 >/dev/null; sleep 0.3

# ---- MIDI FX on the sequencer's MIDI
# (rfm1cs 0.13.0: a new project routes tracks 1-4 / 5-8 / 9-12 to firmwares 1 / 2 / 3. This test's premise is every
# firmware hearing every track, the older default: set it)
edit 19 0 65535 >/dev/null; edit 19 1 65535 >/dev/null; edit 19 2 65535 >/dev/null; settle
settle; n0=$(field notes0); midi 1 144 60 100; settle; n1=$(field notes0); midi 1 128 60 0
[ $((n1 - n0)) = 1 ] && ok "no MIDI FX: a sequencer note on track 1 reaches slot 1 once ($n0 -> $n1)" || bad "plain: $n0 -> $n1"
edit 20 0 0 >/dev/null; edit 19 3 1 >/dev/null   # arp into MIDI FX 1, acting on track 1
wait_for 5 '[ "$(field mfx0)" = arp ]' && ok "the page's FX_LOAD: Arpeggiator in MIDI FX 1 (the storage thread opened it)" || bad "mfx0 '$(field mfx0)'"
settle; n0=$(field notes0); midi 1 144 60 100; midi 1 144 64 100; midi 1 144 67 100; sleep 1.5; settle; n1=$(field notes0)
midi 1 128 60 0; midi 1 128 64 0; midi 1 128 67 0; settle
[ $((n1 - n0)) -ge 6 ] && ok "the arp on track 1: a held chord becomes $((n1 - n0)) notes at slot 1 in 2.8 s (3 without it)" \
    || bad "arp: $n0 -> $n1"
settle; n0=$(field notes0); midi 2 144 62 100; settle; n1=$(field notes0); midi 2 128 62 0
[ $((n1 - n0)) = 1 ] && ok "track 2 is not in the arp's filter: it passes straight through (1 note)" || bad "track 2: $n0 -> $n1"
# the pads played live (track 0) on channel 1: through the arp too, to the active slot
settle; n0=$(field notes0); midi 0 144 60 100; midi 0 144 64 100; midi 0 144 67 100; sleep 1.5; settle; n1=$(field notes0)
midi 0 128 60 0; midi 0 128 64 0; midi 0 128 67 0; settle
[ $((n1 - n0)) -ge 6 ] && ok "live pads on channel 1 go through the arp too: a held chord becomes $((n1 - n0)) notes at the active slot" \
    || bad "live through the arp: $n0 -> $n1"
settle; n0=$(field notes0); midi 0 145 62 100; settle; n1=$(field notes0); midi 0 129 62 0
[ $((n1 - n0)) = 1 ] && ok "live on channel 2 (track 2, not the arp's): straight to the active slot (1 note)" || bad "live ch 2: $n0 -> $n1"
# ---- a firmware's track filter: slot 2 without track 5
edit 19 1 65519 >/dev/null; settle
a0=$(field notes0); b0=$(field notes1); midi 5 144 72 100; settle; a1=$(field notes0); b1=$(field notes1); midi 5 128 72 0
[ $((a1 - a0)) = 1 ] && [ $((b1 - b0)) = 0 ] && ok "slot 2's filter without track 5: track 5 reaches slot 1 only" \
    || bad "filter: slot 1 $a0 -> $a1, slot 2 $b0 -> $b1"
mix | grep -q '"filt":65519' && ok "ROUTE in the core's state (the mix JSON: slot 2's filter 65519)" || bad "no filter in the mix"

# ---- audio FX on a send bus
edit 20 4 2 >/dev/null
wait_for 5 '[ "$(field afx0)" = freeverb ]' || bad "freeverb did not load: '$(field afx0)'"
cmd "note 84 110" "note 88 110" "note 91 110"; sleep 2.2
r0=$(field rpeak0); p0=$(field peak)
edit 18 2 100 >/dev/null; sleep 2.2
r1=$(field rpeak0); p1=$(field peak)
cmd "note 84 0" "note 88 0" "note 91 0"
[ "$r0" = 0 ] && [ "$r1" -gt 500 ] && ok "freeverb on audio FX 1: send 0, its return silent; send 100 from slot 1, its return $r1 (mix peak $p0 -> $p1)" \
    || bad "send: return $r0 -> $r1"
mix | grep -q '"sends":\[100,0,0\]' && ok "MIX in the core's state (slot 1's sends 100, 0, 0)" || bad "sends not in the mix"

# ---- refusals: a module that crashes as it loads; one past the CPU budget
edit 20 5 1 >/dev/null
wait_for 5 'mix | grep -q "crashed as it loaded"' && [ -n "$(field blocks)" ] && [ "$(field afx1)" = "" ] \
    && ok "Crash Test (a segfault in create_instance): refused, the host keeps running" || bad "crashfx: $(mix | head -c 200)"
edit 20 6 0 >/dev/null
wait_for 3 '[ "$(field afx2)" = hogfx ]' && wait_for 6 '[ "$(field afx2)" = "" ] && mix | grep -q "Too busy"' \
    && ok "CPU Hog: past the budget, refused (\"Too busy\"; the core empties its slot: $(mix | grep -o '"afx":.*' | grep -c '"mod":-1'))" \
    || bad "hogfx: afx2 '$(field afx2)', $(mix | grep -o 'Too busy[^"]*')"

# ---- the page and the Move: the same state, both ways (the Move's FX setup open on its screen)
view; press "cc 50"
wait_for 3 '[ "$(field fx_open)" = 1 ]' && ok "the Move's FX setup open (Session in the projects view): the mix JSON says so" \
    || bad "fx_open $(field fx_open)"
edit 22 3 0 >/dev/null
wait_for 3 '[ "$(field fx_page)" = 3 ]' && ok "the page's tab: the Move's FX setup on the Mixer page (fx_page 3)" || bad "page: $(field fx_page)"
tap "cc 40"
wait_for 3 '[ "$(field fx_page)" = 1 ] && mix | grep -q "\"page\":1"' && ok "the Move's Track 4 tap on the Mixer: Routing, and the page follows (page 1)" \
    || bad "the Move's page: $(field fx_page)"
tap "cc 40"; tap "cc 40"   # (Track 4 again: the Kit (stage 3a: the host has the kit, Track 4's third page), then the Mixer)
edit 18 0 50 >/dev/null
wait_for 3 '[ "$(field level0)" = 50 ]' || bad "the page's level: $(field level0)"
cmd "cc 71 3"; sleep 0.4
wait_for 3 '[ "$(field level0)" = 59 ]' && mix | grep -q '"level":59' \
    && ok "the page set slot 1's level to 50 in the core; the Move's knob 1 then made it 59 (3 detents of 0.5 dB, rfm1cs 2026-11-26), and the page sees 59" \
    || bad "level: $(field level0)"
press "note 89"; cmd "cc 71 5"; sleep 0.4   # (0.5.0 Mixer: the right 4 x 4's Send 2, then knob 1 for slot 1)
wait_for 3 'mix | grep -q "\"sends\":\[100,5,0\]"' && ok "the Move's Mixer: Send 2 picked (its pad), knob 1 sets slot 1's: the page's sends follow (100, 5, 0)" \
    || bad "the Move's send: $(mix | grep -o '"sends":\[[0-9,]*\]' | head -1)"
tap "cc 43"   # (a Track 1 tap: out of the view, slot 1 played)

# ---- 3 slots, 3 MIDI FX, 3 audio FX, all playing: no late block
load 2 choralroot 0 >/dev/null
wait_for 10 '[ "$(field id2)" = choralroot ]' || bad "slot 3: '$(field id2)'"
edit 19 1 65535 >/dev/null; edit 19 3 65535 >/dev/null
edit 20 1 1 >/dev/null; edit 20 2 0 >/dev/null; edit 20 5 2 >/dev/null; edit 20 6 2 >/dev/null
for s in 0 1 2; do for k in 2 3 4; do edit 18 $((s * 8 + k)) 70 >/dev/null; done; done
wait_for 8 '[ "$(field mfx1)" = chord ] && [ "$(field mfx2)" = arp ] && [ "$(field afx1)" = freeverb ] && [ "$(field afx2)" = freeverb ]' \
    || bad "FX: $(field mfx0) $(field mfx1) $(field mfx2) / $(field afx0) $(field afx1) $(field afx2)"
late0=$(field late)
cmd "note 84 110" "note 88 110" "note 91 110"
for t in 1 2 3 4 5 6; do midi $t 144 $((48 + t * 3)) 100; done
sleep 6
for t in 1 2 3 4 5 6; do midi $t 128 $((48 + t * 3)) 0; done
cmd "note 84 0" "note 88 0" "note 91 0"
late1=$(field late)
echo "  info  CPU: $(status | grep -o '"slots":\[[^]]*\]' | sed 's/"worst_us[^}]*//g'), FX $(field fx_cpu) %"
[ "$late0" = "$late1" ] && [ "$(field notes0)" -gt 0 ] && [ "$(field notes2)" -gt 0 ] \
    && ok "3 firmwares, 3 MIDI FX, 3 audio FX playing for 6 s: no late block ($late1 so far; FX $(field fx_cpu) % of a core)" \
    || bad "late blocks: $late0 -> $late1"

# ---- (rfm1cs 0.5.0) a module's own parameter pages: Page Test (tests/fxmods/audio_fx/pagesfx, a multi-level
# ui_hierarchy) chosen on the Move (audio FX 3's pad held + the jog, let go), its pages (the jog's click, the jog),
# a knob on page 2; a parameter past index 8 from the page (op 23), reaching the module
curl -s "$U/api/fx" | grep -o '"id":"pagesfx".*' | grep -q '"pages":\[{"name":"Pages","knobs":\[0,1,-1,2,3,4,5,6\]},{"name":"Pages 2".*"name":"Advanced".*"name":"Sub".*"name":"Deep".*"name":"Deep 2"' \
    && ok "/api/fx: Page Test's 6 pages (root with its gap, its overflow page, Advanced, Sub, Deep, Deep 2)" \
    || bad "pagesfx pages: $(curl -s "$U/api/fx" | grep -o '"id":"pagesfx".*' | grep -o '"pages":.*' | head -c 300)"
cmd "cc 40 127"; press "note 93"; cmd "cc 40 0"; sleep 0.2   # (Track 4 held + pad 2: Routing; audio FX 3's pad below picks its column)
cmd "note 80 127"; sleep 0.3; cmd "cc 14 1"; sleep 0.3
[ "$(field afx2)" = freeverb ] && ok "audio FX 3's pad held + the jog: the list shows the next module, nothing loaded yet" \
    || bad "held + jog loaded already: afx2 '$(field afx2)'"
cmd "note 80 0"
wait_for 5 '[ "$(field afx2)" = pagesfx ]' && ok "the pad let go: Page Test loaded into audio FX 3 (one FX_LOAD)" || bad "the release: afx2 '$(field afx2)'"
grep -q "pagesfx: set d9=9" "$T/host.log" && ok "every planned parameter set as it loaded (Deep 9, index 23: 9)" || bad "pagesfx defaults not set"
press "cc 3"
wait_for 3 '[ "$(mixf fxpages)" = 1 ]' && ok "the jog's click: its pages open (the mix says so)" || bad "pages: fxpages $(mixf fxpages)"
cmd "cc 14 1"; sleep 0.3
wait_for 3 '[ "$(afxpg 2)" = 1 ]' && ok "the jog: page 2 (Pages 2), the page sees it (pg 1)" || bad "page 2: pg $(afxpg 2)"
v0=$(afxval 2 9); cmd "cc 73 5"; sleep 0.3
wait_for 3 '[ "$(afxval 2 9)" = $((v0 + 5)) ]' && grep -q "pagesfx: set spare1=15" "$T/host.log" \
    && ok "knob 3 on page 2: Spare One (index 9) $v0 -> $(afxval 2 9), the module set (spare1=15)" || bad "knob on page 2: $v0 -> $(afxval 2 9)"
[ "$(edit 23 11 $((1 << 9 | 2 << 7 | 100)))" = 202 ] \
    && wait_for 3 '[ "$(afxval 2 11)" = 100 ] && grep -q "pagesfx: set time=1.5700" "$T/host.log"' \
    && ok "the page's op 23: Delay Time (index 11, page 3) 100 in the core, the module set (time=1.57 s, its step 0.01)" \
    || bad "op 23: vals $(afxvals 2)"
[ "$(edit 24 6 2)" = 202 ] && wait_for 3 '[ "$(afxpg 2)" = 2 ]' && ok "the page's op 24: the Move's page for audio FX 3 is Advanced (pg 2)" \
    || bad "op 24: pg $(afxpg 2)"
edit 22 $((2 | 1 << 2)) 0 >/dev/null
wait_for 3 '[ "$(mixf focus)" = 0 ] && [ "$(mixf page)" = 2 ] && [ "$(mixf fxpages)" = 0 ]' \
    && ok "the page's op 22 with a row: audio FX 1 selected on the Move (its pages closed)" || bad "focus $(mixf focus), page $(mixf page)"
press "cc 51"   # (Back: out of the view)

# ---- the FX setup and the modules' own state survive a project save and load (the autosave, then a restart)
edit 19 3 64 >/dev/null; edit 21 0 2 >/dev/null; edit 21 32 40 >/dev/null   # arp: track 7, mode down; freeverb: room 40
edit 20 5 4 >/dev/null   # Slow State (80 ms over its saved state) into audio FX 2
wait_for 4 '[ "$(field afx1)" = slowfx ]' || bad "slowfx: afx1 '$(field afx1)'"
wait_for 6 '[ -s "$ROOT/move/projects/move-project-01.fx.json" ] && grep -q Arpeggiator "$ROOT/move/projects/move-project-01.fx.json"' \
    && ok "the project autosaved, its modules' state beside it (move-project-01.fx.json: $(grep -o '"[ma][0-9]":{"name":"[^"]*"' "$ROOT/move/projects/move-project-01.fx.json" | sed 's/":{"name":"/ /;s/"//g' | tr '\n' ' '))" \
    || bad "no fx.json"
sleep 2.5
before=$(mixstate)
stop
start
wait_for 8 '[ "$(field mfx0)" = arp ] && [ "$(field mfx1)" = chord ] && [ "$(field afx2)" = pagesfx ]' || true
after=$(mixstate)
[ -n "$before" ] && [ "$before" = "$after" ] && ok "restarted: the same FX setup, mix and routing from the project (modules, values, filters, sends, levels)" \
    || { bad "the FX setup after a restart differs"; echo "    before: $before"; echo "    after:  $after"; }
[ "$(afxval 2 11)" = 100 ] && [ "$(afxval 2 9)" = 15 ] && grep -q 'time=1.5700' "$ROOT/move/projects/move-project-01.fx.json" \
    && [ "$(grep -c "pagesfx: set time=1.5700" "$T/host.log")" -ge 2 ] \
    && ok "restarted: Page Test's page-3 value kept (Delay Time 100, version 6 project), set again; its state (get_param) had it" \
    || bad "page-3 value after restart: $(afxvals 2)"
wait_for 4 '[ "$(field afx1)" = slowfx ]' && grep -q "Slow State: its saved state.*off the engine thread" "$T/host.log" \
    && ok "Slow State came back with its saved state, set off the engine thread" || bad "slowfx after restart: afx1 '$(field afx1)'"
[ "$(field mfx0)" = arp ] && [ "$(field afx0)" = freeverb ] && grep -q "Arpeggiator: its saved state" "$T/host.log" \
    && ok "the modules running again, the arp with its saved state (get_param / set_param \"state\")" \
    || bad "after restart: mfx0 '$(field mfx0)' afx0 '$(field afx0)'"
late=$(field late)
stop
[ "$RC" = 0 ] && ok "quit: the host left cleanly" || bad "quit: exit $RC"
[ "$late" = 0 ] && ok "no late blocks after the restart" || bad "$late late blocks"

echo "fx_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T/host.log"; }
mv "$T" "$T.done" 2>/dev/null
exit $fail
