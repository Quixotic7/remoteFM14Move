#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The threads (README "Threads"), in the build image, after make:
#   - core 1 (cpu2): the test firmware (tests/cpu2fix) offline, its core-1 jobs on the core-1 thread, and the same PCM
#     with core 1 off (the module then runs every job itself);
#   - an object from before the threads (dist/move-catalog's FIZZ, when there): the same PCM with its own call depth
#     and under the one lock;
#   - the host on the simulator with three slots on their render threads (Felucca, the core-1 fixture, and FIZZ's old
#     object or ChoralRoot), sequencer MIDI to all three: each slot's blocks in the host are the same bytes as that
#     firmware replayed offline, alone on one thread, from the host's own log of its input (tools/fwrun --replay);
#     and the mix the same as what the audio thread played;
#   - the measurement: the same three slots with --threads 0 and 1, the render time split (the status' threads).
#   tests/threads_e2e.sh
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14thr.XXXXXX)
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "threads_e2e"
FIX=build/cpu2fix/cpu2fix-1.0.rfm1m

# ---- core 1, offline
a=$(build/tools/fwrun "$FIX" --pcm "$T/c1.pcm" --ui 2>&1)
b=$(RFM14_CPU2=0 build/tools/fwrun "$FIX" --pcm "$T/c0.pcm" --ui 2>&1)
j1=$(echo "$a" | sed -n 's/.*ui: core1 \([0-9]*\) self \([0-9]*\).*/\1 \2/p')
j0=$(echo "$b" | sed -n 's/.*ui: core1 \([0-9]*\) self \([0-9]*\).*/\1 \2/p')
if [ -s "$T/c1.pcm" ] && cmp -s "$T/c1.pcm" "$T/c0.pcm" && [ "${j1#* }" = 0 ] && [ "${j1% *}" -gt 1000 ] && [ "${j0% *}" = 0 ]; then
    ok "cpu2: the fixture's core-1 jobs on the core-1 thread (${j1% *} jobs) = core 1 off (${j0#* } run by core 0), the same PCM; $(echo "$a" | sed -n 's/.*\(, [0-9.]*x real time\).*/\1/p' | cut -c3-) vs $(echo "$b" | sed -n 's/.*\(, [0-9.]*x real time\).*/\1/p' | cut -c3-)"
else bad "cpu2: core 1 '$j1' / off '$j0'; $(cmp "$T/c1.pcm" "$T/c0.pcm" 2>&1)"; echo "$a"; echo "$b"; fi

# ---- an object from before the threads: its own call depth, or the lock: the same sound
OLD=$(ls dist/move-catalog/fizz-*.rfm1m 2>/dev/null | head -1)
if [ -n "$OLD" ] && ! nm -D "$OLD" | grep -q rfm1m_caps; then
    build/tools/fwrun "$OLD" --pcm "$T/o1.pcm" >/dev/null 2>"$T/o1.err"
    RFM14_LEGACY_LOCK=1 build/tools/fwrun "$OLD" --pcm "$T/o2.pcm" >/dev/null 2>&1
    if [ -s "$T/o1.pcm" ] && cmp -s "$T/o1.pcm" "$T/o2.pcm" && ! grep -q "not redirected" "$T/o1.err"; then
        ok "an old object ($(basename "$OLD")): its own call depth (GOT redirected) = the one lock, the same PCM"
    else bad "the old object: $(cat "$T/o1.err" | tail -2)"; fi
else echo "  skip  no old object in dist/move-catalog"; OLD=""; fi

# ---- the host: three slots on their threads, each the same bytes as its offline replay
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/* build/cpu2fix/*.rfm1m build/cpu2fix/*.json "$FWS/" 2>/dev/null
if [ -n "$OLD" ]; then cp "${OLD%.rfm1m}".* "$FWS/"; THIRD=$(basename "$OLD" .rfm1m); THIRD=${THIRD%%-*}; else THIRD=choralroot; fi
PORT=17790; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
load() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":0}" "$U/api/load"; }
midi() { curl -s -o /dev/null -X POST --data "{\"track\":$1,\"b0\":$2,\"b1\":$3,\"b2\":$4}" "$U/api/test/midi"; }
run_host() {   # run_host NAME [host options]: three slots, a phrase on track 1, the status' threads, quit
    name=$1; shift
    R=$T/$name; mkdir -p "$R/root" "$R/sim" "$R/cap"
    echo "http://127.0.0.1:1" > "$R/root/catalog-url"
    build/sim/remotefm1-host --root "$R/root" --port $PORT --sim-dir "$R/sim" --firmwares "$FWS" --site build/site \
        --fw felucca --test-api --no-fm1 --capture "$R/cap" --capture-seconds 60 "$@" >"$R/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$R/sim/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$R/sim/mr-state" 2>/dev/null' \
        && wait_for 5 '[ "$(field session)" = 2 ]' || { bad "$name: the host did not start"; tail -20 "$R/host.log"; return 1; }
    load 1 cpu2fix >/dev/null; wait_for 15 '[ "$(slotf 1 id)" = cpu2fix ]'
    load 2 "$THIRD" >/dev/null; wait_for 15 '[ "$(slotf 2 id)" = "$THIRD" ]'
    sleep 1
    for n in 60 64 67 72; do midi 1 144 $n 100; sleep 0.4; done
    sleep 2.2
    STAT=$(status)
    for n in 60 64 67 72; do midi 1 128 $n 0; done
    sleep 0.6
    printf 'quit\n' > "$R/sim/mr-cmd"; wait $HOST
}
run_host thr
echo "$STAT" > "$T/thr.status"
[ "$(echo "$STAT" | grep -o '"threads":[01]' )" = '"threads":1' ] && ok "the slots render on their own threads (status threads 1; $(echo "$STAT" | grep -o '"tcpu":{[^}]*}'))" \
    || bad "threads: $(echo "$STAT" | grep -o '"threads":[0-9]*')"
grep -q "rfm1-c1-2" "$T/thr/host.log" 2>/dev/null || echo "$STAT" | grep -q '"c1":1' \
    && ok "slot 2's core 1 working ($(echo "$STAT" | grep -o '"c1_jobs":[0-9]*' | head -1))" || bad "no core 1 for cpu2fix: $STAT"
for s in 1 2 3; do
    log=$T/thr/cap/inputs-slot$s.log; raw=$T/thr/cap/raw-slot$s.raw
    case $s in 1) so=build/fw/felucca-1.8.2.rfm1m;; 2) so=$FIX;; 3) so=$(ls "$FWS"/$THIRD-*.rfm1m | head -1);; esac
    if [ ! -s "$raw" ]; then bad "slot $s: no capture"; continue; fi
    N=$(( $(stat -c %s "$raw") / 4 ))
    RFM14_CPU2=0 build/tools/fwrun "$so" --replay "$log" --frames $N --pcm "$T/rep$s.raw" >"$T/rep$s.txt" 2>&1
    cmp -s "$T/rep$s.raw" "$raw" \
        && ok "slot $s ($(basename "$so" .rfm1m)) on its thread = replayed offline alone on one thread$( [ $s = 2 ] && echo ', core 1 off'), bit for bit ($N frames, $(( $(stat -c %s "$log") / 12 )) inputs)" \
        || bad "slot $s ($(basename "$so")): the replay differs ($(cmp "$T/rep$s.raw" "$raw" 2>&1 | head -1))"
done
osz=$(stat -c %s "$T/thr/cap/out.raw"); cmp -s -n "$osz" "$T/thr/cap/out.raw" "$T/thr/cap/mix.raw" \
    && ok "the audio thread played the engine's mix, sample for sample" || bad "out.raw is not mix.raw's start"
late=$(echo "$STAT" | grep -o '"late":[0-9]*' | head -1 | cut -d: -f2)
echo "  info  threads: $(echo "$STAT" | grep -o '"block_avg_us[^}]*"tcpu":{[^}]*}')"
echo "  info  slots:   $(echo "$STAT" | grep -o '"render_avg_us":[0-9]*,"render_max_us":[0-9]*' | tr '\n' ' ')"

# ---- the same on one thread (--threads 0), for the split
run_host ser --threads 0
echo "$STAT" > "$T/ser.status"
[ "$(echo "$STAT" | grep -o '"threads":[01]')" = '"threads":0' ] && ok "--threads 0: every slot on the engine's thread" || bad "--threads 0"
cmp -s "$T/ser/cap/raw-slot2.raw" "$T/thr/cap/raw-slot2.raw" 2>/dev/null; :
echo "  info  serial:  $(echo "$STAT" | grep -o '"block_avg_us[^}]*"tcpu":{[^}]*}')"
mv "$T" "$T.done" 2>/dev/null
echo "threads_e2e: $checks checks, $fail failed"
exit $fail
