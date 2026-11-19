#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Core 1 for real firmwares (README "Threads", cpu2 without an import: FM1VST's shim, its docs/RFM1.md "cpu2"), in the
# build image, after make with FIZZ and ChoralRoot cpu2 builds among the inputs (make DEV_FIRMWARES=..: build/fw-dev,
# else build/fw: fizz-*.rfm1m, choralroot-*.rfm1m whose objects have rfm1m_cpu2): the host on the simulator, FIZZ in slot 1 and ChoralRoot in slot
# 2, a chord on FIZZ's third track (MIDI channel 3) and on ChoralRoot's channel; core 1 took jobs in both; each slot's
# blocks are the bytes of that firmware replayed offline from the host's log of its input with core 1 off (the module
# runs every job itself, as on a host without cpu2).
#   tests/cpu2fw_e2e.sh
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14c2.XXXXXX)
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "cpu2fw_e2e"
pick() { f=$(ls build/fw-dev/$1-*.rfm1m 2>/dev/null | sort -V | tail -1); [ -n "$f" ] || f=$(ls build/fw/$1-*.rfm1m 2>/dev/null | sort -V | tail -1); echo "$f"; }
FZ=$(pick fizz)
CR=$(pick choralroot)
for f in "$FZ" "$CR"; do
    if [ -z "$f" ] || ! nm -D "$f" | grep -q rfm1m_cpu2; then echo "  skip  no cpu2 build of FIZZ and ChoralRoot in build/fw"; exit 0; fi
done
FWS=$T/fws; mkdir -p "$FWS"
cp "${FZ%.rfm1m}".* "${CR%.rfm1m}".* build/fw/felucca-1.8.2.* "$FWS/"
PORT=17796; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":0}" "$U/api/load"; }
midi() { curl -s -o /dev/null -X POST --data "{\"track\":$1,\"b0\":$2,\"b1\":$3,\"b2\":$4}" "$U/api/test/midi"; }
R=$T/run; mkdir -p "$R/root" "$R/sim" "$R/cap"
echo "http://127.0.0.1:1" > "$R/root/catalog-url"
build/sim/remotefm1-host --root "$R/root" --port $PORT --sim-dir "$R/sim" --firmwares "$FWS" --site build/site \
    --fw fizz --test-api --no-fm1 --capture "$R/cap" --capture-seconds 60 >"$R/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$R/sim/mr-cmd" ] && [ "$(field session)" = 2 ]' || { bad "the host did not start"; tail -20 "$R/host.log"; exit 1; }
load 1 choralroot; wait_for 20 '[ "$(slotf 1 id)" = choralroot ]'
sleep 1
for n in 48 52 55 59 60 64 67 71; do midi 1 146 $n 100; midi 1 144 $n 100; sleep 0.1; done
sleep 4
STAT=$(status)
for n in 48 52 55 59 60 64 67 71; do midi 1 130 $n 0; midi 1 128 $n 0; done
sleep 0.6
printf 'quit\n' > "$R/sim/mr-cmd"; wait $HOST
for s in 0 1; do
    j=$(echo "$STAT" | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g' | sed -n "$((s + 1))p" | grep -o '"c1_jobs":[0-9]*' | cut -d: -f2)
    [ "${j:-0}" -gt 100 ] && ok "slot $((s + 1)): core 1 ran ${j} jobs" || bad "slot $((s + 1)): core 1 ran '${j:-}' jobs"
done
for s in 1 2; do
    log=$R/cap/inputs-slot$s.log; raw=$R/cap/raw-slot$s.raw
    case $s in 1) so=$FZ;; 2) so=$CR;; esac
    if [ ! -s "$raw" ]; then bad "slot $s: no capture"; continue; fi
    N=$(( $(stat -c %s "$raw") / 4 ))
    RFM14_CPU2=0 build/tools/fwrun "$so" --replay "$log" --frames $N --pcm "$T/rep$s.raw" >"$T/rep$s.txt" 2>&1
    cmp -s "$T/rep$s.raw" "$raw" \
        && ok "slot $s ($(basename "$so" .rfm1m)) with core 1 on its thread = replayed offline with core 1 off, bit for bit ($N frames)" \
        || bad "slot $s ($(basename "$so")): the replay differs ($(cmp "$T/rep$s.raw" "$raw" 2>&1 | head -1))"
done
mv "$T" "$T.done" 2>/dev/null
echo "cpu2fw_e2e: $checks checks, $fail failed"
exit $fail
