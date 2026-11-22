#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# A project holds its firmwares, and the firmwares' states (rfm1cs-abletonmove 0.13.0, RFM_MOVE_CAP_PROJFW), on the
# simulator in the build image after make. Felucca and ChoralRoot installed; a firmware's memory is changed through
# the test API (POST /api/test/poke: a byte near its end) and read back from the files the host writes:
#   - project 1: Felucca + ChoralRoot, its own copies (move-project-01.fw1/fw2.bin) follow the memory;
#   - an empty pad: a new project, its slots empty; Felucca loaded there and changed: project 2's copy differs;
#   - project 1 again: its loading screen comes and goes, its firmwares back, Felucca's memory project 1's;
#   - Copy held + project 1, then an empty pad: the copy carries the firmware copies;
#   - States (Track 1 held + pad 7): an empty pad saves, the active one follows the memory (autosave), a stored one
#     loads, Copy + a state then an empty one pastes, Delete cuts (pasted again), a paste onto the active one loads it,
#     step 14 New and Undo;
#   - the page's Files: the copies are in the projects' zip, the open project's are not deleted;
#   - restarted with ChoralRoot removed: project 1's slot 2 missing, the project keeps its name and its copy; an old
#     (version 8) project leaves the firmwares alone and becomes version 9, with copies, at its next save; the open
#     project cut: its copies into the buffer, pasted onto an empty pad with the project.
#   tests/projfw_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "projfw_e2e"
T=$(mktemp -d /tmp/rfm14pfw.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17826; U="http://127.0.0.1:$PORT"
P=$R/move/projects
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
load() { curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "{\"slot\":$1,\"id\":\"$2\"}" "$U/api/load"; }
poke() { curl -s -o /dev/null -X POST -H 'Content-Type: application/json' --data "{\"slot\":$1,\"off\":$X,\"val\":$2}" "$U/api/test/poke"; }
byte() { [ -f "$1" ] && od -An -tx1 -j "$X" -N1 "$1" | tr -d ' \n'; }          # the poked byte of a file
pcopy() { printf '%s/move-project-%02d.fw%d.bin' "$P" "$1" "$2"; }              # project n's copy of slot k (1-based)
st() { printf '%s/states/felucca/state-%02d.bin' "$R" "$1"; }
projects() { [ "$(field projects)" = 1 ] || { cmd "cc 40 127"; sleep 0.3; hit 16; cmd "cc 40 0"; wait_for 3 '[ "$(field projects)" = 1 ]'; }; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 >>"$T/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"'
}
stop() { printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST; }
start || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
sleep 2.5

# ---- project 1: Felucca (the default) in slot 1, ChoralRoot loaded into slot 2, a step (Track 1 held + pad 3: the
# sequencer; step 1): the project is stored, with its own copies
[ "$(load 1 choralroot)" = 202 ] && wait_for 10 '[ "$(field id1)" = choralroot ]' || bad "ChoralRoot into slot 2: '$(field id1)'"
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"; sleep 1.3; hit 16
wait_for 10 '[ -s "$P/move-project-01.rfmmove" ] && [ -s "$(pcopy 1 1)" ] && [ -s "$(pcopy 1 2)" ] && [ ! -e "$(pcopy 1 3)" ]' \
    && grep -q '"id":"felucca"' "$P/move-project-01.fw1.json" && grep -q '"id":"choralroot"' "$P/move-project-01.fw2.json" \
    && grep -q '"hash":"' "$P/move-project-01.fw1.json" && [ "$(field pcproj0)" = 0 ] \
    && ok "project 1 (Felucca, ChoralRoot): its copies move-project-01.fw1/fw2.bin, each .json with its id and hash" \
    || bad "project 1's copies: $(ls "$P")"
X=$(( $(stat -c %s "$(pcopy 1 1)") - 7 ))
poke 0 90
wait_for 8 '[ "$(byte "$(pcopy 1 1)")" = 5a ]' && ok "Felucca's memory changed (byte $X = 5a): project 1's copy follows it" \
    || bad "project 1's copy: byte '$(byte "$(pcopy 1 1)")'"

# ---- an empty pad (project 2): a new project, its three slots empty; Felucca there, changed: project 2's own
projects
hit 93
wait_for 8 '[ "$(field proj_open)" = 1 ] && [ -z "$(field id0)" ] && [ -z "$(field id1)" ] && [ -z "$(field id2)" ]' \
    && [ "$(field proj_cur)" = 1 ] && ok "an empty pad: project 2, a new one: its three slots empty" \
    || bad "the new project: proj_open '$(field proj_open)', slots '$(field id0)' '$(field id1)' '$(field id2)'"
[ "$(byte "$(pcopy 1 1)")" = 5a ] && [ -s "$(pcopy 1 2)" ] && [ ! -e "$(pcopy 2 1)" ] \
    && ok "project 1's copies kept as they were left; project 2 has none" || bad "the copies after the switch: $(ls "$P")"
[ "$(load 0 felucca)" = 202 ] && wait_for 10 '[ "$(field id0)" = felucca ]' || bad "Felucca into project 2's slot 1"
poke 0 165
wait_for 8 '[ "$(byte "$(pcopy 2 1)")" = a5 ]' && [ "$(byte "$(pcopy 1 1)")" = 5a ] && [ ! -e "$(pcopy 2 2)" ] \
    && ok "project 2: Felucca changed (a5): its own copy; project 1's still 5a" \
    || bad "project 2's copy '$(byte "$(pcopy 2 1)")', project 1's '$(byte "$(pcopy 1 1)")'"
[ "$(load 2 choralroot)" = 202 ] && wait_for 10 '[ "$(field id2)" = choralroot ] && [ -s "$(pcopy 2 3)" ]' || bad "ChoralRoot into slot 3: no copy"
curl -s -o /dev/null -X POST -H 'Content-Type: application/json' --data '{"op":17,"a":2,"b":0}' "$U/api/edit"
wait_for 10 '[ -z "$(field id2)" ] && [ ! -e "$(pcopy 2 3)" ] && [ ! -e "$P/move-project-02.fw3.json" ]' \
    && ok "ChoralRoot into project 2's slot 3: its copy; the slot emptied: at the project's save the copy is gone" \
    || bad "the emptied slot's copy: slot 3 '$(field id2)', $(ls "$P" | grep 02)"
FL=$R/states/felucca/flash.bin
wait_for 5 '[ "$(byte "$FL")" = a5 ]' || bad "Felucca's flash.bin (its last memory anywhere): '$(byte "$FL")'"

# ---- project 1 again: the loading screen comes and goes; Felucca's memory swapped for project 1's, ChoralRoot back
s0=$(field pfw_shown); e0=$(field pfw_ended); b0=$(field boot0)
projects
hit 92
wait_for 10 '[ "$(field pfw_shown)" -gt "$s0" ] && [ "$(field pfw_ended)" -gt "$e0" ]' \
    && ok "project 1's pad: its loading screen came up and went ($s0 -> $(field pfw_shown))" \
    || bad "the loading screen: shown '$(field pfw_shown)' ended '$(field pfw_ended)' (was $s0 $e0)"
wait_for 10 '[ "$(field id0)" = felucca ] && [ "$(field id1)" = choralroot ] && [ "$(field proj_open)" = 0 ]' \
    && [ "$(field boot0)" -gt "$b0" ] && ok "project 1's firmwares back: Felucca (its memory swapped: boot $b0 -> $(field boot0)), ChoralRoot" \
    || bad "project 1: slots '$(field id0)' '$(field id1)'"
wait_for 8 '[ "$(byte "$FL")" = 5a ]' && [ "$(byte "$(pcopy 2 1)")" = a5 ] \
    && ok "Felucca holds project 1's memory again (5a: its flash.bin, written from it, was project 2's a5)" \
    || bad "Felucca's memory (flash.bin) '$(byte "$FL")', project 2's copy '$(byte "$(pcopy 2 1)")'"
grep -q "project 1: slot 1: felucca with the project's memory" "$T/host.log" && ok "the log: slot 1 from the project's memory" \
    || bad "the log: $(grep 'project 1: slot 1' "$T/host.log" | tail -1)"

# ---- Copy held + project 1, then an empty pad (project 3): a duplicate, with its firmwares' copies
cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.3; hit 94; sleep 0.2; cmd "cc 60 0"
wait_for 8 '[ -s "$P/move-project-03.rfmmove" ] && [ -s "$(pcopy 3 1)" ] && [ -s "$(pcopy 3 2)" ]' \
    && cmp -s "$(pcopy 1 1)" "$(pcopy 3 1)" && cmp -s "$(pcopy 1 2)" "$(pcopy 3 2)" \
    && cmp -s "$P/move-project-01.fw1.json" "$P/move-project-03.fw1.json" \
    && ok "Copy + project 1, then an empty pad: project 3, with the same firmware copies (and .json)" \
    || bad "the duplicate: $(ls "$P")"
[ "$(field proj_open)" = 0 ] && [ "$(field id1)" = choralroot ] && ok "the copy left project 1 open as it was" || bad "after the copy: proj_open '$(field proj_open)'"
press 51; sleep 0.3

# ---- States: Track 1 held + pad 7
cmd "cc 43 127"; sleep 0.3; hit 98; cmd "cc 43 0"
wait_for 3 '[ "$(field states_open)" = 0 ]' && ok "Track 1 held + pad 7: Felucca's States" || bad "States: states_open '$(field states_open)'"
sleep 1.2
hit 92
wait_for 5 '[ -s "$(st 0)" ] && [ "$(field stcur0)" = 0 ] && [ "$(field cstcur0)" = 0 ]' && [ "$(byte "$(st 0)")" = 5a ] \
    && grep -q '"hash":"' "$R/states/felucca/state-00.json" \
    && ok "an empty pad: state 1 saved (state-00.bin, its hash), the active one" || bad "state 1: stcur '$(field stcur0)' '$(field cstcur0)'"
poke 0 51
wait_for 8 '[ "$(byte "$(st 0)")" = 33 ]' && ok "the memory changed (33): the active state 1 follows it (autosave)" \
    || bad "autosave: state 1 byte '$(byte "$(st 0)")'"
hit 93
wait_for 5 '[ -s "$(st 1)" ] && [ "$(field stcur0)" = 1 ]' || bad "state 2: stcur '$(field stcur0)'"
poke 0 68
wait_for 8 '[ "$(byte "$(st 1)")" = 44 ]' && [ "$(byte "$(st 0)")" = 33 ] \
    && ok "state 2 saved and active: it follows the memory (44), state 1 stays 33" \
    || bad "state 2 '$(byte "$(st 1)")', state 1 '$(byte "$(st 0)")'"
b0=$(field boot0)
hit 92
wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field stcur0)" = 0 ]' && wait_for 8 '[ "$(byte "$(pcopy 1 1)")" = 33 ]' \
    && [ "$(byte "$(st 1)")" = 44 ] \
    && ok "state 1 loaded (boot $b0 -> $(field boot0)): the memory 33 (the project's copy), state 2 kept its 44" \
    || bad "state 1 load: stcur '$(field stcur0)', copy '$(byte "$(pcopy 1 1)")', state 2 '$(byte "$(st 1)")'"
cmd "cc 60 127"; sleep 0.2; hit 92; sleep 0.3; hit 95; sleep 0.2; cmd "cc 60 0"
wait_for 5 '[ -s "$(st 3)" ]' && cmp -s "$(st 0)" "$(st 3)" && [ "$(field stcur0)" = 0 ] \
    && ok "Copy + state 1, then the empty state 4: pasted (the same bytes); state 1 still the active one" || bad "the state paste: $(ls "$R/states/felucca")"
cp "$(st 3)" "$T/state4"
cmd "cc 119 127"; sleep 0.2; hit 95; sleep 0.2; cmd "cc 119 0"
wait_for 5 '[ ! -e "$(st 3)" ]' && [ ! -e "$R/states/felucca/state-03.json" ] && ok "Delete + state 4: gone" || bad "the delete"
cmd "cc 60 127"; sleep 0.2; hit 96; sleep 0.2; cmd "cc 60 0"
wait_for 5 '[ -s "$(st 4)" ]' && cmp -s "$(st 4)" "$T/state4" && ok "Copy + the empty state 5: the deleted state pasted there" || bad "paste after a delete"
b0=$(field boot0)
cmd "cc 60 127"; sleep 0.2; hit 93; sleep 0.3; hit 92; sleep 0.2; cmd "cc 60 0"
wait_for 5 '[ "$(field boot0)" -gt "$b0" ]' && cmp -s "$(st 0)" "$(st 1)" && [ "$(field stcur0)" = 0 ] \
    && wait_for 8 '[ "$(byte "$(pcopy 1 1)")" = 44 ]' \
    && ok "Copy + state 2, then the active state 1: replaced and loaded (boot $b0 -> $(field boot0); the memory 44)" \
    || bad "a paste onto the active state: boot '$(field boot0)', copy '$(byte "$(pcopy 1 1)")'"
b0=$(field boot0)
hit 29
wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field stcur0)" = -1 ] && [ "$(field cstcur0)" = -1 ] && [ "$(field undo0)" = 1 ]' \
    && wait_for 8 '[ "$(byte "$(pcopy 1 1)")" != 44 ]' && [ "$(byte "$(st 0)")" = 44 ] \
    && ok "step 14 New: Felucca as first installed (copy byte '$(byte "$(pcopy 1 1)")'), no state active, state 1 untouched" \
    || bad "New: stcur '$(field stcur0)' '$(field cstcur0)', undo '$(field undo0)', copy '$(byte "$(pcopy 1 1)")'"
b0=$(field boot0)
press 56
wait_for 5 '[ "$(field boot0)" -gt "$b0" ] && [ "$(field stcur0)" = 0 ] && [ "$(field cstcur0)" = 0 ] && [ "$(field undo0)" = 0 ]' \
    && wait_for 8 '[ "$(byte "$(pcopy 1 1)")" = 44 ]' \
    && ok "Undo: the memory before New back (44), state 1 the active one again" \
    || bad "Undo: stcur '$(field stcur0)' '$(field cstcur0)', undo '$(field undo0)', copy '$(byte "$(pcopy 1 1)")'"
press 51; sleep 0.3; press 51; sleep 0.3

# ---- the page's Files: the copies are in move/projects' zip (not the buffer's hidden ones); the open project's stay
curl -s -o "$T/proj.zip" "$U/api/fs/zip?path=move/projects"
zl=$(cd "$T" && busybox unzip -l proj.zip 2>/dev/null)
echo "$zl" | grep -q "move-project-01.fw1.bin" && echo "$zl" | grep -q "move-project-01.fw2.json" \
    && echo "$zl" | grep -q "move-project-03.fw2.bin" && ! echo "$zl" | grep -q "clip" \
    && ok "Files: the projects' zip holds the firmware copies (not the buffer's)" || bad "the zip: $zl"
c=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$U/api/fs/delete?path=move/projects/move-project-01.fw1.bin")
[ "$c" = 403 ] && [ -s "$(pcopy 1 1)" ] && ok "Files: the open project's copy is not deleted (403)" || bad "delete of the open project's copy: $c"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
[ "$(byte "$(pcopy 1 1)")" = 44 ] || bad "at exit: project 1's copy '$(byte "$(pcopy 1 1)")'"

# ---- an old project (version 8): project 3's file made version 8 as project 5 (its firmware part taken out, its
# check again); ChoralRoot removed from the firmwares
v8() {   # $1 a version 9 .rfmmove -> $2 the same project as version 8
    L=$(od -An -tu4 -j14 -N4 "$1" | tr -d ' ')
    tail -c +19 "$1" | head -c "$L" > "$T/b9"
    { head -c 4 "$T/b9"; printf '\010'; tail -c +6 "$T/b9" | head -c $((L - 4 - 176 - 5)); } > "$T/b8"
    h=2166136261
    for b in $(od -An -tu1 -v "$T/b8"); do h=$(( ((h ^ b) * 16777619) & 4294967295 )); done
    le() { printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"; }
    { head -c 8 "$1"; printf '\004\000'; tail -c +11 "$1" | head -c 4; le $((L - 176)); cat "$T/b8"; le $h; } > "$2"
}
v8 "$P/move-project-03.rfmmove" "$P/move-project-05.rfmmove"
[ "$(od -An -tu1 -j22 -N1 "$P/move-project-05.rfmmove" | tr -d ' ')" = 8 ] || bad "the version 8 fixture"
mv "$R/firmwares/choralroot-0.43.json" "$T/"; mv "$FWS/choralroot-0.43.json" "$T/cr-bundled.json"
sha0=$(cat "$R"/states/felucca/state-*.bin | sha256sum)
start || { bad "the host did not start again"; tail -20 "$T/host.log"; exit 1; }
wait_for 10 '[ "$(field id0)" = felucca ] && [ "$(field want1)" = choralroot ]' && [ -z "$(field id1)" ] && [ -s "$(pcopy 1 2)" ] \
    && grep -q "project 1: choralroot is not installed" "$T/host.log" \
    && ok "restarted, ChoralRoot removed: project 1's Felucca back, slot 2 missing (said), the project still wants ChoralRoot, its copy kept" \
    || bad "the missing firmware: slots '$(field id0)' '$(field id1)', want1 '$(field want1)'"
sleep 2.5
b0=$(field boot0)
projects
hit 96
wait_for 8 '[ "$(field proj_cur)" = 4 ] && [ "$(field proj_open)" = 4 ]' && sleep 1.5 \
    && [ "$(field boot0)" = "$b0" ] && [ "$(field id0)" = felucca ] && [ -z "$(field id1)" ] \
    && ok "project 5 (version 8): opened, the firmwares left as they were (Felucca, boot $b0)" \
    || bad "the version 8 project: proj '$(field proj_cur)', boot $b0 -> '$(field boot0)', slots '$(field id0)' '$(field id1)'"
hit 93
wait_for 8 '[ "$(field proj_open)" = 1 ]' && wait_for 8 '[ "$(od -An -tu1 -j22 -N1 "$P/move-project-05.rfmmove" | tr -d " ")" = 9 ] && [ -s "$(pcopy 5 1)" ]' \
    && [ ! -e "$(pcopy 5 2)" ] && grep -q '"id":"felucca"' "$P/move-project-05.fw1.json" \
    && ok "saved as it was left: project 5 is version 9 now, with its copy of Felucca" \
    || bad "project 5 after its save: version '$(od -An -tu1 -j22 -N1 "$P/move-project-05.rfmmove")', $(ls "$P")"
[ -s "$(pcopy 1 2)" ] && grep -q '"id":"choralroot"' "$P/move-project-01.fw2.json" \
    && ok "project 1 saved since: its copy of the missing ChoralRoot still there" || bad "project 1's ChoralRoot copy"
# ---- the open project cut (Delete + its pad): its copies go into the buffer (the core leaves an empty project in its
# slot with the firmwares as they are, unsaved); then pasted onto an empty pad
wait_for 8 '[ "$(field id0)" = felucca ] && [ "$(field pfw_loading)" = 0 ]' && sleep 2.5
projects
cmd "cc 119 127"; sleep 0.2; hit 93; sleep 0.3; cmd "cc 119 0"
wait_for 8 '[ -s "$P/.clip.fw1.bin" ] && [ ! -e "$P/move-project-02.rfmmove" ] && [ ! -e "$(pcopy 2 1)" ]' \
    && grep -q '"id":"felucca"' "$P/.clip.fw1.json" && [ "$(field id0)" = felucca ] \
    && ok "project 2 (open) cut: its copy moved into the buffer (.clip.fw1.bin, hidden); Felucca stays loaded" \
    || bad "the cut: slot 1 '$(field id0)', $(ls -a "$P" | tr '\n' ' ')"
cmd "cc 119 127"; sleep 0.2; hit 97; sleep 0.3; cmd "cc 119 0"
wait_for 8 '[ -s "$P/move-project-06.rfmmove" ] && [ -s "$(pcopy 6 1)" ]' && cmp -s "$P/.clip.fw1.bin" "$(pcopy 6 1)" \
    && ok "Delete + an empty pad: pasted as project 6, with its copy" || bad "the paste: $(ls "$P" | tr '\n' ' ')"
[ "$(cat "$R"/states/felucca/state-*.bin | sha256sum)" = "$sha0" ] && ok "states/: Felucca's states as they were" || bad "states/ changed"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
stop
mv "$T" "$T.done" 2>/dev/null
echo "projfw_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T.done/host.log"; }
exit $fail
