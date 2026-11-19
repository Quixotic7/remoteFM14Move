#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Fantasy CFWs (FM1VST docs/VIRTUAL-CARTS.md) on Move Remote's simulator, end to end, in the build image: the page puts a
# WAV into the RemoteFM1 folder's audio/ (POST /api/files/audio/<name>; the name, WAV and size checks; the list; a
# delete); the Move's catalog (Mute) downloads Elasticat and Ripple from a local server (build/testcat-v: tools/
# build_move_catalog.py --only elasticat,ripple, signed with the test key); Elasticat in slot 1 reads that WAV through
# rfm1_host_1 (src/fsvc.c, the file thread), writes its own data/elasticat/, sounds from the pads; Ripple in slot 2
# lists its code/ripple/; both come back after a restart, Elasticat with what it wrote; a Move with too little memory
# refuses Elasticat (280 MB) instead of being killed.
#   tests/fantasy_e2e.sh      (after make and make testcat-v)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14fc.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17750; CATP=18090
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
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.15; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
LOG=$T/host.log
start() {   # start [VAR=value ...]
    env "$@" build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares "$T/bundled" \
        --site build/site --catalog-index build/testcat-v/index.json --fw felucca >>"$LOG" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' &&
        wait_for 5 '[ "$(field session)" = 2 ]'
}
stop() { cmd quit; wait $HOST; RC=$?; }
view() { cmd "cc 43 127"; sleep 0.15; press "note 16"; cmd "cc 43 0"; sleep 0.3; }   # Track 1 held + step 1
back() { press "cc 51"; press "cc 51"; press "cc 51"; press "cc 51"; }
load() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":$3}" "$U/api/load"; }
up() {   # up NAME FILE [curl args]: the HTTP code of the upload
    _n=$1; _f=$2; shift 2
    curl -s -o "$T/up.out" -w '%{http_code}' -X POST --data-binary "@$_f" -H 'Content-Type: audio/wav' "$@" "$U/api/files/audio/$_n"
}
list() { curl -s "$U/api/files/audio"; }
le32() { printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"; }
mkwav() {   # mkwav FILE FRAMES: 44.1 kHz 16-bit stereo, noise
    _d=$(($2 * 4))
    { printf 'RIFF'; le32 $((36 + _d)); printf 'WAVEfmt '; le32 16; printf '\001\000\002\000'; le32 44100; le32 176400
      printf '\004\000\020\000data'; le32 $_d; head -c $_d /dev/urandom; } > "$1"
}

echo "fantasy_e2e"
[ -f build/testcat-v/index.json.sig ] && ls build/testcat-v/elasticat-*.rfm1m build/testcat-v/ripple-*.rfm1m >/dev/null 2>&1 \
    || { echo "  FAIL  no test catalog of Fantasy CFWs in build/testcat-v (make testcat-v)"; exit 1; }
EC=$(cd build/testcat-v && ls elasticat-*.rfm1m); EC=${EC%.rfm1m}
for f in build/fw/felucca-1.8.2.*; do cp "$f" "$T/bundled/"; done
[ "$(grep -o '"virtual":true' build/testcat-v/index.json | wc -l)" = 2 ] && grep -q '"folders":{"audio":"r","data":"rw"}' build/testcat-v/index.json \
    && ok "index.json marks the Fantasy CFWs: virtual, needs fs1, their folders, memory" || bad "index.json has no virtual mark"
busybox httpd -f -p 127.0.0.1:$CATP -h build/testcat-v & S1=$!
echo "http://127.0.0.1:$CATP" > "$ROOT/catalog-url"
start || { bad "the host did not start"; tail -20 "$LOG"; kill $S1; exit 1; }
for d in audio code data; do [ -d "$ROOT/$d" ] || bad "no $d/ in the RemoteFM1 folder"; done
ok "the RemoteFM1 folder has audio/, code/, data/"

# ---- the page puts samples into audio/
mkwav "$T/loop.wav" 88200; mkwav "$T/two.wav" 4410
WAVSZ=$(wc -c < "$T/loop.wav" | tr -d ' ')
c=$(up "Amen%20loop%20(1).wav" "$T/loop.wav")
[ "$c" = 201 ] && cmp -s "$T/loop.wav" "$ROOT/audio/Amen loop (1).wav" && list | grep -q "\"name\":\"Amen loop (1).wav\",\"size\":$WAVSZ" \
    && ok "POST /api/files/audio/Amen%20loop%20(1).wav: 201, the bytes in audio/, listed ($WAVSZ bytes)" \
    || bad "the upload: $c $(cat "$T/up.out"); $(list | head -c 200)"
[ "$(up two.wav "$T/two.wav")" = 201 ] && [ "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$U/api/files/audio/two.wav")" = 200 ] \
    && [ ! -f "$ROOT/audio/two.wav" ] && ! list | grep -q two.wav && ok "DELETE /api/files/audio/two.wav: gone" || bad "the delete"
[ "$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$U/api/files/audio/nothing.wav")" = 404 ] && ok "DELETE a missing file: 404" || bad "delete missing"
head -c 5000 /dev/urandom > "$T/noise.bin"
c1=$(up noise.wav "$T/noise.bin"); c2=$(up "notes.txt" "$T/two.wav"); c3=$(up "%2E%2E%2Fescape.wav" "$T/two.wav")
c4=$(up ".hidden.wav" "$T/two.wav"); c5=$(up "a%2Fb.wav" "$T/two.wav")
[ "$c1" = 415 ] && [ "$c2" = 400 ] && [ "$c3" = 400 ] && [ "$c4" = 400 ] && [ "$c5" = 400 ] && [ ! -f "$ROOT/audio/noise.wav" ] \
    && [ ! -f "$ROOT/escape.wav" ] && [ -z "$(ls -A "$ROOT/audio" | grep -v '^Amen loop (1).wav$')" ] \
    && ok "refused, nothing left: not a WAV (415), .txt, ../escape.wav, .hidden.wav, a/b.wav (400)" \
    || bad "the checks: $c1 $c2 $c3 $c4 $c5; audio/: $(ls -A "$ROOT/audio" | tr '\n' ' ')"
c=$(curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Length: 268435460' --data-binary "@$T/two.wav" "$U/api/files/audio/big.wav")
[ "$c" = 413 ] && [ ! -f "$ROOT/audio/big.wav" ] && ok "over 256 MB: 413" || bad "the size cap: $c"
list | grep -q '"free":[1-9]' && ok "the list says the free space: $(list | grep -o '"free":[0-9]*')" || bad "no free space in the list"

# ---- the Move's catalog: Mute, Elasticat (the first pad, A-Z), jog press; then Ripple
view; press "cc 88"; press "note 92"; press "cc 3"
wait_for 20 '[ -f "$ROOT/firmwares/$EC.json" ]' && ok "Mute, Elasticat, jog press: downloaded and installed ($EC)" \
    || bad "Elasticat's download: state $(field dl_state), err '$(field dl_err)'"
sleep 2.2; press "note 94"; press "cc 3"   # (A-Z: Elasticat, Felucca, Ripple)
wait_for 20 'ls "$ROOT"/firmwares/ripple-*.json >/dev/null 2>&1' && ok "and Ripple" || bad "Ripple's download: '$(field dl_err)'"
back
curl -s "$U/api/firmwares" | tr -d '\n' | grep -q '"id":"elasticat"[^}]*"virtual":true' \
    && ok "/api/firmwares marks Elasticat virtual (the page's Fantasy CFW badge)" || bad "/api/firmwares: no virtual mark"

# ---- Elasticat in slot 1: it reads the WAV, writes its data/, sounds
[ "$(load 0 elasticat 1)" = 202 ] && wait_for 15 '[ "$(field id0)" = elasticat ]' && ok "Elasticat loads into slot 1" \
    || { bad "Elasticat does not load: '$(field id0)'"; grep -i "elasticat\|fwrt" "$LOG" | tail -5; }
wait_for 10 '[ "$(slotf 0 fs_read)" -ge "$WAVSZ" ]' \
    && ok "it read audio/'Amen loop (1).wav' through rfm1_host_1: $(slotf 0 fs_req) requests, $(slotf 0 fs_read) bytes read (the WAV: $WAVSZ)" \
    || bad "Elasticat's reads: $(slotf 0 fs_req) requests, $(slotf 0 fs_read) bytes, refused $(slotf 0 fs_refused)"
[ "$(slotf 0 mem_mb)" = 280 ] && ok "its memory: $(slotf 0 mem_mb) MB" || bad "memory: $(slotf 0 mem_mb)"
wait_for 10 '[ -s "$ROOT/data/elasticat/settings.bin" ] && [ "$(slotf 0 fs_written)" -gt 0 ]' \
    && ok "it wrote its own data/elasticat/ ($(ls "$ROOT/data/elasticat" | tr '\n' ' '); $(slotf 0 fs_written) bytes)" \
    || bad "data/elasticat: $(ls -R "$ROOT/data" | tr '\n' ' ')"
[ -z "$(ls "$ROOT/data" | grep -v '^elasticat$')" ] && [ ! -e "$ROOT/code/elasticat" ] \
    && ok "nothing outside its own data/ (no code/elasticat: its manifest lists audio r, data rw)" || bad "the folder: $(ls -R "$ROOT/data" "$ROOT/code" | tr '\n' ' ')"
sleep 1.2; p0=$(field peak)
build/tools/wsprobe $PORT 3500 --key 0 >/dev/null &   # (the page's panel: the FM-1's first key)
WP=$!
wait_for 4 '[ "$(field peak)" -gt 300 ]' && ok "the page's key 0: Elasticat plays the WAV (mix peak $(field peak), before $p0)" \
    || bad "no sound from Elasticat: peak $(field peak)"
sleep 1.2; info "CPU, Elasticat playing: $(slotf 0 cpu) % of a core, worst block $(slotf 0 worst_us) us"
wait $WP

# ---- Ripple in slot 2: its code/ripple/ (a droplet added there is listed by it)
mkdir -p "$ROOT/code/ripple/droplets"
printf 'function init() end\n' > "$ROOT/code/ripple/droplets/zz_test.lua"
[ "$(load 1 ripple 0)" = 202 ] && wait_for 15 '[ "$(field id1)" = ripple ]' && ok "Ripple loads into slot 2" || bad "Ripple: '$(field id1)'"
wait_for 6 '[ "$(slotf 1 fs_done)" -ge 2 ] && [ "$(slotf 1 fs_read)" -gt 0 ]' && [ -d "$ROOT/data/ripple" ] \
    && ok "Ripple read its code/ripple/ (the droplet zz_test.lua: $(slotf 1 fs_req) requests, $(slotf 1 fs_read) bytes)" \
    || bad "Ripple's requests: $(slotf 1 fs_req), $(slotf 1 fs_read) bytes"
late0=$(field late); sleep 3; late1=$(field late)
[ "$late0" = "$late1" ] && ok "Elasticat and Ripple running together, 3 s: no late block" || bad "late blocks: $late0 -> $late1"
info "CPU: $(status | grep -o '"slots":\[[^]]*\]' | sed 's/"worst_us[^}]*//g')"

# ---- a restart: both back, Elasticat from what it wrote
sleep 2.6   # (the library saved)
stop
[ "$RC" = 0 ] && ok "the host left cleanly" || bad "exit $RC"
start
wait_for 10 '[ "$(field id0)" = elasticat ] && [ "$(field id1)" = ripple ]' && ok "restarted: slot 1 Elasticat, slot 2 Ripple (the library)" \
    || bad "restart: '$(field id0)' '$(field id1)'"
wait_for 10 '[ "$(slotf 0 fs_read)" -ge "$WAVSZ" ]' && grep -q . "$ROOT/data/elasticat/last.txt" \
    && ok "Elasticat read its data/ and the WAV again ($(slotf 0 fs_read) bytes; last.txt: $(head -c 60 "$ROOT/data/elasticat/last.txt" | tr '\n' ' '))" \
    || bad "after the restart: $(slotf 0 fs_read) bytes"
stop

# ---- too little memory: refused, not killed
start RFM14_MEM_AVAIL_MB=300
wait_for 6 'grep -q "elasticat needs 28[0-9] MB of memory" "$LOG"' && [ "$(field id0)" != elasticat ] && [ "$(field id1)" = ripple ] \
    && ok "a Move with 300 MB available: Elasticat refused ($(grep -o 'elasticat needs [^)]*free' "$LOG" | tail -1)), Ripple (16 MB) loads" \
    || bad "the memory check: slot 1 '$(field id0)'; $(grep -i 'needs' "$LOG" | tail -2)"
stop
kill $S1 2>/dev/null
echo "fantasy_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
