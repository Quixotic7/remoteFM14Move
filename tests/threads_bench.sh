#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The render time split on the simulator (README "Threads"), in the build image, after make: three firmwares from
# dist/move-catalog (default: FIZZ, ChoralRoot, SLOOP 3, as the owner's Move had them; BENCH_FWS="id id id"), a chord
# held on sequencer track 1 (all three hear it), the status read after SECONDS (default 8), once with --threads 0
# (every slot on the engine's thread, as before) and once with the slots on their threads, then with the slots on their
# threads and core 1 off (--no-cpu2: a cpu2 module runs its core-1 jobs itself). Prints the numbers only.
# BENCH_ARGS (more host options for every run: "--frame-ms 15,15", the ABI's screen rate), BENCH_CH (the chord's channel, 0-based: FIZZ's tracks have one each), BENCH_NOTES ("48 55 ..": the chord)
# BENCH_DIRS="dir.." : where the objects are looked for, in order (default dist/move-catalog; e.g. "build/fw
# dist/move-catalog" for this build's cpu2 objects first)
#   tests/threads_bench.sh [SECONDS]
set -u
cd "$(dirname "$0")/.."
SECS=${1:-8}
CH=${BENCH_CH:-0}                     # the chord's MIDI channel, 0-based (FIZZ: a channel per track)
NOTES=${BENCH_NOTES:-"48 55 60 64 67 72"}
FWS_IDS=${BENCH_FWS:-"fizz choralroot sloop3"}
T=$(mktemp -d /tmp/rfm14bench.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
for id in $FWS_IDS; do
    f=""
    for d in ${BENCH_DIRS:-dist/move-catalog}; do
        [ -n "$f" ] || f=$(ls "$d"/$id-[0-9]*.rfm1m 2>/dev/null | sort -V | tail -1)
    done
    [ -n "$f" ] || { echo "threads_bench: no $id in ${BENCH_DIRS:-dist/move-catalog}"; exit 1; }
    cp "${f%.rfm1m}".* "$FWS/"
done
set -- $FWS_IDS
A=$1; B=$2; C=$3
PORT=17795; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":0}" "$U/api/load"; }
midi() { curl -s -o /dev/null -X POST --data "{\"track\":$1,\"b0\":$2,\"b1\":$3,\"b2\":$4}" "$U/api/test/midi"; }
one() {
    name=$1; shift
    R=$T/$name; mkdir -p "$R/root" "$R/sim"
    echo "http://127.0.0.1:1" > "$R/root/catalog-url"
    build/sim/remotefm1-host --root "$R/root" --port $PORT --sim-dir "$R/sim" --firmwares "$FWS" --site build/site \
        --fw "$A" --test-api --no-fm1 ${BENCH_ARGS:-} "$@" >"$R/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$R/sim/mr-cmd" ] && [ "$(field session)" = 2 ]' || { echo "the host did not start"; tail "$R/host.log"; return; }
    load 1 "$B"; wait_for 20 '[ "$(slotf 1 id)" = "$B" ]'
    load 2 "$C"; wait_for 20 '[ "$(slotf 2 id)" = "$C" ]'
    sleep 1
    for n in $NOTES; do midi 1 $((144 + CH)) $n 100; done
    late0=$(field late)
    sleep "$SECS"
    st=$(status)
    late1=$(echo "$st" | grep -o '"late":[0-9]*' | head -1 | cut -d: -f2)
    echo "$name: late blocks in ${SECS}s: $((late1 - late0))"
    echo "  $(echo "$st" | grep -o '"block_avg_us[^}]*}')"
    for s in 0 1 2; do
        echo "  slot $((s + 1)): $(echo "$st" | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g' | sed -n "$((s + 1))p" \
            | grep -o '"id":"[^"]*"\|"version":"[^"]*"\|"cpu":[0-9.]*\|"render_avg_us":[0-9]*\|"render_max_us":[0-9]*\|"obj":"[^"]*"\|"c1":[0-9]*\|"c1_jobs":[0-9]*\|"c1_inline":[0-9]*\|"c1_cpu":[0-9.]*\|"c1_worst_us":[0-9]*\|"split":{[^}]*}' | tr '\n' ' ')"
    done
    for n in $NOTES; do midi 1 $((128 + CH)) $n 0; done
    printf 'quit\n' > "$R/sim/mr-cmd"; wait $HOST
}
echo "threads_bench: $FWS_IDS, a 6-note chord on track 1, ${SECS}s ($(nproc) cores)"
one serial --threads 0
one threads
one threads-no-cpu2 --no-cpu2
mv "$T" "$T.done" 2>/dev/null
