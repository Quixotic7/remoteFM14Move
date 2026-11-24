#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# A new random project (rfm1cs 0.14.0's step 13 on the Projects page, the owner's "create a new project with 3 random
# firmwares (random from the ones installed) and then a random scale and root note ... in a empty slot"), on the
# simulator in the build image after make, through the simulator's command pipe. The core sends PROJECT_SAVE (the
# open one), PROJECT_FW x 3 for the NEW slot (no PROJECT_LOAD, no copies there yet), PROJECT_SAVE (the new one); the
# host loads each named firmware as first installed (not another project's copy, not its last flash.bin):
#   - two installed (Felucca, ChoralRoot): project 1 Felucca, its memory changed (poke, 5a); step 13: project 2 with
#     both, fresh (the log), its third slot empty; project 1 untouched (its copy 5a);
#   - Undo: project 2 deleted (its blob and copies gone, none written again), project 1 back with its memory (5a);
#   - three installed (tkdemo too, restarted): step 13: project 2 with the three, each fresh; quit and start again:
#     project 2 opens with its three firmwares from its own copies.
#   tests/projnew_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "projnew_e2e"
T=$(mktemp -d /tmp/rfm14pn.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17833; U="http://127.0.0.1:$PORT"
P=$R/move/projects; L=$T/host.log
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
poke() { curl -s -o /dev/null -X POST -H 'Content-Type: application/json' --data "{\"slot\":$1,\"off\":$X,\"val\":$2}" "$U/api/test/poke"; }
byte() { [ -f "$1" ] && od -An -tx1 -j "$X" -N1 "$1" | tr -d ' \n'; }
pcopy() { printf '%s/move-project-%02d.fw%d.bin' "$P" "$1" "$2"; }
projects() { [ "$(field projects)" = 1 ] || { cmd "cc 40 127"; sleep 0.3; hit 16; cmd "cc 40 0"; wait_for 3 '[ "$(field projects)" = 1 ]'; }; }
ids() { echo "$(field id0) $(field id1) $(field id2)"; }
mark() { LM=$(wc -l < "$L"); }
since() { tail -n +$((LM + 1)) "$L"; }
start() {
    build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
        --test-api --no-fm1 >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"'
}
stop() { printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST; }
start || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5
late0=$(field late)

# ---- project 1: Felucca, stored (a step in the sequencer), its memory changed
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"; sleep 1.3; hit 16; sleep 0.3
wait_for 10 '[ -s "$P/move-project-01.rfmmove" ] && [ -s "$(pcopy 1 1)" ]' || bad "project 1 not stored: $(ls "$P")"
X=$(( $(stat -c %s "$(pcopy 1 1)") - 7 ))
poke 0 90
wait_for 8 '[ "$(byte "$(pcopy 1 1)")" = 5a ]' && wait_for 8 '[ "$(byte "$R/states/felucca/flash.bin")" = 5a ] || true' \
    && ok "project 1: Felucca, its memory changed (5a), its copy follows" || bad "project 1's copy: '$(byte "$(pcopy 1 1)")'"
sleep 1

# ---- step 13 on the Projects page, two installed: project 2 with both, fresh; the third slot empty
projects; mark
hit 28
wait_for 15 '[ "$(field proj_cur)" = 1 ] && [ "$(field pfw_loading)" = 0 ] && [ -n "$(field id0)$(field id1)$(field id2)" ]'; sleep 1
set -- $(ids)
n=$#
fresh=$(since | grep -c "project 2: slot [123]: [a-z]* with a fresh memory")
if [ "$n" = 2 ] && [ "$1" != "$2" ] && echo "$*" | grep -q felucca && echo "$*" | grep -q choralroot && [ "$fresh" = 2 ] \
    && [ -s "$P/move-project-02.rfmmove" ] && ! since | grep -q "project 2: slot .* (its last memory"; then
    ok "step 13: project 2 (the first empty slot) open with both installed firmwares ($*), each as first installed (the log), one slot empty, stored"
else
    bad "step 13 with two installed: slots '$(ids)', $fresh fresh, $(since | grep 'project 2: slot' | tr '\n' ' ')"
fi
[ "$(byte "$(pcopy 1 1)")" = 5a ] && ok "project 1's copy untouched (5a)" || bad "project 1's copy: '$(byte "$(pcopy 1 1)")'"
sleep 5   # (its copies written by the autosave)
fs=$(for k in 1 2 3; do f=$(pcopy 2 $k); [ -s "$f" ] && grep -q '"id":"felucca"' "${f%.bin}.json" && echo "$f"; done)
[ -n "$fs" ] && [ "$(byte "$fs")" != 5a ] && ok "project 2's Felucca copy is a fresh memory (byte '$(byte "$fs")', not flash.bin's 5a)" \
    || bad "project 2's Felucca: '$fs' byte '$( [ -n "$fs" ] && byte "$fs")'"

# ---- Undo: project 2 gone, project 1 back as it was
mark
press 56
wait_for 15 '[ "$(field proj_cur)" = 0 ] && [ "$(field pfw_loading)" = 0 ] && [ "$(field id0)" = felucca ]' && sleep 2
[ "$(field proj_cur)" = 0 ] && [ "$(ids)" = "felucca  " ] && [ ! -e "$P/move-project-02.rfmmove" ] \
    && [ -z "$(ls "$P" | grep 'move-project-02.fw')" ] && [ "$(byte "$(pcopy 1 1)")" = 5a ] \
    && since | grep -q "project 1: slot 1: felucca with the project's memory" \
    && ok "Undo: project 2 deleted with its copies (none written again), project 1 back with Felucca's own memory (5a)" \
    || bad "Undo: proj_cur '$(field proj_cur)', slots '$(ids)', $(ls "$P" | grep 02 | tr '\n' ' '), copy '$(byte "$(pcopy 1 1)")'"
press 51; sleep 0.3
grep -q "trap" "$L" && bad "the host's log: $(grep -m1 'trap' "$L")"
late1=$(field late)
stop

# ---- three installed: step 13 makes project 2 with the three, each fresh; it survives a restart
cp build/tkdemo/tkdemo-1.0.* "$FWS/"
start || { bad "the host did not start again"; tail -20 "$L"; exit 1; }
sleep 2.5
projects; mark
hit 28
wait_for 15 '[ "$(field proj_cur)" = 1 ] && [ "$(field pfw_loading)" = 0 ] && [ -n "$(field id2)" ]'; sleep 1
set -- $(ids)
fresh=$(since | grep -c "project 2: slot [123]: [a-z]* with a fresh memory")
[ $# = 3 ] && [ "$1" != "$2" ] && [ "$2" != "$3" ] && [ "$1" != "$3" ] && [ "$fresh" = 3 ] \
    && ok "three installed, step 13: project 2 with three different firmwares ($*), each as first installed" \
    || bad "step 13 with three installed: slots '$(ids)', $fresh fresh"
w="$*"
sleep 5; press 51; sleep 0.3
stop
mark
start || { bad "the host did not start a third time"; tail -20 "$L"; exit 1; }
wait_for 15 '[ "$(field proj_cur)" = 1 ] && [ "$(field pfw_loading)" = 0 ] && [ -n "$(field id2)" ]'; sleep 1
[ "$(ids)" = "$w" ] && [ "$(since | grep -c "project 2: slot [123]: [a-z]* with the project's memory")" = 3 ] \
    && ok "quit and started again: project 2 opens with its three firmwares ($w) from its own copies" \
    || bad "after a restart: slots '$(ids)' (want $w), $(since | grep 'project 2: slot' | tr '\n' ' ')"
[ "$late1" = "$late0" ] && [ "$(field late)" = 0 ] && ok "no late block" || bad "late blocks: $late0 -> $late1, then $(field late)"
grep -q "trap" "$L" && bad "the host's log: $(grep -m1 'trap' "$L")"
stop
mv "$T" "$T.done" 2>/dev/null
echo "projnew_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T.done/host.log"; }
exit $fail
