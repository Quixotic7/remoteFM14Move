#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page's States and projects (src/web.c /api/store, engine.c store_run; README "The page's States and projects"),
# on the simulator in the build image after make, through the API the page uses, the sound read back as
# tests/states_e2e.sh reads it: ChoralRoot 0.43 in slot 1 with core 1 on, its voicing turned with knob 1 in its takeover
# and read without a change (knob 1 touched: the screen reader's words, a fake TTS that logs them):
#   - voicing +3, state 1 saved from the page (the Move's core has it stored and active: its pads show it); +7, state 2;
#   - state 1 downloaded (a .zip: the image, the working memory, the .json), deleted, uploaded again (the same bytes),
#     loaded: +3 (its working memory restored); state 2 loaded: +7;
#   - Reset to default (state-reset): state 1 (stored, not active): an erased flash, no .mem, the firmware still +7;
#     loaded: 0; the active state 2: the firmware restarts fresh (0), state 2 still active; the empty state 5: a fresh
#     state stored there; refused: an empty slot, another firmware's id;
#   - refused in plain words: Felucca's state into ChoralRoot's slot (nothing written), a file over the active state
#     without load=1, a state file as a project, the file browser's own write into a running firmware's states/ (its
#     guard as it was); over the active state with load=1: replaced and loaded (+3);
#   - projects: the open one saved (its firmware copy with its working memory) at +5 and downloaded; at +7 saved again;
#     the file uploaded as project 3 and opened: +5 (the copy restored); project 1 again: +7; project 3 duplicated into
#     6 (its copies too), opened: +5; project 6 deleted (its copies gone); an upload over the open project refused;
#   - no late block meanwhile.
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

# ---- States from the page: saved, downloaded, deleted, uploaded, loaded
v=$(vo); [ "$v" = 0 ] || bad "the voicing at start: '$v'"
turn 3; v=$(vo)
store '{"op":"state-save","slot":0,"k":0,"id":"choralroot"}'
[ "$code" = 200 ] && [ -s "$CRS/state-00.bin" ] && [ -s "$CRS/state-00.mem" ] && wait_for 3 '[ "$(field states0)" = 1 ] && [ "$(field cstcur0)" = 0 ]' \
    && ok "voicing $v, state 1 saved from the page (\"$(said_out)\"): its image and working memory; the Move's core has it stored and active" \
    || bad "state-save: $code $(cat "$T/out"), states0 '$(field states0)', cstcur0 '$(field cstcur0)'"
turn 4; v=$(vo)
store '{"op":"state-save","slot":0,"k":1}'
[ "$code" = 200 ] && wait_for 3 '[ "$(field states0)" = 3 ] && [ "$(field cstcur0)" = 1 ]' && ok "voicing $v, state 2 saved from the page" || bad "state 2: $code $(cat "$T/out")"
store '{"op":"state-save","slot":0,"k":0}'
[ "$code" = 409 ] && ok "a save into a stored state without replace refused: \"$(said_out)\"" || bad "save over without replace: $code"
get "state?slot=0&k=0" "$D/s1.zip"
unzip1 "$D/s1.zip" state-00.bin "$D/s1.bin"; unzip1 "$D/s1.zip" state-00.mem "$D/s1.mem"; unzip1 "$D/s1.zip" state-00.json "$D/s1.json"
[ "$code" = 200 ] && cmp -s "$D/s1.bin" "$CRS/state-00.bin" && cmp -s "$D/s1.mem" "$CRS/state-00.mem" && grep -q '"id":"choralroot","version":"0.43"' "$D/s1.json" \
    && grep -q '"mem":"' "$D/s1.json" && ok "state 1 downloaded: a .zip of its image, working memory and .json (choralroot 0.43, the hashes)" \
    || bad "state 1's file: $code, $(busybox unzip -l "$D/s1.zip" 2>&1 | tail -4)"
store '{"op":"state-delete","slot":0,"k":0}'
[ "$code" = 200 ] && [ ! -e "$CRS/state-00.bin" ] && [ ! -e "$CRS/state-00.mem" ] && wait_for 3 '[ "$(field states0)" = 2 ]' \
    && ok "state 1 deleted from the page: its files gone, the core's pads with it" || bad "delete: $code $(cat "$T/out") $(ls "$CRS")"
put "kind=state&slot=0&k=0" "$D/s1.zip"
[ "$code" = 200 ] && cmp -s "$D/s1.bin" "$CRS/state-00.bin" && cmp -s "$D/s1.mem" "$CRS/state-00.mem" && wait_for 3 '[ "$(field states0)" = 3 ]' \
    && ok "state 1's file uploaded again: the same bytes in place" || bad "upload: $code $(cat "$T/out")"
b0=$(field boot0)
store '{"op":"state-load","slot":0,"k":0}'
v=$(vo2)
[ "$code" = 200 ] && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = +3 ] && [ "$(field restored0)" = 1 ] && [ "$(field cstcur0)" = 0 ] \
    && ok "state 1 loaded from the page: voicing $v again, its working memory restored from the file (round trip)" \
    || bad "state 1 loaded: $code, voicing '$v' (want +3), restored '$(field restored0)'"
store '{"op":"state-load","slot":0,"k":1}'; v=$(vo2)
[ "$v" = +7 ] && ok "state 2 loaded from the page: voicing $v" || bad "state 2: voicing '$v' (want +7)"

# ---- Reset to default (the owner's "an option to reset the selected state to default"): the firmware as first
# installed (step 14 New's), a fresh (erased) flash and no working memory
erased() { [ -s "$1" ] && [ "$(tr -d '\377' < "$1" | wc -c)" = 0 ]; }
b0=$(field boot0)
store '{"op":"state-reset","slot":0,"k":0,"id":"choralroot"}'; v=$(vo)
[ "$code" = 200 ] && said_out | grep -q "State 1 reset to default" && erased "$CRS/state-00.bin" && [ ! -e "$CRS/state-00.mem" ] \
    && [ "$(field boot0)" = "$b0" ] && [ "$v" = +7 ] && [ "$(field cstcur0)" = 1 ] \
    && ok "state 1 (stored, not active) reset to default (\"$(said_out)\"): an erased flash, no .mem; the firmware untouched (voicing $v, state 2 active)" \
    || bad "reset state 1: $code $(cat "$T/out"), voicing '$v', boot $b0 -> $(field boot0), cstcur0 '$(field cstcur0)'"
store '{"op":"state-load","slot":0,"k":0}'; v=$(vo2)
[ "$code" = 200 ] && [ "$v" = 0 ] && ok "state 1 loaded: voicing $v (the firmware as first installed)" || bad "state 1 after its reset: $code, voicing '$v' (want 0)"
store '{"op":"state-load","slot":0,"k":1}'; v=$(vo2)
[ "$v" = +7 ] || bad "state 2 again: voicing '$v' (want +7)"
b0=$(field boot0)
store '{"op":"state-reset","slot":0,"k":1,"id":"choralroot"}'; v=$(vo2)
[ "$code" = 200 ] && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = 0 ] && [ "$(field cstcur0)" = 1 ] && [ "$(field restored0)" = 0 ] \
    && erased "$CRS/state-01.bin" && [ ! -e "$CRS/state-01.mem" ] && grep -q "state 2 reset to default: the firmware restarted fresh" "$T/host.log" \
    && ok "the active state 2 reset to default: the firmware restarted fresh (voicing $v), state 2 still active, its file the fresh flash" \
    || bad "reset the active state: $code $(cat "$T/out"), voicing '$v', boot $b0 -> $(field boot0), cstcur0 '$(field cstcur0)', restored0 '$(field restored0)'"
st=$(field states0)
store '{"op":"state-reset","slot":0,"k":4,"id":"choralroot"}'; v=$(vo)
[ "$code" = 200 ] && erased "$CRS/state-04.bin" && wait_for 3 '[ $(( $(field states0) >> 4 & 1 )) = 1 ]' && [ "$(field cstcur0)" = 1 ] && [ "$v" = 0 ] \
    && ok "an empty state 5 reset to default: a fresh state stored there (states $st -> $(field states0)), state 2 still the active one" \
    || bad "reset the empty state 5: $code $(cat "$T/out"), states0 '$(field states0)', cstcur0 '$(field cstcur0)'"
store '{"op":"state-reset","slot":1,"k":0}'
[ "$code" = 409 ] && said_out | grep -q "empty" && [ ! -d "$R/states/felucca" ] && ok "a reset in an empty slot refused: \"$(said_out)\"" \
    || bad "reset in an empty slot: $code $(cat "$T/out")"
store '{"op":"state-reset","slot":0,"k":2,"id":"felucca"}'
[ "$code" = 409 ] && said_out | grep -q "runs ChoralRoot" && [ ! -e "$CRS/state-02.bin" ] && ok "a reset meant for another firmware refused: \"$(said_out)\"" \
    || bad "reset for felucca in ChoralRoot's slot: $code $(cat "$T/out")"

# ---- refusals, in plain words
curl -s -o /dev/null -X POST --data '{"slot":1,"id":"felucca","activate":0}' "$U/api/load"
wait_for 15 '[ "$(field id1)" = felucca ]'; sleep 1
store '{"op":"state-save","slot":1,"k":0}'
get "state?slot=1&k=0" "$D/fe.zip"
put "kind=state&slot=0&k=5" "$D/fe.zip"
[ "$code" = 422 ] && said_out | grep -q "state of Felucca 1.8.2, and slot 1 runs ChoralRoot" && [ ! -e "$CRS/state-05.bin" ] \
    && ok "Felucca's state into ChoralRoot's slot refused: \"$(said_out)\"" || bad "Felucca's state: $code $(cat "$T/out")"
put "kind=state&slot=0&k=1&replace=1" "$D/s1.zip"
[ "$code" = 409 ] && said_out | grep -q "active state" && ok "a file over the active state without load=1 refused: \"$(said_out)\"" || bad "over the active: $code $(cat "$T/out")"
put "kind=project&k=4" "$D/s1.zip"
[ "$code" = 422 ] && said_out | grep -q "a state file" && ok "a state file as a project refused: \"$(said_out)\"" || bad "a state as a project: $code $(cat "$T/out")"
fc=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data-binary @"$D/s1.bin" "$U/api/fs/put?path=states/choralroot&name=state-09.bin")
[ "$fc" = 403 ] && [ ! -e "$CRS/state-09.bin" ] && ok "the file browser still leaves a running firmware's states/ alone (403)" || bad "fs/put into states/choralroot: $fc"
b0=$(field boot0)
put "kind=state&slot=0&k=1&replace=1&load=1" "$D/s1.zip"; v=$(vo2)
[ "$code" = 200 ] && [ "$(field boot0)" -gt "$b0" ] && [ "$v" = +3 ] && cmp -s "$D/s1.mem" "$CRS/state-01.mem" \
    && ok "...with load=1: the active state 2 replaced by the file and loaded (voicing $v)" || bad "replace and load: $code $(cat "$T/out"), voicing '$v'"

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
rm -f "/dev/shm/rfm14-sv-$$"
mv "$T" "$T.done" 2>/dev/null
echo "store_e2e: $checks checks, $fail failed"
exit $fail
