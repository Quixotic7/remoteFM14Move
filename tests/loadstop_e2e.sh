#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# A project or a state opened while the Move is stopped is silent and stopped (the owner's "Loading a new project is
# starting playback when the Move's playback is stopped"; engine.c slot_boot: after a working memory is restored, the
# Move stopped, the Stop every firmware gets as the sequencer stops, FC; README "The working memory"), on the
# simulator in the build image after make. ChoralRoot 0.43 in slot 1 with core 1 on, its loop PLAYED through its own
# takeover (Track 1 held + pad 2: a key, step 1 gets the chord, Play) and heard in the host's log (its "chord:" lines,
# a bar at 120 BPM: 2 s):
#   - while its loop plays: project 1 saved and duplicated into project 4, library state 2 saved (snapshots of a
#     running loop); the loop stopped (Play again), the takeover left (Track 1 held + pad 3: the sequencer);
#   - project 4 opened, stopped: restored with the loop (1 event) and stopped by the Stop ("loop: stop"), no chord for
#     5 s, the Move's sequencer stopped; project 1 again (its copy written stopped as it was left): Stop sent, silent;
#     state 2 (saved while playing; Track held + pad 7's path, from the page) loaded: stopped and silent;
#   - the Move playing (its Play: the loop follows its Start): state 2 loaded: no Stop sent, the loop
#     goes on with the Move;
#   - quit; project 1's copy replaced by the one saved while the loop played; started again: project 1 opened at start
#     from it, stopped and silent.
#   tests/loadstop_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "loadstop_e2e"
T=$(mktemp -d /tmp/rfm14ls.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/choralroot-0.43.* "$FWS/"
SIM=$T/sim; R=$T/root; CFG=$T/cfg; SAID=$T/said.log; KEEP=$T/keep; mkdir -p "$SIM" "$R" "$CFG" "$KEEP"
echo "http://127.0.0.1:1" > "$R/catalog-url"
echo '{ "debounce_ms": 0 }' > "$CFG/tts.json"
PORT=17831; U="http://127.0.0.1:$PORT"
P=$R/move/projects; L=$T/host.log
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
alive() { kill -0 "$HOST" 2>/dev/null; }
cmd() { alive || return 1; printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
store() { code=$(curl -s -o "$T/out" -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "$1" "$U/api/store"); }
tk() { [ "$(field move_mode)" = 4 ] && return 0; cmd "cc 43 127"; cmd "note 93 100"; cmd "note 93 0"; cmd "cc 43 0"; wait_for 3 '[ "$(field move_mode)" = 4 ]'; sleep 1.6; }
opened() { _p=$1; wait_for 20 '[ "$(field proj_cur)" = "$_p" ] && [ "$(field pfw_loading)" = 0 ]'; }
# the host's log since mark
mark() { LM=$(wc -l < "$L"); }
since() { tail -n +$((LM + 1)) "$L"; }
chords() { since | grep -c "^\[choralroot\] chord: C"; }
start() {
    FAKE_TTS_SHM=/rfm14-ls-$$ FAKE_TTS_LOG=$SAID build/sim/remotefm1-host --root "$R" --port $PORT \
        --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw choralroot --test-api --no-fm1 --screen-reader on \
        --tts-sidecar build/tools/fake-tts --tts-shm /rfm14-ls-$$ --tts-config "$CFG" >>"$L" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
        && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"'
}
stop() { printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST; }
# a load done while stopped ($1 what; $2 1: its snapshot held the loop running): restored, the Stop said in the log,
# the loop (1 event) stopped by it, no chord in 5 s (two bars and a half), the Move's sequencer stopped
silent() {
    wait_for 5 'since | grep -q "Stop sent"'
    [ "$2" = 1 ] && wait_for 5 'since | grep -q "loop: stop (state 1 layers 1 events 1"'
    sleep 5
    if since | grep -q "its saved working memory; the Move stopped: Stop sent" && [ "$(chords)" = 0 ] && [ "$(field seq_playing)" = 0 ] \
        && { [ "$2" = 0 ] || since | grep -q "loop: stop (state 1 layers 1 events 1"; }; then
        ok "$1 while stopped: its working memory restored, the Stop sent$([ "$2" = 1 ] && echo ", its running loop (1 event) stopped by it"), no chord in 5 s"
    else
        bad "$1: $(chords) chords in 5 s, seq_playing $(field seq_playing), $(since | grep -c 'Stop sent') Stop, $(since | grep 'loop:' | tail -1)"
    fi
}
start || { bad "the host did not start"; tail -20 "$L"; exit 1; }
wait_for 10 '[ "$(field snapable0)" = 1 ]' || bad "slot 1: snapable '$(field snapable0)'"
sleep 2
open0=$(field proj_cur)

# ---- the loop played in ChoralRoot's takeover; saved while it plays
tk; mark
cmd "note 68 100"; sleep 0.3; cmd "note 68 0"; sleep 0.3          # a key: the chord C
cmd "note 16 100"; sleep 0.1; cmd "note 16 0"; sleep 0.5          # step 1 gets it
press 85; sleep 4.5                                               # Play: the loop
[ "$(chords)" -ge 2 ] && since | grep -q "loop: play (state 2 layers 1 events 1" \
    && ok "ChoralRoot's loop playing (its takeover's Play): $(chords) chords in 4.5 s, the Move's sequencer with it ($(field seq_playing))" \
    || bad "the loop: $(chords) chords, $(since | grep 'loop:' | tail -1)"
store "{\"op\":\"project-save\",\"k\":$open0}"; c1=$code
for f in "$P"/move-project-0$((open0 + 1)).fw1.*; do cp "$f" "$KEEP/"; done
store "{\"op\":\"project-dup\",\"k\":$open0,\"to\":3}"; c2=$code
store '{"op":"state-save","slot":0,"k":1}'; c3=$code
[ "$c1$c2$c3" = 200200200 ] && [ -s "$P/move-project-04.fw1.mem" ] && [ -s "$R/states/choralroot/state-01.mem" ] \
    && ok "while it plays: project $((open0 + 1)) saved, duplicated into 4, library state 2 saved (snapshots of a running loop)" \
    || bad "saves while playing: $c1 $c2 $c3"
mark; press 85; sleep 1
since | grep -q "loop: stop" && [ "$(field seq_playing)" = 0 ] && ok "the loop stopped (Play), the Move stopped" || bad "stop: $(since | grep 'loop:' | tail -1), seq_playing $(field seq_playing)"
cmd "cc 43 127"; cmd "note 94 100"; cmd "note 94 0"; cmd "cc 43 0"   # Track 1 held + pad 3: the sequencer (the takeover left)
wait_for 4 '[ "$(field move_mode)" = 3 ]' || bad "the sequencer's mode: move_mode '$(field move_mode)'"

# ---- opened while stopped: silent and stopped (project 1's copy gets the memory as it is, stopped, before each load;
# a library state is never written by playing: what comes back running is project 4's copy and state 2)
mark; store '{"op":"project-open","k":3}'; opened 3; silent "project 4 opened" 1
mark; store "{\"op\":\"project-open\",\"k\":$open0}"; opened "$open0"; silent "project $((open0 + 1)) opened again" 0
mark; store '{"op":"state-load","slot":0,"k":1}'; silent "state 2 loaded" 1

# ---- the Move playing: nothing sent, the loop as it was saved goes on with the Move
press 85; wait_for 3 '[ "$(field seq_playing)" = 1 ]'; sleep 1
mark; store '{"op":"state-load","slot":0,"k":1}'; sleep 5
since | grep -q "slot 1: ChoralRoot 0.43 (its saved working memory)" && ! since | grep -q "Stop sent" && [ "$(chords)" -ge 2 ] \
    && ok "the Move playing (its loop with it): state 2 loaded with no Stop sent, its loop goes on ($(chords) chords in 5 s)" \
    || bad "the Move playing: $(since | grep -c 'Stop sent') Stop, $(chords) chords, seq_playing $(field seq_playing)"
press 85; sleep 1

# ---- a copy saved while playing, opened at start
stop
cp "$KEEP"/* "$P/"
mark
start || { bad "the host did not start again"; tail -20 "$L"; exit 1; }
opened "$open0"
since | grep -q "project $((open0 + 1)): slot 1: choralroot with the project's memory (its working memory restored)" \
    || bad "at start: project $((open0 + 1))'s copy not restored: $(since | grep 'project' | tail -3)"
silent "project $((open0 + 1)) opened at start from a copy saved while its loop played" 1
grep -q "trap" "$L" && bad "the host's log: $(grep -m1 'trap' "$L")"
stop
rm -f "/dev/shm/rfm14-ls-$$"
mv "$T" "$T.done" 2>/dev/null
echo "loadstop_e2e: $checks checks, $fail failed"
exit $fail
