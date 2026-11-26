#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page's States and projects (src/web.c /api/store, engine.c store_run; README "The page's States and projects"),
# on the simulator in the build image after make, through the API the page uses, the sound read back as
# tests/states_e2e.sh reads it: ChoralRoot 0.43 in slot 1 with core 1 on, its voicing turned with knob 1 in its takeover
# and read without a change (knob 1 touched: the screen reader's words, a fake TTS that logs them):
# (rfm1cs 0.14.0, the owner's 2026-11-24: k 0, pad 1, the open project's own sound; k 1..31 the library)
#   - voicing +3, state 2 (k 1) saved from the page (the Move's core has it stored; nothing active); +7, state 3; a
#     save into pad 1 refused; state 2 downloaded (a .zip: the image, the working memory, the .json), deleted,
#     uploaded again (the same bytes), loaded into the project: +3 (its working memory restored, the undo kept),
#     played: the file byte for byte; state 3 loaded: +7;
#   - pad 1: downloaded (the project's copy as a state file, state-00.* inside as in files from before 0.14.0) and
#     uploaded into the library; reset to default (New: 0, the undo kept), Undo on the Move: +7; a state file uploaded
#     onto pad 1: the project's sound replaced and loaded (+3), no state file 0;
#   - Reset to default of library states: a stored one (an erased flash, no .mem, the firmware untouched; loaded: 0),
#     an empty one (stored); refused: an empty slot, another firmware's id;
#   - refused in plain words: Felucca's state into ChoralRoot's slot (nothing written), a file over a stored state
#     without replace=1, a state file as a project, the file browser's own write into a running firmware's states/;
#     with replace=1: stored, the firmware left alone; loaded: the file's sound;
#   - projects: the open one saved (its firmware copy with its working memory) at +5 and downloaded; at +7 saved again;
#     the file uploaded as project 3 and opened: +5 (the copy restored); project 1 again: +7; project 3 duplicated into
#     6 (its copies too), opened: +5; project 6 deleted (its copies gone); an upload over the open project refused;
#     a new empty project from the page (project-new) in an empty slot, refused over a stored one;
#   - no late block meanwhile;
#   - restarted (a damaged project file and a newer one added): the first GET /api/store lists every stored project
#     with its tempo, clips and firmwares, the two said "damaged" and "newer" (and in the log).
#   tests/store_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "store_e2e"
T=$(mktemp -d /tmp/rfm14sv.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; CFG=$T/cfg; SAID=$T/said.log; D=$T/dl; mkdir -p "$SIM" "$R" "$CFG" "$D"
echo "http://127.0.0.1:1" > "$R/catalog-url"
echo '{ "debounce_ms": 0 }' > "$CFG/tts.json"
PORT=17829; U="http://127.0.0.1:$PORT"
P=$R/move/projects; CRS=$R/states/choralroot
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
sfield() { curl -s "$U/api/store" | tr ',{}[]' '\n\n\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
alive() { kill -0 "$HOST" 2>/dev/null; }
cmd() { alive || return 1; printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
mark() { M=$(wc -l < "$SAID" 2>/dev/null || echo 0); }
said() { tail -n +$((M + 1)) "$SAID" 2>/dev/null | grep -E -q "$1"; }
M=0
# the page's requests: POST /api/store (answered when done), a file up, a file down; their HTTP status in $code, the
# answer in $T/out
store() { code=$(curl -s -o "$T/out" -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "$1" "$U/api/store"); }
put() { code=$(curl -s -o "$T/out" -w '%{http_code}' -X POST --data-binary @"$2" "$U/api/store/put?$1"); }
get() { code=$(curl -s -o "$2" -w '%{http_code}' "$U/api/store/$1"); }
said_out() { sed -n 's/.*"\(message\|error\)":"\([^"]*\)".*/\2/p' "$T/out"; }
# a store-only .zip's file by name, out of it (the Move's own zips: tests/page_e2e.mjs reads them the same way)
unzip1() { busybox unzip -p "$1" "*/$2" > "$3" 2>/dev/null; }
# ChoralRoot's voicing, read without changing it (tests/states_e2e.sh): its takeover, knob 1 touched, its words
tk() { [ "$(field move_mode)" = 4 ] && return 0; cmd "cc 43 127"; cmd "note 93 100"; cmd "note 93 0"; cmd "cc 43 0"; wait_for 3 '[ "$(field move_mode)" = 4 ]'; sleep 1.6; }
vo() {
    tk; mark; cmd "note 0 127"
    wait_for 3 'said "^Voicing"'; cmd "note 0 0"
    tail -n +$((M + 1)) "$SAID" | grep "^Voicing" | tail -1 | sed 's/^Voicing //; s/^plus /+/; s/^minus /-/'
}
# (a firmware just loaded or booted: its takeover and its words a moment later)
vo2() { sleep 1; _v=$(vo); [ -n "$_v" ] || { sleep 1.5; _v=$(vo); }; echo "$_v"; }
turn() { tk; _d=$1; while [ "$_d" -gt 0 ]; do cmd "cc 71 1"; _d=$((_d - 1)); sleep 0.1; done; sleep 0.4; }
opened() { _p=$1; wait_for 20 '[ "$(field proj_cur)" = "$_p" ] && [ "$(field pfw_loading)" = 0 ]'; }
start() {
    FAKE_TTS_SHM=/rfm14-sv-$$ FAKE_TTS_LOG=$SAID build/sim/remotefm1-host --root "$R" --port $PORT \
        --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw choralroot --test-api --no-fm1 --screen-reader on \
        --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-sv-$$ --tts-config "$CFG" >>"$T/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"'
}
stop() { printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST; }
start || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
wait_for 10 '[ "$(field snapable0)" = 1 ]' && ok "ChoralRoot 0.43 in slot 1 (its snapshots whole: the working memory goes with a state)" \
    || bad "slot 1: snapable '$(field snapable0)'"
sleep 2
late0=$(field late)

# ---- States from the page (rfm1cs 0.14.0, the owner's 2026-11-24: k 0, pad 1, is the open project's own sound; k
# 1..31 the library): saved, downloaded, deleted, uploaded, loaded into the project
v=$(vo); [ "$v" = 0 ] || bad "the voicing at start: '$v'"
turn 3; v=$(vo)
store '{"op":"state-save","slot":0,"k":1,"id":"choralroot"}'
[ "$code" = 200 ] && [ -s "$CRS/state-01.bin" ] && [ -s "$CRS/state-01.mem" ] && wait_for 3 '[ "$(field states0)" = 2 ]' && [ "$(field cstcur0)" = -1 ] \
    && ok "voicing $v, library state 2 (k 1) saved from the page (\"$(said_out)\"): its image and working memory; the Move's core has it stored, nothing active" \
    || bad "state-save: $code $(cat "$T/out"), states0 '$(field states0)', cstcur0 '$(field cstcur0)'"
turn 4; v=$(vo)
store '{"op":"state-save","slot":0,"k":2}'
[ "$code" = 200 ] && wait_for 3 '[ "$(field states0)" = 6 ]' && ok "voicing $v, state 3 saved from the page" || bad "state 3: $code $(cat "$T/out")"
store '{"op":"state-save","slot":0,"k":1}'
[ "$code" = 409 ] && ok "a save into a stored state without replace refused: \"$(said_out)\"" || bad "save over without replace: $code"
store '{"op":"state-save","slot":0,"k":0}'
[ "$code" = 409 ] && said_out | grep -q "project's own sound" && [ ! -e "$CRS/state-00.bin" ] \
    && ok "a state save into pad 1 refused: \"$(said_out)\"" || bad "state-save k 0: $code $(cat "$T/out")"
get "state?slot=0&k=1" "$D/s1.zip"
unzip1 "$D/s1.zip" state-01.bin "$D/s1.bin"; unzip1 "$D/s1.zip" state-01.mem "$D/s1.mem"; unzip1 "$D/s1.zip" state-01.json "$D/s1.json"
[ "$code" = 200 ] && cmp -s "$D/s1.bin" "$CRS/state-01.bin" && cmp -s "$D/s1.mem" "$CRS/state-01.mem" && grep -q '"id":"choralroot","version":"0.43"' "$D/s1.json" \
    && grep -q '"mem":"' "$D/s1.json" && ok "state 2 downloaded: a .zip of its image, working memory and .json (choralroot 0.43, the hashes)" \
    || bad "state 2's file: $code, $(busybox unzip -l "$D/s1.zip" 2>&1 | tail -4)"
store '{"op":"state-delete","slot":0,"k":1}'
[ "$code" = 200 ] && [ ! -e "$CRS/state-01.bin" ] && [ ! -e "$CRS/state-01.mem" ] && wait_for 3 '[ "$(field states0)" = 4 ]' \
    && ok "state 2 deleted from the page: its files gone, the core's pads with it" || bad "delete: $code $(cat "$T/out") $(ls "$CRS")"
put "kind=state&slot=0&k=1" "$D/s1.zip"
[ "$code" = 200 ] && cmp -s "$D/s1.bin" "$CRS/state-01.bin" && cmp -s "$D/s1.mem" "$CRS/state-01.mem" && wait_for 3 '[ "$(field states0)" = 6 ]' \
    && ok "state 2's file uploaded again: the same bytes in place" || bad "upload: $code $(cat "$T/out")"
b0=$(field boot0)
store '{"op":"state-load","slot":0,"k":1}'
v=$(vo2)
[ "$code" = 200 ] && said_out | grep -q "into the project" && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = +3 ] && [ "$(field restored0)" = 1 ] \
    && [ "$(field undo0)" = 1 ] && ok "state 2 loaded from the page (\"$(said_out)\"): voicing $v, its working memory restored from the file; the undo kept" \
    || bad "state 2 loaded: $code $(cat "$T/out"), voicing '$v' (want +3), restored '$(field restored0)', undo '$(field undo0)'"
turn 1; v=$(vo)
cmp -s "$D/s1.mem" "$CRS/state-01.mem" && cmp -s "$D/s1.bin" "$CRS/state-01.bin" \
    && ok "played after the load (voicing $v): library state 2 byte for byte as it was" || bad "library state 2 changed by playing"
store '{"op":"state-load","slot":0,"k":2}'; v=$(vo2)
[ "$v" = +7 ] && ok "state 3 loaded from the page: voicing $v" || bad "state 3: voicing '$v' (want +7)"

# ---- pad 1, the project's own sound: downloaded (the project's copy as a state file), uploaded (replaces it and
# loads it), reset to default (New: the firmware restarts fresh, the undo kept)
open0=$(field proj_cur)
store "{\"op\":\"project-save\",\"k\":$open0}"
get "state?slot=0&k=0" "$D/p0.zip"
unzip1 "$D/p0.zip" state-00.bin "$D/p0.bin"; unzip1 "$D/p0.zip" state-00.mem "$D/p0.mem"
pc=$(printf '%s/move-project-%02d.fw1' "$P" $((open0 + 1)))
[ "$code" = 200 ] && cmp -s "$D/p0.bin" "$pc.bin" && cmp -s "$D/p0.mem" "$pc.mem" \
    && ok "pad 1 downloaded: project $((open0 + 1))'s copy of ChoralRoot as a state file (state-00.*, voicing +7)" \
    || bad "pad 1's file: $code, $(busybox unzip -l "$D/p0.zip" 2>&1 | tail -4)"
put "kind=state&slot=0&k=11" "$D/p0.zip"
[ "$code" = 200 ] && cmp -s "$CRS/state-11.mem" "$D/p0.mem" && ok "...uploaded into the library (state 12): the same bytes (its state-00.* as in every file downloaded before 0.14.0)" || bad "pad 1's file as state 12: $code $(cat "$T/out")"
b0=$(field boot0)
store '{"op":"state-reset","slot":0,"k":0,"id":"choralroot"}'; v=$(vo2)
[ "$code" = 200 ] && said_out | grep -q "project's sound reset" && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = 0 ] && [ "$(field undo0)" = 1 ] \
    && [ ! -e "$CRS/state-00.bin" ] && ok "pad 1 reset to default from the page (\"$(said_out)\"): the firmware restarted fresh (voicing $v), the undo kept" \
    || bad "pad 1 reset: $code $(cat "$T/out"), voicing '$v', undo '$(field undo0)'"
cmd "cc 43 127"; cmd "note 98 100"; cmd "note 98 0"; cmd "cc 43 0"; wait_for 3 '[ "$(field states_open)" = 0 ]'; sleep 0.6
b0=$(field boot0); cmd "cc 56 127"; cmd "cc 56 0"
wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field undo0)" = 0 ]'
for i in 1 2 3; do [ "$(field states_open)" = -1 ] || { cmd "cc 51 127"; cmd "cc 51 0"; sleep 0.4; }; done
v=$(vo2); [ "$v" = +7 ] && ok "...Undo on the Move (States, Undo): voicing $v back" || bad "Undo after the page's reset: voicing '$v' (want +7)"
b0=$(field boot0)
put "kind=state&slot=0&k=0" "$D/s1.zip"; v=$(vo2)
[ "$code" = 200 ] && said_out | grep -q "project's sound replaced" && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = +3 ] && [ "$(field restored0)" = 1 ] \
    && [ ! -e "$CRS/state-00.bin" ] && wait_for 8 'cmp -s "$pc.mem" "$D/s1.mem" || ! cmp -s "$pc.mem" "$D/p0.mem"' \
    && ok "a state file uploaded onto pad 1: the project's sound replaced and loaded (\"$(said_out)\", voicing $v); no state file 0" \
    || bad "upload onto pad 1: $code $(cat "$T/out"), voicing '$v'"
store '{"op":"state-load","slot":0,"k":2}'; v=$(vo2)
[ "$v" = +7 ] || bad "state 3 again: '$v'"

# ---- Reset to default of library states (the owner's "an option to reset the selected state to default"): the
# firmware as first installed stored there (an erased flash, no working memory), the firmware untouched
erased() { [ -s "$1" ] && [ "$(tr -d '\377' < "$1" | wc -c)" = 0 ]; }
b0=$(field boot0)
store '{"op":"state-reset","slot":0,"k":1,"id":"choralroot"}'; v=$(vo)
[ "$code" = 200 ] && said_out | grep -q "State 2 reset to default" && erased "$CRS/state-01.bin" && [ ! -e "$CRS/state-01.mem" ] \
    && grep -q '"fresh":"1"' "$CRS/state-01.json" && [ "$(field boot0)" = "$b0" ] && [ "$v" = +7 ] \
    && ok "stored state 2 reset to default (\"$(said_out)\"): an erased flash, no .mem; the firmware untouched (voicing $v)" \
    || bad "reset state 2: $code $(cat "$T/out"), voicing '$v', boot $b0 -> $(field boot0)"
store '{"op":"state-load","slot":0,"k":1}'; v=$(vo2)
[ "$code" = 200 ] && [ "$v" = 0 ] && ok "state 2 loaded: voicing $v (the firmware as first installed)" || bad "state 2 after its reset: $code, voicing '$v' (want 0)"
store '{"op":"state-load","slot":0,"k":2}'; v=$(vo2)
[ "$v" = +7 ] || bad "state 3 again: voicing '$v' (want +7)"
st=$(field states0); b0=$(field boot0)
store '{"op":"state-reset","slot":0,"k":4,"id":"choralroot"}'; v=$(vo)
[ "$code" = 200 ] && erased "$CRS/state-04.bin" && wait_for 3 '[ $(( $(field states0) >> 4 & 1 )) = 1 ]' && [ "$v" = +7 ] && [ "$(field boot0)" = "$b0" ] \
    && ok "an empty state 5 reset to default: a fresh state stored there (states $st -> $(field states0)), the firmware untouched" \
    || bad "reset the empty state 5: $code $(cat "$T/out"), states0 '$(field states0)'"
store '{"op":"state-reset","slot":1,"k":3}'
[ "$code" = 409 ] && said_out | grep -q "empty" && [ ! -d "$R/states/felucca" ] && ok "a reset in an empty slot refused: \"$(said_out)\"" \
    || bad "reset in an empty slot: $code $(cat "$T/out")"
store '{"op":"state-reset","slot":0,"k":3,"id":"felucca"}'
[ "$code" = 409 ] && said_out | grep -q "runs ChoralRoot" && [ ! -e "$CRS/state-03.bin" ] && ok "a reset meant for another firmware refused: \"$(said_out)\"" \
    || bad "reset for felucca in ChoralRoot's slot: $code $(cat "$T/out")"

# ---- refusals, in plain words
curl -s -o /dev/null -X POST --data '{"slot":1,"id":"felucca","activate":0}' "$U/api/load"
wait_for 15 '[ "$(field id1)" = felucca ]'; sleep 1
store '{"op":"state-save","slot":1,"k":1}'
get "state?slot=1&k=1" "$D/fe.zip"
put "kind=state&slot=0&k=5" "$D/fe.zip"
[ "$code" = 422 ] && said_out | grep -q "state of Felucca 1.8.2, and slot 1 runs ChoralRoot" && [ ! -e "$CRS/state-05.bin" ] \
    && ok "Felucca's state into ChoralRoot's slot refused: \"$(said_out)\"" || bad "Felucca's state: $code $(cat "$T/out")"
put "kind=state&slot=0&k=2" "$D/s1.zip"
[ "$code" = 409 ] && said_out | grep -q "holds a sound" && ok "a file over a stored state without replace=1 refused: \"$(said_out)\"" || bad "over a stored one: $code $(cat "$T/out")"
put "kind=project&k=4" "$D/s1.zip"
[ "$code" = 422 ] && said_out | grep -q "a state file" && ok "a state file as a project refused: \"$(said_out)\"" || bad "a state as a project: $code $(cat "$T/out")"
fc=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary @"$D/s1.bin" "$U/api/fs/put?path=states/choralroot&name=state-09.bin")
[ "$fc" = 403 ] && ok "the file browser still leaves a running firmware's states/ alone (403)" || bad "fs/put into states/choralroot: $fc"
b0=$(field boot0)
put "kind=state&slot=0&k=2&replace=1" "$D/s1.zip"; v=$(vo)
[ "$code" = 200 ] && [ "$(field boot0)" = "$b0" ] && [ "$v" = +7 ] && cmp -s "$D/s1.mem" "$CRS/state-02.mem" \
    && ok "...with replace=1: state 3 replaced by the file, the firmware left as it is (voicing $v)" || bad "replace: $code $(cat "$T/out"), voicing '$v'"
store '{"op":"state-load","slot":0,"k":2}'; v=$(vo2)
[ "$v" = +3 ] && ok "the replaced state 3 loaded: voicing $v (the file's)" || bad "the replaced state 3: '$v' (want +3)"

# ---- projects from the page: saved, downloaded, uploaded, opened, duplicated, deleted
open0=$(field proj_cur)
f1=move-project-$(printf %02d $((open0 + 1)))
turn 2; v5=$(vo)
store "{\"op\":\"project-save\",\"k\":$open0}"
[ "$code" = 200 ] && [ -s "$P/$f1.rfmmove" ] && [ -s "$P/$f1.fw1.bin" ] && [ -s "$P/$f1.fw1.mem" ] && [ -s "$P/$f1.fw2.bin" ] \
    && ok "voicing $v5, the open project $((open0 + 1)) saved from the page (\"$(said_out)\"): the project, ChoralRoot's and Felucca's copies with their working memory" \
    || bad "project-save: $code $(cat "$T/out") $(ls "$P")"
get "project?k=$open0" "$D/p1.zip"
n=$(busybox unzip -l "$D/p1.zip" 2>/dev/null | grep -c "$f1\.")
[ "$code" = 200 ] && [ "$n" -ge 8 ] && ok "project $((open0 + 1)) downloaded: $n files (.rfmmove, .fx.json, its firmware copies)" || bad "the project's file: $code, $n files"
turn 2; v7=$(vo)
store "{\"op\":\"project-save\",\"k\":$open0}"
put "kind=project&k=2" "$D/p1.zip"
[ "$code" = 200 ] && [ -s "$P/move-project-03.rfmmove" ] && [ -s "$P/move-project-03.fw1.mem" ] \
    && ok "voicing $v7 saved into project $((open0 + 1)); its file from before uploaded as project 3 (its copies with it)" || bad "upload project 3: $code $(cat "$T/out")"
store '{"op":"project-open","k":2}'; opened 2; v=$(vo2)
[ "$code" = 200 ] && [ "$v" = "$v5" ] && grep -q "project 3: slot 1: choralroot with the project's memory (its working memory restored)" "$T/host.log" \
    && ok "project 3 opened from the page: voicing $v (the uploaded copy's working memory)" || bad "project 3: $code, voicing '$v' (want $v5)"
store "{\"op\":\"project-open\",\"k\":$open0}"; opened "$open0"; v=$(vo2)
[ "$v" = "$v7" ] && ok "project $((open0 + 1)) again: voicing $v" || bad "project $((open0 + 1)) again: voicing '$v' (want $v7)"
store '{"op":"project-dup","k":2,"to":5}'
[ "$code" = 200 ] && cmp -s "$P/move-project-03.fw1.mem" "$P/move-project-06.fw1.mem" && cmp -s "$P/move-project-03.fx.json" "$P/move-project-06.fx.json" \
    && ok "project 3 duplicated into 6 from the page: its firmware copies too" || bad "duplicate: $code $(cat "$T/out")"
store '{"op":"project-dup","k":2,"to":5}'
[ "$code" = 409 ] && ok "a duplicate into a stored project refused: \"$(said_out)\"" || bad "duplicate over: $code"
store '{"op":"project-open","k":5}'; opened 5; v=$(vo2)
[ "$v" = "$v5" ] && ok "project 6 opened: voicing $v" || bad "project 6: voicing '$v' (want $v5)"
put "kind=project&k=5&replace=1" "$D/p1.zip"
[ "$code" = 409 ] && said_out | grep -q "open on the Move" && ok "an upload over the open project refused: \"$(said_out)\"" || bad "over the open project: $code $(cat "$T/out")"
store "{\"op\":\"project-open\",\"k\":$open0}"; opened "$open0"
store '{"op":"project-delete","k":5}'
[ "$code" = 200 ] && [ ! -e "$P/move-project-06.rfmmove" ] && [ ! -e "$P/move-project-06.fw1.bin" ] && [ ! -e "$P/move-project-06.fw1.mem" ] \
    && [ $(( $(sfield used) >> 5 & 1 )) = 0 ] && ok "project 6 deleted from the page: the project and its copies gone, the core's grid with it" \
    || bad "delete project 6: $code $(cat "$T/out")"
# ---- a new empty project from the page (the owner's "no way to start new project in empty slot"; rfm1cs 0.14.0
# rfm_move_project_new: as an empty pad's click on the Move)
store '{"op":"project-new","k":2}'
[ "$code" = 409 ] && said_out | grep -q "not empty" && ok "a new project over a stored one refused: \"$(said_out)\"" || bad "project-new over project 3: $code $(cat "$T/out")"
store '{"op":"project-new","k":9}'; opened 9
[ "$code" = 200 ] && said_out | grep -q "New project 10" && [ "$(field proj_cur)" = 9 ] && [ -z "$(field id0)" ] && [ -z "$(field id1)" ] \
    && [ -s "$P/move-project-10.rfmmove" ] && [ -s "$P/$f1.fw1.mem" ] \
    && ok "a new project from the page (\"$(said_out)\"): project 10 open and stored, its slots empty; project $((open0 + 1)) kept its copies" \
    || bad "project-new: $code $(cat "$T/out"), proj_cur '$(field proj_cur)', slots '$(field id0)' '$(field id1)'"
store "{\"op\":\"project-open\",\"k\":$open0}"; opened "$open0"; v=$(vo2)
[ "$v" = "$v7" ] && ok "project $((open0 + 1)) again after the new one: voicing $v" || bad "project $((open0 + 1)) after project-new: '$v' (want $v7)"
# ---- the Move's own "Saved" (rfm1cs 0.14.0, the owner's "Clicking the open project again should show Saved and save
# it"): the projects' page (Track 4 held + step 1), the open project's pad clicked: its firmware copies written with
# the sound as it is at the click (PROJECT_SAVE with value 1), nothing reloaded
cp "$P/$f1.fw1.mem" "$T/before.mem"; boot0=$(field boot0)
turn 1; v8=$(vo)
pn=$((68 + (3 - open0 / 8) * 8 + open0 % 8))
cmd "cc 40 127"; cmd "note 16 127"; cmd "note 16 0"; cmd "cc 40 0"; sleep 0.3
cmd "note $pn 100"; cmd "note $pn 0"
wait_for 5 'grep -q "project $((open0 + 1)) saved by its pad" "$T/host.log"' && sleep 0.5
grep -q "project $((open0 + 1)) saved by its pad" "$T/host.log" && ! cmp -s "$P/$f1.fw1.mem" "$T/before.mem" \
    && [ "$(field proj_cur)" = "$open0" ] && [ "$(field boot0)" = "$boot0" ] \
    && ok "the open project's pad clicked on the Move: saved with the sound as it is now (voicing $v8), nothing reloaded" \
    || bad "the Move's Saved click: voicing '$v8', proj_cur $(field proj_cur), boot $boot0 -> $(field boot0), $(grep -c 'saved by its pad' "$T/host.log") log lines"
cmd "cc 51 127"; cmd "cc 51 0"
late=$(field late)
[ "$late" = "$late0" ] && ok "no late block while states and projects went in and out ($late)" || bad "late blocks: $late0 -> $late"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
# ---- after a start, the panel's first listing complete: every stored project's tempo, clips and firmwares (read on
# the storage thread at start, as the Move's grid needs them too); a damaged file and a newer one said in a word
cp "$P/move-project-03.rfmmove" "$P/move-project-20.rfmmove"
printf 'Z' | dd of="$P/move-project-20.rfmmove" bs=1 seek=60 conv=notrunc 2>/dev/null
cp "$P/move-project-03.rfmmove" "$P/move-project-21.rfmmove"   # (its version byte past any this host reads: 11 was
                                                                # when it read 10; the kit's version 15 on 2026-11-26)
printf '\377' | dd of="$P/move-project-21.rfmmove" bs=1 seek=22 conv=notrunc 2>/dev/null
start || { bad "the host did not start again"; tail -20 "$T/host.log"; exit 1; }
curl -s "$U/api/store" > "$T/store1.json"
# (the projects' array, one object a line: k, bpm, read and the first firmware)
sed 's/.*"projects":\[//; s/\],"slots".*//; s/},{/}\n{/g' "$T/store1.json" > "$T/projs"
r=$(awk -F'"' '
    { k = $0; sub(/.*"k":/, "", k); sub(/,.*/, "", k); bpm = $0; sub(/.*"bpm":/, "", bpm); sub(/,.*/, "", bpm)
      rd = $0; sub(/.*"read":"/, "", rd); sub(/".*/, "", rd); fw = $0; sub(/.*"fw":\["/, "", fw); sub(/".*/, "", fw); n++
      if (k == 19) { if (rd != "damaged") bad = bad " 20:" rd; next }
      if (k == 20) { if (rd != "newer") bad = bad " 21:" rd; next }
      if (rd != "ok" || bpm + 0 <= 0) bad = bad " " k + 1 ":" rd "/" bpm
      if (k == 2 && fw != "choralroot") bad = bad " 3:fw " fw }
    END { print bad != "" ? "bad" bad : "ok " n }' "$T/projs")
case "$r" in
    ok*) ok "restarted: the first GET /api/store lists every stored project (${r#ok } of them) with its tempo, clips and firmwares; project 20 \"damaged\", 21 \"newer\"" ;;
    *) bad "the first listing after a start: $r ($(head -c 400 "$T/store1.json"))" ;;
esac
grep -q "project 20: its file cannot be read" "$T/host.log" && grep -q "project 21: a newer project file" "$T/host.log" \
    && [ "$(field late)" = 0 ] && ok "...said in the log; no late block" || bad "the log or late blocks ($(field late))"
stop
rm -f "/dev/shm/rfm14-sv-$$"
mv "$T" "$T.done" 2>/dev/null
echo "store_e2e: $checks checks, $fail failed"
exit $fail
