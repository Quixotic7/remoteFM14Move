#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The render time split on the simulator (README "Threads"), in the build image, after make: three firmwares from
# dist/move-catalog (default: FIZZ, ChoralRoot, SLOOP 3, as the owner's Move had them; BENCH_FWS="id id id"), a chord
# held on sequencer track 1 (all three hear it), the status read after SECONDS (default 8), once with --threads 0
# (every slot on the engine's thread, as before) and once with the slots on their threads. Prints the numbers only.
#   tests/threads_bench.sh [SECONDS]
set -u
cd "$(dirname "$0")/.."
SECS=${1:-8}
FWS_IDS=${BENCH_FWS:-"fizz choralroot sloop3"}
T=$(mktemp -d /tmp/rfm14bench.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
for id in $FWS_IDS; do
    f=$(ls dist/move-catalog/$id-[0-9]*.rfm1m 2>/dev/null | head -1)
    [ -n "$f" ] || { echo "threads_bench: no $id in dist/move-catalog"; exit 1; }
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
        --fw "$A" --test-api --no-fm1 "$@" >"$R/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$R/sim/mr-cmd" ] && [ "$(field session)" = 2 ]' || { echo "the host did not start"; tail "$R/host.log"; return; }
    load 1 "$B"; wait_for 20 '[ "$(slotf 1 id)" = "$B" ]'
    load 2 "$C"; wait_for 20 '[ "$(slotf 2 id)" = "$C" ]'
    sleep 1
    for n in 48 55 60 64 67 72; do midi 1 144 $n 100; done
    late0=$(field late)
    sleep "$SECS"
    st=$(status)
    late1=$(echo "$st" | grep -o '"late":[0-9]*' | head -1 | cut -d: -f2)
    echo "$name: late blocks in ${SECS}s: $((late1 - late0))"
    echo "  $(echo "$st" | grep -o '"block_avg_us[^}]*}')"
    for s in 0 1 2; do
        echo "  slot $((s + 1)): $(echo "$st" | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g' | sed -n "$((s + 1))p" \
            | grep -o '"id":"[^"]*"\|"cpu":[0-9.]*\|"render_avg_us":[0-9]*\|"render_max_us":[0-9]*\|"obj":"[^"]*"' | tr '\n' ' ')"
    done
    for n in 48 55 60 64 67 72; do midi 1 128 $n 0; done
    printf 'quit\n' > "$R/sim/mr-cmd"; wait $HOST
}
echo "threads_bench: $FWS_IDS, a 6-note chord on track 1, ${SECS}s ($(nproc) cores)"
one serial --threads 0
one threads
mv "$T" "$T.done" 2>/dev/null
