#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The States and a project's firmwares hold the firmware as it is (README "The working memory", memsnap.h), on the
# simulator in the build image after make, the firmware PLAYED through the Move's controls (not its memory poked):
# ChoralRoot 0.43 in slot 1 with core 1 on (as on the Move), its own takeover (Track 1 held + pad 2), its voicing turned
# with knob 1 and read back without a change: knob 1 touched, the takeover's popup says it (the screen reader, a fake
# TTS that logs what it is asked to say). ChoralRoot writes its flash only once it has been quiet a while, so a state
# saved right after a change held its old sound before (the owner's "always loads the same"):
#   - voicing +3, state 1 saved at once; +7, state 2 saved: their flash images are the same bytes, their working
#     memories (state-NN.mem) are not; state 1 loaded: +3, state 2: +7 (restored, not booted: its log, the status);
#   - New: 0 (a fresh memory); Undo: +7 back; Copy + state 1, then the empty state 4: pasted with its working memory,
#     loaded: +3;
#   - a state as an older build saved it (state-NN.bin and .json only): loads from its flash (said in the log); a .mem
#     whose image is not the .bin beside it: the flash only too;
#   - Felucca 1.8.2 (single core) in slot 2: its PRESETS knob turned, states 1 and 2 saved, state 1 loaded: its screen
#     as it was (the LCD's FNV-1a, a few seconds on); its working memory restored;
#   - no late block while the states are saved and loaded (the status's "late"), the snapshots' cost said;
#   - the active state written again while the firmware is played (RFM14_TEST_MEM_MS: 4 s here, 30 s on the Move) and
#     not while it is left alone;
#   - projects: project 1 stored with its copies (.fwK.mem too); an empty pad: project 2, ChoralRoot loaded there and
#     turned to +5; project 1 again: ChoralRoot at +3 (the copy's working memory); project 2: +5;
#   - quit and start again: the open project's ChoralRoot as it was left.
# First, offline (tools/fwrun --snap-test): each firmware in build/fw (and the core-1 fixture, the takeover's, every
# published one in dist/move-catalog when it is there) snapshotted mid-chord, the file written and read back, restored
# into a second instance: the next 1102 blocks of both the same bit for bit; a Fantasy CFW refused (flash only).
#   tests/states_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "states_e2e"
T=$(mktemp -d /tmp/rfm14st.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; CFG=$T/cfg; SAID=$T/said.log; mkdir -p "$SIM" "$R" "$CFG"
echo "http://127.0.0.1:1" > "$R/catalog-url"
echo '{ "debounce_ms": 0 }' > "$CFG/tts.json"
PORT=17828; U="http://127.0.0.1:$PORT"
P=$R/move/projects; CRS=$R/states/choralroot; FES=$R/states/felucca
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotrec() { status | sed 's/.*"slots":\[//; s/\],"threads".*//; s/null/{"id":"-"}/g' | awk -v RS='[{]"id":' -v n="$1" 'NR == n + 2 { print "\"id\":" $0 }'; }
sfield() { slotrec "$1" | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
alive() { kill -0 "$HOST" 2>/dev/null; }
cmd() { alive || return 1; printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":${3:-0}}" "$U/api/load"; }
lcd() { curl -s -X POST --data "{\"slot\":$1,\"lcd\":1}" "$U/api/test/peek" | sed -n 's/.*"sum":"\([0-9a-f]*\)".*/\1/p'; }
jmem() { sed -n 's/.*"mem":"\([0-9a-f]*\)".*/\1/p' "$1" 2>/dev/null; }   # a .json's working memory hash
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
M=0
# ---- offline: a snapshot restored renders what the original goes on to render
for f in build/fw/*.rfm1m build/cpu2fix/cpu2fix-1.0.rfm1m build/tkdemo/tkdemo-1.0.rfm1m; do
    o=$(build/tools/fwrun "$f" --snap-test 6000 2>/dev/null); r=$?
    l=$(echo "$o" | grep "snapshot at" | sed 's/^ *snapshot at ms [0-9]*: //')
    [ $r = 0 ] && echo "$o" | grep -q "bit for bit" && ok "$(basename "$f" .rfm1m): restored from a snapshot mid-chord, the same 1102 blocks as the original ($l)" \
        || bad "$(basename "$f"): $(echo "$o" | grep snapshot | tail -1)"
done
if ls dist/move-catalog/*.rfm1m >/dev/null 2>&1; then
    n=0; same=0; flash=0; worst=""
    for f in dist/move-catalog/*.rfm1m; do
        o=$(timeout 120 build/tools/fwrun "$f" --snap-test 6000 2>/dev/null); n=$((n + 1))
        if echo "$o" | grep -q "bit for bit"; then same=$((same + 1))
        elif echo "$o" | grep -q "not known to be whole" && grep -q '"fs1"' "${f%.rfm1m}.json"; then flash=$((flash + 1))
        else worst="$worst $(basename "$f" .rfm1m)"; fi
    done
    [ -z "$worst" ] && ok "dist/move-catalog: $same of $n firmwares restored bit for bit, $flash Fantasy CFWs flash only" \
        || bad "dist/move-catalog: not restored:$worst"
fi
# the takeover (Track 1 held + pad 2) and FM-1 mode (Track 1 held + pad 1)
tk() { [ "$(field move_mode)" = 4 ] && return 0; cmd "cc 43 127"; cmd "note 93 100"; cmd "note 93 0"; cmd "cc 43 0"; wait_for 3 '[ "$(field move_mode)" = 4 ]'; sleep 1.6; }
# ChoralRoot's voicing, read without changing it: knob 1 touched in its takeover, the popup's words ("Voicing plus 3")
vo() {
    tk; mark; cmd "note 0 127"
    wait_for 3 'said "^Voicing"'; cmd "note 0 0"
    tail -n +$((M + 1)) "$SAID" | grep "^Voicing" | tail -1 | sed 's/^Voicing //; s/^plus /+/; s/^minus /-/'
}
turn() { tk; _d=$1; while [ "$_d" -gt 0 ]; do cmd "cc 71 1"; _d=$((_d - 1)); sleep 0.1; done; sleep 0.4; }
# a slot's States (Track n held + pad 7), a pad there (save an empty one, load a stored one), back
states() {
    _st=$(($1 - 1))
    [ "$(field states_open)" = $_st ] && return 0
    [ "$(field states_open)" = -1 ] || back
    cmd "cc $((44 - $1)) 127"; sleep 0.3; hit 98; cmd "cc $((44 - $1)) 0"; wait_for 3 '[ "$(field states_open)" = $_st ]'; sleep 0.6
}
back() { _k=0; while [ "$(field states_open)" != -1 ] && [ $_k -lt 3 ]; do press 51; sleep 0.4; _k=$((_k + 1)); done; }
stload() { _f=boot$(($1 - 1)); b0=$(field $_f); states "$1"; hit "$2"; wait_for 6 '[ "$(field $_f)" -gt "$b0" ]'; r=$?; back; return $r; }
stsave() { _b=$3; _m=${3%.bin}.mem; states "$1"; hit "$2"; wait_for 6 '[ -s "$_b" ] && [ -s "$_m" ]'; r=$?; back; return $r; }
projects() { [ "$(field projects)" = 1 ] || { cmd "cc 40 127"; sleep 0.3; hit 16; cmd "cc 40 0"; wait_for 3 '[ "$(field projects)" = 1 ]'; }; }
start() {
    FAKE_TTS_SHM=/rfm14-st-$$ FAKE_TTS_LOG=$SAID RFM14_TEST_MEM_MS=4000 build/sim/remotefm1-host --root "$R" --port $PORT \
        --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw choralroot --test-api --no-fm1 --screen-reader on \
        --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-st-$$ --tts-config "$CFG" >>"$T/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"'
}
stop() { printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST; }
start || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
wait_for 10 '[ "$(sfield 0 c1)" = 1 ]' && [ "$(field snapable0)" = 1 ] \
    && ok "ChoralRoot 0.43 in slot 1, core 1 on (as on the Move); its snapshots known to be whole (src/snapsafe.h)" \
    || bad "slot 1: core 1 '$(sfield 0 c1)', snapable '$(field snapable0)'"
sleep 2
late0=$(field late)

# ---- ChoralRoot: voicing +3, state 1 at once; +7, state 2; state 1 loaded, then state 2
v=$(vo); [ "$v" = 0 ] && ok "the takeover, knob 1 touched: \"Voicing $v\" (a fresh memory)" || bad "the voicing at start: '$v'"
turn 3; v=$(vo); [ "$v" = +3 ] || bad "knob 1 turned 3 detents: '$v'"
stsave 1 92 "$CRS/state-00.bin" && [ -n "$(jmem "$CRS/state-00.json")" ] \
    && ok "voicing $v, state 1 saved at once: state-00.bin, state-00.mem ($(stat -c %s "$CRS/state-00.mem") bytes), its .json names it" \
    || bad "state 1: $(ls "$CRS")"
turn 4; v=$(vo); [ "$v" = +7 ] || bad "4 more detents: '$v'"
stsave 1 93 "$CRS/state-01.bin" || bad "state 2: $(ls "$CRS")"
cmp -s "$CRS/state-00.bin" "$CRS/state-01.bin" && ! cmp -s "$CRS/state-00.mem" "$CRS/state-01.mem" \
    && ok "voicing $v, state 2 saved: ChoralRoot had written nothing to its flash (the two images the same bytes, as on the owner's Move); their working memories differ" \
    || bad "the two states' files: images $(cmp -s "$CRS/state-00.bin" "$CRS/state-01.bin" && echo same || echo differ)"
stload 1 92 || bad "state 1 did not load"
v=$(vo); [ "$v" = +3 ] && [ "$(field restored0)" = 1 ] && grep -q "slot 1: state 1: its working memory restored" "$T/host.log" \
    && ok "state 1 loaded: voicing $v (its working memory restored, not booted from its flash)" \
    || bad "state 1 loaded: voicing '$v' (want +3), restored '$(field restored0)'"
stload 1 93 || bad "state 2 did not load"
v=$(vo); [ "$v" = +7 ] && ok "state 2 loaded: voicing $v" || bad "state 2 loaded: voicing '$v' (want +7)"

# ---- New, Undo (the States open: New's undo lasts while they are); New again, left; Copy + state 1, the empty state
# 4: pasted, loaded
b0=$(field boot0); states 1; hit 29; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 1 ]' || bad "New"
b0=$(field boot0); press 56; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 0 ]' || bad "Undo"; back
v=$(vo); [ "$v" = +7 ] && [ "$(field restored0)" = 1 ] && ok "step 14 New, then Undo: voicing $v again (the working memory New kept)" \
    || bad "New + Undo: voicing '$v' (want +7)"
b0=$(field boot0); states 1; hit 29; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 1 ]'; back
v=$(vo); [ "$v" = 0 ] && [ "$(field restored0)" = 0 ] && ok "step 14 New: voicing $v (a fresh memory, booted)" || bad "New: voicing '$v'"
stload 1 93 || bad "state 2 after New"
states 1; cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.3; hit 95; sleep 0.2; cmd "cc 60 0"
wait_for 5 '[ -s "$CRS/state-03.mem" ]' && cmp -s "$CRS/state-00.mem" "$CRS/state-03.mem" \
    && [ "$(jmem "$CRS/state-03.json")" = "$(jmem "$CRS/state-00.json")" ] && ok "Copy + state 1, then the empty state 4: pasted with its working memory" \
    || bad "the paste: $(ls "$CRS")"
back
stload 1 95 || bad "state 4 did not load"
v=$(vo); [ "$v" = +3 ] && ok "state 4 (the paste) loaded: voicing $v" || bad "state 4: voicing '$v' (want +3)"

# ---- a state as an older build saved it (no .mem): its flash; a .mem that goes with another image: its flash too
cp "$CRS/state-00.bin" "$CRS/state-05.bin"
sed 's/,"mem":"[0-9a-f]*"//' "$CRS/state-00.json" > "$CRS/state-05.json"
cp "$CRS/state-00.bin" "$CRS/state-06.bin"; cp "$CRS/state-00.mem" "$CRS/state-06.mem"; cp "$CRS/state-00.json" "$CRS/state-06.json"
printf 'X' | dd of="$CRS/state-06.bin" bs=1 seek=$(( $(stat -c %s "$CRS/state-06.bin") - 9 )) conv=notrunc 2>/dev/null
states 1; cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.2; cmd "cc 60 0"; sleep 0.5; back   # (a Copy: the States see the new files)
stload 1 97 || bad "state 6 (an older build's) did not load"
v=$(vo); [ "$(field restored0)" = 0 ] && grep -q "slot 1: state 6: its flash only: no working memory saved with it" "$T/host.log" \
    && ok "a state as an older build saved it (no .mem): loaded from its flash (voicing $v: the flash never had the turns), said in the log" \
    || bad "the older build's state: restored '$(field restored0)', voicing '$v'"
stload 1 98 || bad "state 7 did not load"
[ "$(field restored0)" = 0 ] && grep -q "slot 1: state 7: its flash only: its working memory goes with another flash image" "$T/host.log" \
    && ok "a .mem beside an image it was not taken with: not used (the flash only)" || bad "state 7: restored '$(field restored0)'"
stload 1 93 || bad "state 2 again"

# ---- Felucca, single core, in slot 2: PRESETS turned (Move knob 2), states 1 and 2, state 1 back: its screen
load 1 felucca 1; wait_for 15 '[ "$(field id1)" = felucca ] && [ "$(field active)" = 1 ]' || bad "Felucca into slot 2"
sleep 2
cmd "cc 43 127"; cmd "note 92 100"; cmd "note 92 0"; cmd "cc 43 0"; sleep 0.5   # (FM-1 mode)
fpre() { cmd "note 1 127"; _d=$1; while [ "$_d" -gt 0 ]; do cmd "cc 72 1" "cc 72 1"; _d=$((_d - 1)); sleep 0.1; done; cmd "note 1 0"; sleep 2.5; }
# (Felucca writes its flash when its sound changes: the active state follows that (rule (c), as before); so New in
# between, no state active, before the second sound)
fpre 2; la=$(lcd 1)
stsave 2 92 "$FES/state-00.bin" || bad "Felucca's state 1"
b0=$(field boot1); states 2; hit 29; wait_for 5 '[ "$(field boot1)" -gt "$b0" ]'; back; sleep 2
fpre 3; lb=$(lcd 1)
stsave 2 93 "$FES/state-01.bin" || bad "Felucca's state 2"
[ -n "$la" ] && [ "$la" != "$lb" ] || bad "Felucca's screen did not change with PRESETS ($la, $lb)"
stload 2 92 || bad "Felucca's state 1 did not load"
sleep 2.5; l1=$(lcd 1)
[ "$l1" = "$la" ] && [ "$(field restored1)" = 1 ] \
    && ok "Felucca 1.8.2 (one core): PRESETS turned, state 1 saved; New, PRESETS, state 2; state 1 loaded: restored, its screen as it was ($la), not state 2's ($lb)" \
    || bad "Felucca's state 1: screen $l1 (want $la, state 2's $lb), restored '$(field restored1)'"
stload 2 93 || bad "Felucca's state 2 did not load"
sleep 2.5; l2=$(lcd 1)
[ "$l2" = "$lb" ] && ok "Felucca's state 2 loaded: its screen ($lb)" || bad "Felucca's state 2: screen $l2 (want $lb)"
late1=$(field late)
[ "$late1" = "$late0" ] && ok "no late block while the states were saved and loaded (late $late0 -> $late1; costliest snapshot $(field snap_max_us) us, restore $(field restore_max_us) us)" \
    || bad "late blocks: $late0 -> $late1 (snapshots up to $(field snap_max_us) us)"

# ---- the active state written again while the firmware is played (every 4 s here), not while it is left alone
cmd "cc 43 127"; cmd "note 92 100"; cmd "note 92 0"; cmd "cc 43 0"; sleep 0.3
load 0 choralroot 1 >/dev/null; wait_for 5 '[ "$(field active)" = 0 ]'
sleep 5   # (a flash write of its own since the load: written then, rule (c); then nothing)
h0=$(jmem "$CRS/state-01.json"); n0=$(field snaps0)
sleep 9
[ "$(field snaps0)" = "$n0" ] && [ "$(jmem "$CRS/state-01.json")" = "$h0" ] && ok "left alone 9 s: the active state not written again ($n0 snapshots)" \
    || bad "left alone: snapshots $n0 -> $(field snaps0)"
turn 2
wait_for 8 '[ "$(jmem "$CRS/state-01.json")" != "$h0" ]' && ok "played (voicing +2): the active state 2 written again with its working memory, no save asked ($(field snaps0) snapshots)" \
    || bad "the idle autosave: state 2's mem hash still $h0"
v=$(vo); [ "$v" = +9 ] || bad "voicing after 2 more detents: '$v'"

# ---- projects: project 1 stored with its copies; project 2 (an empty pad), ChoralRoot there at +5; back and forth
cmd "cc 43 127"; cmd "note 92 100"; cmd "note 92 0"; cmd "cc 43 0"; sleep 0.3
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"; sleep 1.3; hit 16; sleep 0.3; press 51; sleep 0.3
wait_for 10 '[ -s "$P/move-project-01.rfmmove" ] && [ -s "$P/move-project-01.fw1.bin" ]' || bad "project 1 not stored: $(ls "$P")"
projects; hit 93
wait_for 8 '[ "$(field proj_open)" = 1 ] && [ -z "$(field id0)" ]' && [ -s "$P/move-project-01.fw1.mem" ] \
    && [ -n "$(jmem "$P/move-project-01.fw1.json")" ] && ok "an empty pad: project 2; project 1's copies written as it was left, with their working memory (.fw1.mem)" \
    || bad "project 2: proj_open '$(field proj_open)', $(ls "$P")"
press 51; sleep 0.3
load 0 choralroot 1; wait_for 15 '[ "$(field id0)" = choralroot ]' || bad "ChoralRoot into project 2"
sleep 2
v=$(vo); turn 5; v2=$(vo)
[ "$v2" = "+$(( ${v#+} + 5 ))" ] || bad "project 2's ChoralRoot: $v, then '$v2'"
projects; hit 92
wait_for 10 '[ "$(field proj_open)" = 0 ] && [ "$(field id0)" = choralroot ]' || bad "project 1 again: '$(field id0)'"
press 51; sleep 0.5
v=$(vo); [ "$v" = +9 ] && [ "$(field restored0)" = 1 ] && grep -q "project 1: slot 1: choralroot with the project's memory (its working memory restored)" "$T/host.log" \
    && ok "project 1 again: ChoralRoot at voicing $v, as it was left (the copy's working memory)" \
    || bad "project 1's ChoralRoot: voicing '$v' (want +9), restored '$(field restored0)'"
projects; hit 93
wait_for 10 '[ "$(field proj_open)" = 1 ] && [ "$(field id0)" = choralroot ]' || bad "project 2 again"
press 51; sleep 0.5
v=$(vo); [ "$v" = "$v2" ] && ok "project 2 again: voicing $v (its own copy)" || bad "project 2's ChoralRoot: '$v' (want $v2)"

# ---- quit and start again: the open project's ChoralRoot as it was left
turn 1; v=$(vo)
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
start || { bad "the host did not start again"; tail -20 "$T/host.log"; exit 1; }
wait_for 15 '[ "$(field id0)" = choralroot ] && [ "$(field restored0)" = 1 ]'; sleep 1.5
v3=$(vo); [ "$v3" = "$v" ] && ok "quit and started again: project 2's ChoralRoot at voicing $v3, as it was left" \
    || bad "after a restart: voicing '$v3' (want $v), restored '$(field restored0)'"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
rm -f "/dev/shm/rfm14-st-$$"
echo "  (snapshot sizes: ChoralRoot $(stat -c %s "$CRS/state-00.mem") bytes, Felucca $(stat -c %s "$FES/state-00.mem") bytes)"
mv "$T" "$T.done" 2>/dev/null
echo "states_e2e: $checks checks, $fail failed"
exit $fail
