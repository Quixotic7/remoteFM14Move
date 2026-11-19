#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Audio input for Fantasy CFWs (ain1: FM1VST docs/VIRTUAL-CARTS.md section 8), in the build image, with Elasticat
# (build/testcat-v, make testcat-v): its manifest wants ain1 and it exports rfm1_audio_in.
#   - offline: tools/fwrun --audio-in against FM1VST's rfm1_run --audio-in (wasm3): the same PCM, byte for byte, the
#     same ui_info (ain:1 lat:N), the same blocks fed; a REC take (FILE x5 = the SAMPLER, REC held, OCT+ arms, PLAY)
#     written by both is the same WAV, and it is the input sample for sample;
#   - on the simulator (--sim-audio-in: Move Remote's input pcm from a WAV): Elasticat in slot 1 gets the Move's input
#     (status: ain 1, the latency the host computes, blocks fed, the input's peak), its REC take from the page's
#     panel lands in data/elasticat/rec/ and is the input sample for sample; Ripple (no ain1) beside it gets none and
#     runs; no late block; --no-audio-in: Elasticat runs without input (ain 0, nothing fed).
#   tests/ain_e2e.sh      (after make and make testcat-v)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14ain.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17760
mkdir -p "$SIM" "$ROOT" "$T/bundled"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slotf() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n "$(($1 + 1))p" \
          | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
start() {
    build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares "$T/bundled" --site build/site \
        --catalog-url http://127.0.0.1:1 --fw elasticat "$@" >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' &&
        wait_for 5 '[ "$(field session)" = 2 ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
# the active firmware's matrix button for its label index (FM1P_B_*: SAVE 7, PLAY 10, REC 11, OCT+ 13), from /api/info
btn_id() { curl -s "$U/api/info" | grep -o '"buttons":\[\[.*\]\]' | grep -o ',[0-9]*\]' | sed -n "$(($1 + 1))p" | tr -d ',]'; }
hold() { build/tools/wsprobe $PORT $(($2 + 600)) --btn "$1" >/dev/null; sleep 0.4; }   # the page's panel: held $2 ms

echo "ain_e2e"
EC=$(cd build/testcat-v 2>/dev/null && ls elasticat-*.rfm1m 2>/dev/null); EC=${EC%.rfm1m}
RFM1=$(ls build/catalog-cache/rfm1/$EC.rfm1 2>/dev/null)
[ -n "$EC" ] && [ -n "$RFM1" ] || { echo "  FAIL  no Elasticat in build/testcat-v (make testcat-v)"; exit 1; }
build/tools/ainchk make "$T/in.wav" || { echo "  FAIL  ainchk make"; exit 1; }

# ---- offline: the host's runtime against wasm3
for d in a b c; do mkdir -p "$T/$d/audio" "$T/$d/code" "$T/$d/data/elasticat"; done
REC="--press SAVE@1000 --press SAVE@2000 --press SAVE@3000 --press SAVE@4000 --press SAVE@5000 --press REC@6000+500 --press OCT+@7000 --press PLAY@7500"
ref=$(timeout 600 build/ref/rfm1_run --wav "$T/ref.wav" --folder "$T/a" --audio-in "$T/in.wav" --audio-in-latency 1152 \
      --settle 18000 --seconds 1 $REC --expect-ui "ain:1 lat:1152" --expect-ui "takes:1" "$RFM1" 2>"$T/ref.err" | head -1)
refrc=$?
out=$(build/tools/fwrun "build/testcat-v/$EC.rfm1m" --pcm "$T/fw.pcm" --folder "$T/b" --audio-in "$T/in.wav" \
      --audio-in-latency 1152 --settle 18000 --seconds 1 $REC --ui 2>"$T/fw.err")
a=$(tail -c +45 "$T/ref.wav" | md5sum | cut -d' ' -f1); b=$(md5sum < "$T/fw.pcm" | cut -d' ' -f1)
fedr=$(grep -o 'audio in: [0-9]* blocks fed' "$T/ref.err" | grep -o '[0-9]*'); fedf=$(echo "$out" | grep -o 'audio in: [0-9]* blocks fed' | grep -o '[0-9]*')
[ "$refrc" = 0 ] && [ "$a" = "$b" ] && [ -n "$fedr" ] && [ "$fedr" = "$fedf" ] \
    && ok "fwrun --audio-in = rfm1_run --audio-in (wasm3): the same PCM ($a), $fedf blocks fed by both" \
    || bad "offline: rfm1_run $refrc, PCM $a / $b, fed $fedr / $fedf; $(tail -2 "$T/ref.err")"
echo "$out" | grep -q "ain:1 lat:1152 rec:idle recpop:1 rsrc:in" && echo "$out" | grep -q "takes:1" \
    && ok "Elasticat's ui_info through the host's runtime: ain:1 lat:1152, its REC take done (takes:1)" \
    || bad "ui_info: $(echo "$out" | grep -o 'ain:[^ ]* lat:[^ ]* rec:[^ ]*.*takes:[0-9]*')"
TA=$T/a/data/elasticat/rec/rec-0001.wav; TB=$T/b/data/elasticat/rec/rec-0001.wav
if [ -f "$TB" ] && build/tools/ainchk same "$TA" "$TB" >/dev/null && r=$(build/tools/ainchk find "$TB" "$T/in.wav"); then
    ok "the take (data/elasticat/rec/rec-0001.wav) is wasm3's, and the input: ${r#ainchk: }"
else bad "the take: $(ls -R "$T/b/data" | tr '\n' ' '); $(build/tools/ainchk find "$TB" "$T/in.wav")"; fi
out=$(build/tools/fwrun "build/fw/felucca-1.8.2.rfm1m" --audio-in "$T/in.wav" 2>&1 >/dev/null)
echo "$out" | grep -q "ignored: the manifest has no ain1" && ok "Felucca (no ain1): --audio-in ignored, as rfm1_run does" \
    || bad "Felucca with --audio-in: $out"

# ---- on the simulator: the Move's input
for f in build/testcat-v/$EC.* build/testcat-v/ripple-*.* build/fw/felucca-1.8.2.*; do cp "$f" "$T/bundled/"; done
start --sim-audio-in "$T/in.wav" || { bad "the host did not start"; tail -20 "$LOG"; exit 1; }
LAT=$(field ain_latency)
wait_for 15 '[ "$(slotf 0 id)" = elasticat ] && [ "$(slotf 0 ain)" = 1 ] && [ "$(slotf 0 ain_blocks)" -gt 100 ]' \
    && [ "$(slotf 0 ain_lat)" = "$LAT" ] && [ "$LAT" -gt 0 ] \
    && ok "Elasticat in slot 1 takes the Move's input: rfm1_audio_in_info(1, $LAT) ($(echo "$LAT" | awk '{printf "%.1f", $1 / 44.1}') ms round trip), $(slotf 0 ain_blocks) blocks fed" \
    || bad "Elasticat's input: id '$(slotf 0 id)', ain '$(slotf 0 ain)', lat '$(slotf 0 ain_lat)' / '$LAT', blocks '$(slotf 0 ain_blocks)'; $(grep -i 'audio in\|input' "$LOG" | tail -2)"
pk=0; for i in 1 2 3 4 5 6 7 8; do p=$(slotf 0 ain_peak); [ "${p:-0}" -gt $pk ] && pk=$p; sleep 0.2; done
[ $pk -ge 5000 ] && [ $pk -le 20000 ] && ok "the input blocks it is given are the WAV's (their peak $pk; the WAV's 8000 / 6000 tone, 20000 clicks)" \
    || bad "the input's peak: $pk"
SAVE=$(btn_id 7); PLAY=$(btn_id 10); RECB=$(btn_id 11); OCTUP=$(btn_id 13)
for i in 1 2 3 4 5; do hold "$SAVE" 100; sleep 0.5; done
hold "$RECB" 500; hold "$OCTUP" 100; hold "$PLAY" 100
wait_for 20 '[ -f "$ROOT/data/elasticat/rec/rec-0001.wav" ] && [ "$(slotf 0 fs_done)" = "$(slotf 0 fs_req)" ]'
sleep 1
if r=$(build/tools/ainchk find "$ROOT/data/elasticat/rec/rec-0001.wav" "$T/in.wav"); then
    ok "its REC take from the page's panel (FILE x5, REC held, OCT+, PLAY): data/elasticat/rec/rec-0001.wav, ${r#ainchk: }"
else bad "the take on the simulator: $r; $(ls -R "$ROOT/data/elasticat" | tr '\n' ' ')"; fi
info "the input path: under $(field ain_under) blocks, dropped $(field ain_dropped) frames, ring full $(field ain_over)"
wait_for 10 '[ "$(field late)" = 0 ] || true'
[ "$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"slot":1,"id":"ripple","activate":0}' "$U/api/load")" = 202 ] \
    && wait_for 15 '[ "$(slotf 1 id)" = ripple ]' && sleep 1.5 && ! status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g' | sed -n 2p | grep -q '"ain"' \
    && [ "$(slotf 1 dead)" = 0 ] && [ "$(slotf 0 ain)" = 1 ] \
    && ok "Ripple (no ain1) in slot 2: no input given, it runs; Elasticat still has its input" \
    || bad "Ripple beside: '$(slotf 1 id)' dead '$(slotf 1 dead)'; $(status | grep -o '"slots":\[[^]]*\]' | head -c 400)"
l0=$(field late); sleep 3; l1=$(field late)
[ "$l0" = "$l1" ] && ok "3 s with the input fed: no late block" || bad "late blocks: $l0 -> $l1"
stop
[ "$RC" = 0 ] && ok "the host left cleanly (the input off: rfm1_audio_in_info(0, 0) as Elasticat unloads)" || bad "exit $RC"
start --no-audio-in
wait_for 15 '[ "$(slotf 0 id)" = elasticat ]'; sleep 1.5
[ "$(slotf 0 ain)" = 0 ] && [ "$(slotf 0 ain_blocks)" = 0 ] && [ "$(field ain_live)" = 0 ] && [ "$(slotf 0 dead)" = 0 ] \
    && ok "--no-audio-in: Elasticat runs without input (ain 0, nothing fed)" \
    || bad "--no-audio-in: ain '$(slotf 0 ain)', blocks '$(slotf 0 ain_blocks)', live '$(field ain_live)'"
stop
echo "ain_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
