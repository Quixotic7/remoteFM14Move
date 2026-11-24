#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The States and a project's firmwares hold the firmware as it is (README "The working memory", memsnap.h), on the
# simulator in the build image after make, the firmware PLAYED through the Move's controls (not its memory poked):
# ChoralRoot 0.43 in slot 1 with core 1 on (as on the Move), its own takeover (Track 1 held + pad 2), its voicing turned
# with knob 1 and read back without a change: knob 1 touched, the takeover's popup says it (the screen reader, a fake
# TTS that logs what it is asked to say). ChoralRoot writes its flash only once it has been quiet a while, so a state
# saved right after a change held its old sound before (the owner's "always loads the same"):
# (rfm1cs 0.14.0, the owner's 2026-11-24) pad 1 of States is the open project's own sound, pads 2..32 a library shared
# by all projects (pad n: state-(n-1).bin), never written by playing:
#   - voicing +3, the empty pad 2 stored at once; +7, pad 3: their flash images the same bytes, their working memories
#     (state-NN.mem) not; pad 2 loaded into the project: +3 (restored, not booted); played to +5: the library files
#     byte for byte; pad 3 loaded, Undo: +5 back;
#   - New: 0 (a fresh memory); Undo: back; Copy + pad 1 (the sound now) then the empty pad 4: stored, loaded; pad 4
#     held + Delete: reset (an erased flash), the firmware untouched; loaded: 0;
#   - a state as an older build saved it (state-NN.bin and .json only): loads from its flash (said in the log); a .mem
#     whose image is not the .bin beside it: the flash only too;
#   - Felucca 1.8.2 (single core) in slot 2: its PRESETS knob turned, pads 2 and 3 stored, pad 2 loaded: its screen
#     as it was (the LCD's FNV-1a, a few seconds on); its working memory restored;
#   - no late block while the states are saved and loaded (the status's "late"), the snapshots' cost said;
#   - the project's copy written again while the firmware is played (RFM14_TEST_MEM_MS: 4 s here, 30 s on the Move),
#     not while it is left alone, and a library state never;
#   - the owner's point, two projects holding ChoralRoot: project 1 at +9; an empty pad, project 2, ChoralRoot there
#     turned; project 1 again: +9; there library pad 2 loaded and played to +4: the library file byte for byte;
#     project 2: its own sound; project 1: +4; quit and start again: project 1 +4, project 2 its own, the library file
#     as stored; no late block.
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

# ---- ChoralRoot (rfm1cs 0.14.0, the owner's 2026-11-24: pad 1 is the open project's own sound, pads 2..32 a library
# shared by all projects; pad n is library state n - 1, "State n", state-(n-1).bin): voicing +3, the empty pad 2 at
# once; +7, pad 3; pad 2 loaded into the project, then pad 3
v=$(vo); [ "$v" = 0 ] && ok "the takeover, knob 1 touched: \"Voicing $v\" (a fresh memory)" || bad "the voicing at start: '$v'"
turn 3; v=$(vo); [ "$v" = +3 ] || bad "knob 1 turned 3 detents: '$v'"
stsave 1 93 "$CRS/state-01.bin" && [ -n "$(jmem "$CRS/state-01.json")" ] && [ ! -e "$CRS/state-00.bin" ] \
    && ok "voicing $v, the empty pad 2: stored at once as state-01.bin with state-01.mem ($(stat -c %s "$CRS/state-01.mem") bytes), its .json names it" \
    || bad "pad 2: $(ls "$CRS")"
turn 4; v=$(vo); [ "$v" = +7 ] || bad "4 more detents: '$v'"
stsave 1 94 "$CRS/state-02.bin" || bad "pad 3: $(ls "$CRS")"
cmp -s "$CRS/state-01.bin" "$CRS/state-02.bin" && ! cmp -s "$CRS/state-01.mem" "$CRS/state-02.mem" \
    && ok "voicing $v, pad 3 stored: ChoralRoot had written nothing to its flash (the two images the same bytes, as on the owner's Move); their working memories differ" \
    || bad "the two states' files: images $(cmp -s "$CRS/state-01.bin" "$CRS/state-02.bin" && echo same || echo differ)"
for e in bin mem json; do cp "$CRS/state-01.$e" "$T/lib2.$e"; done
stload 1 93 || bad "pad 2 did not load"
v=$(vo); [ "$v" = +3 ] && [ "$(field restored0)" = 1 ] && grep -q "slot 1: state 2 into the project: its working memory restored" "$T/host.log" \
    && ok "pad 2 loaded into the project: voicing $v (its working memory restored, not booted from its flash)" \
    || bad "pad 2 loaded: voicing '$v' (want +3), restored '$(field restored0)'"
turn 2; v=$(vo); [ "$v" = +5 ] || bad "played after the load: '$v'"
cmp -s "$CRS/state-01.bin" "$T/lib2.bin" && cmp -s "$CRS/state-01.mem" "$T/lib2.mem" && cmp -s "$CRS/state-01.json" "$T/lib2.json" \
    && ok "played after the load (voicing $v): library state 2's files byte for byte as they were (nothing autosaves into a state)" \
    || bad "library state 2 changed by playing"
# Undo after a library load: the sound before it
b0=$(field boot0); states 1; hit 94; wait_for 6 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 1 ]' || bad "pad 3's load"
b0=$(field boot0); press 56; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 0 ]' || bad "Undo"; back
v=$(vo); [ "$v" = +5 ] && [ "$(field restored0)" = 1 ] && ok "pad 3 loaded (+7), then Undo: voicing $v again (the sound before the load)" \
    || bad "load + Undo: voicing '$v' (want +5)"

# ---- New, Undo (the States open: the undo lasts while they are); New again, left; Copy + pad 1 (the sound now), the
# empty pad 4: stored, loaded
b0=$(field boot0); states 1; hit 29; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 1 ]' || bad "New"
b0=$(field boot0); press 56; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 0 ]' || bad "Undo"; back
v=$(vo); [ "$v" = +5 ] && [ "$(field restored0)" = 1 ] && ok "step 14 New, then Undo: voicing $v again (the working memory New kept)" \
    || bad "New + Undo: voicing '$v' (want +5)"
b0=$(field boot0); states 1; hit 29; wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 1 ]'; back
v=$(vo); [ "$v" = 0 ] && [ "$(field restored0)" = 0 ] && ok "step 14 New: voicing $v (a fresh memory, booted)" || bad "New: voicing '$v'"
turn 6; v=$(vo); [ "$v" = +6 ] || bad "6 detents after New: '$v'"
states 1; cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.3; hit 95; sleep 0.2; cmd "cc 60 0"
wait_for 5 '[ -s "$CRS/state-03.mem" ]' && [ -n "$(jmem "$CRS/state-03.json")" ] \
    && ok "Copy + pad 1 (the sound now, +6), then the empty pad 4: stored there with its working memory" \
    || bad "Copy + pad 1 then a paste: $(ls "$CRS")"
back
stload 1 93 || bad "pad 2 after the copy"
stload 1 95 || bad "pad 4 did not load"
v=$(vo); [ "$v" = +6 ] && ok "pad 4 (the copy of pad 1's sound) loaded: voicing $v" || bad "pad 4: voicing '$v' (want +6)"
# a pad held + Delete: that library state reset (as first installed); the firmware left alone
b0=$(field boot0); states 1
cmd "note 95 100"; sleep 0.45; cmd "cc 119 127"; sleep 0.1; cmd "cc 119 0"; sleep 0.1; cmd "note 95 0"
wait_for 5 '[ "$(tr -d "\377" < "$CRS/state-03.bin" | wc -c)" = 0 ]' && [ ! -e "$CRS/state-03.mem" ] && [ "$(field boot0)" = "$b0" ] \
    && ok "pad 4 held + Delete: state 4 reset (a fresh, erased flash, no working memory); the firmware not restarted" \
    || bad "hold + Delete: $(ls "$CRS"), boot $b0 -> $(field boot0)"
back
v=$(vo); [ "$v" = +6 ] && ok "...the sound played is still voicing $v" || bad "after the reset: voicing '$v' (want +6)"
stload 1 95 || bad "the reset pad 4 did not load"
v=$(vo); [ "$v" = 0 ] && ok "the reset pad 4 loaded: voicing $v (the firmware as first installed)" || bad "the reset state: voicing '$v' (want 0)"

# ---- a state as an older build saved it (no .mem): its flash; a .mem that goes with another image: its flash too
cp "$CRS/state-01.bin" "$CRS/state-05.bin"
sed 's/,"mem":"[0-9a-f]*"//' "$CRS/state-01.json" > "$CRS/state-05.json"
cp "$CRS/state-01.bin" "$CRS/state-06.bin"; cp "$CRS/state-01.mem" "$CRS/state-06.mem"; cp "$CRS/state-01.json" "$CRS/state-06.json"
printf 'X' | dd of="$CRS/state-06.bin" bs=1 seek=$(( $(stat -c %s "$CRS/state-06.bin") - 9 )) conv=notrunc 2>/dev/null
states 1; cmd "cc 60 127"; sleep 0.2; hit 93; sleep 0.2; cmd "cc 60 0"; sleep 0.5; back   # (a Copy: the States see the new files)
stload 1 97 || bad "pad 6 (an older build's) did not load"
v=$(vo); [ "$(field restored0)" = 0 ] && grep -q "slot 1: state 6 into the project: its flash only: no working memory saved with it" "$T/host.log" \
    && ok "a state as an older build saved it (no .mem): loaded from its flash (voicing $v: the flash never had the turns), said in the log" \
    || bad "the older build's state: restored '$(field restored0)', voicing '$v'"
stload 1 98 || bad "pad 7 did not load"
[ "$(field restored0)" = 0 ] && grep -q "slot 1: state 7 into the project: its flash only: its working memory goes with another flash image" "$T/host.log" \
    && ok "a .mem beside an image it was not taken with: not used (the flash only)" || bad "pad 7: restored '$(field restored0)'"
stload 1 94 || bad "pad 3 again"

# ---- Felucca, single core, in slot 2: PRESETS turned (Move knob 2), pads 2 and 3 stored, pad 2 back: its screen
load 1 felucca 1; wait_for 15 '[ "$(field id1)" = felucca ] && [ "$(field active)" = 1 ]' || bad "Felucca into slot 2"
sleep 2
cmd "cc 43 127"; cmd "note 92 100"; cmd "note 92 0"; cmd "cc 43 0"; sleep 0.5   # (FM-1 mode)
fpre() { cmd "note 1 127"; _d=$1; while [ "$_d" -gt 0 ]; do cmd "cc 72 1" "cc 72 1"; _d=$((_d - 1)); sleep 0.1; done; cmd "note 1 0"; sleep 2.5; }
fpre 2; la=$(lcd 1)
stsave 2 93 "$FES/state-01.bin" || bad "Felucca's pad 2"
fpre 3; lb=$(lcd 1)
stsave 2 94 "$FES/state-02.bin" || bad "Felucca's pad 3"
[ -n "$la" ] && [ "$la" != "$lb" ] || bad "Felucca's screen did not change with PRESETS ($la, $lb)"
stload 2 93 || bad "Felucca's pad 2 did not load"
sleep 2.5; l1=$(lcd 1)
[ "$l1" = "$la" ] && [ "$(field restored1)" = 1 ] \
    && ok "Felucca 1.8.2 (one core): PRESETS turned, pad 2 stored; PRESETS, pad 3; pad 2 loaded: restored, its screen as it was ($la), not pad 3's ($lb)" \
    || bad "Felucca's pad 2: screen $l1 (want $la, pad 3's $lb), restored '$(field restored1)'"
stload 2 94 || bad "Felucca's pad 3 did not load"
sleep 2.5; l2=$(lcd 1)
[ "$l2" = "$lb" ] && ok "Felucca's pad 3 loaded: its screen ($lb)" || bad "Felucca's pad 3: screen $l2 (want $lb)"
late1=$(field late)
[ "$late1" = "$late0" ] && ok "no late block while the states were saved and loaded (late $late0 -> $late1; costliest snapshot $(field snap_max_us) us, restore $(field restore_max_us) us)" \
    || bad "late blocks: $late0 -> $late1 (snapshots up to $(field snap_max_us) us)"

# ---- the project's copy written while the firmware is played (every 4 s here), not while it is left alone; a
# library state never
cmd "cc 43 127"; cmd "note 92 100"; cmd "note 92 0"; cmd "cc 43 0"; sleep 0.3
load 0 choralroot 1 >/dev/null; wait_for 5 '[ "$(field active)" = 0 ]'
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"; sleep 1.3; hit 16; sleep 0.3; press 51; sleep 0.3   # (project 1 stored)
wait_for 10 '[ -s "$P/move-project-01.rfmmove" ] && [ -s "$P/move-project-01.fw1.mem" ]' || bad "project 1 not stored: $(ls "$P")"
sleep 5
h0=$(jmem "$P/move-project-01.fw1.json"); n0=$(field snaps0); l0=$(jmem "$CRS/state-02.json")
sleep 9
[ "$(field snaps0)" = "$n0" ] && [ "$(jmem "$P/move-project-01.fw1.json")" = "$h0" ] && ok "left alone 9 s: the project's copy not written again ($n0 snapshots)" \
    || bad "left alone: snapshots $n0 -> $(field snaps0)"
turn 2
wait_for 8 '[ "$(jmem "$P/move-project-01.fw1.json")" != "$h0" ]' && [ "$(jmem "$CRS/state-02.json")" = "$l0" ] \
    && ok "played (voicing +2): project 1's copy written again with its working memory, no save asked; library state 3 (loaded last) untouched" \
    || bad "the autosave of the project's copy: its mem hash still $h0, state 3's $(jmem "$CRS/state-02.json") (was $l0)"
v=$(vo); [ "$v" = +9 ] || bad "voicing after 2 more detents: '$v'"

# ---- the owner's point: two projects holding ChoralRoot, each its own sound. Project 1 (A) at +9; an empty pad:
# project 2 (B), ChoralRoot loaded there and turned
vA=$v
projects; hit 93
wait_for 8 '[ "$(field proj_open)" = 1 ] && [ -z "$(field id0)" ]' && [ -s "$P/move-project-01.fw1.mem" ] \
    && [ -n "$(jmem "$P/move-project-01.fw1.json")" ] && ok "an empty pad: project 2; project 1's copies written as it was left, with their working memory (.fw1.mem)" \
    || bad "project 2: proj_open '$(field proj_open)', $(ls "$P")"
press 51; sleep 0.3
load 0 choralroot 1; wait_for 15 '[ "$(field id0)" = choralroot ]' || bad "ChoralRoot into project 2"
sleep 2
v=$(vo); turn 5; vB=$(vo)
[ "$vB" = "+$(( ${v#+} + 5 ))" ] || bad "project 2's ChoralRoot: $v, then '$vB'"
projects; hit 92
wait_for 10 '[ "$(field proj_open)" = 0 ] && [ "$(field id0)" = choralroot ]' || bad "project 1 again: '$(field id0)'"
press 51; sleep 0.5
v=$(vo); [ "$v" = "$vA" ] && [ "$(field restored0)" = 1 ] && grep -q "project 1: slot 1: choralroot with the project's memory (its working memory restored)" "$T/host.log" \
    && ok "project 1 again: ChoralRoot at voicing $v, as it was left (its own copy's working memory)" \
    || bad "project 1's ChoralRoot: voicing '$v' (want $vA), restored '$(field restored0)'"
# in A: library pad 2 (+3) loaded into the project, then changed by playing (+4)
for e in bin mem json; do cp "$CRS/state-01.$e" "$T/lib2.$e"; done
stload 1 93 || bad "pad 2 in project 1"
turn 1; vA=$(vo)
[ "$vA" = +4 ] && cmp -s "$CRS/state-01.bin" "$T/lib2.bin" && cmp -s "$CRS/state-01.mem" "$T/lib2.mem" && cmp -s "$CRS/state-01.json" "$T/lib2.json" \
    && ok "project 1: library pad 2 loaded (+3), played to voicing $vA: the library file byte for byte as it was" \
    || bad "project 1 after pad 2 and a turn: voicing '$vA' (want +4), the library file $(cmp -s "$CRS/state-01.mem" "$T/lib2.mem" && echo same || echo changed)"
sleep 5   # (the copy's autosave: every 4 s while played)
projects; hit 93
wait_for 10 '[ "$(field proj_open)" = 1 ] && [ "$(field id0)" = choralroot ]' || bad "project 2 again"
press 51; sleep 0.5
v=$(vo); [ "$v" = "$vB" ] && ok "project 2 again: voicing $v (its own sound, untouched by project 1's load and playing)" || bad "project 2's ChoralRoot: '$v' (want $vB)"
projects; hit 92
wait_for 10 '[ "$(field proj_open)" = 0 ] && [ "$(field id0)" = choralroot ]' || bad "project 1 once more"
press 51; sleep 0.5
v=$(vo); [ "$v" = "$vA" ] && ok "project 1 once more: voicing $v (its own sound, the loaded library state as played)" || bad "project 1: '$v' (want $vA)"
cmp -s "$CRS/state-01.mem" "$T/lib2.mem" || bad "library state 2 changed by the project switches"

# ---- quit and start again: the open project (1) as it was left; project 2 its own
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
start || { bad "the host did not start again"; tail -20 "$T/host.log"; exit 1; }
wait_for 15 '[ "$(field id0)" = choralroot ] && [ "$(field restored0)" = 1 ]'; sleep 1.5
v3=$(vo); [ "$v3" = "$vA" ] && ok "quit and started again: project 1's ChoralRoot at voicing $v3, as it was left" \
    || bad "after a restart: voicing '$v3' (want $vA), restored '$(field restored0)'"
projects; hit 93
wait_for 10 '[ "$(field proj_open)" = 1 ] && [ "$(field id0)" = choralroot ]' || bad "project 2 after the restart"
press 51; sleep 0.5
v=$(vo); [ "$v" = "$vB" ] && ok "...and project 2: voicing $v (its own sound)" || bad "project 2 after a restart: '$v' (want $vB)"
cmp -s "$CRS/state-01.mem" "$T/lib2.mem" && ok "library state 2 byte for byte as stored, after both projects were played and the restart" \
    || bad "library state 2 changed"
late2=$(field late)
[ "$late2" = 0 ] && ok "no late block since the restart" || bad "late blocks after the restart: $late2"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
rm -f "/dev/shm/rfm14-st-$$"
echo "  (snapshot sizes: ChoralRoot $(stat -c %s "$CRS/state-01.mem") bytes, Felucca $(stat -c %s "$FES/state-01.mem") bytes)"
mv "$T" "$T.done" 2>/dev/null
echo "states_e2e: $checks checks, $fail failed"
exit $fail
