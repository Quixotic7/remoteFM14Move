#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# remotefm1-host on Move Remote's simulator (plat_sim), end to end, in the build image: the Move's pads, buttons and
# jog through the simulator's test FIFO (mr-cmd), its state line (mr-state), its screen shot; the page on :7730.
#   tests/sim_e2e.sh            (from the repo, after make; uses build/sim, build/fw, build/tools)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14e2e.XXXXXX)
ROOT=$T/root; SIM=$T/sim; CAP=$T/cap; PORT=17730
mkdir -p "$SIM" "$CAP" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"   # (no catalog server here: tests/dl_e2e.sh has one)
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
status() { curl -s "http://127.0.0.1:$PORT/api/status"; }
# a number from the status: field NAME of the whole line, or of slot N's object (slot N NAME)
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
slot() { field "$2$1"; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
press() { cmd "$1 127"; sleep 0.12; cmd "$1 0"; sleep 0.12; }     # "note N" / "cc N"
wait_for() {   # wait_for SECONDS 'test'
    _n=$(( $1 * 10 )); shift
    while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1
}
start() {
    build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca "$@" \
        >>"$T/host.log" 2>&1 &
    HOST=$!
    wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null'
}
stop() { cmd quit; wait $HOST; RC=$?; }

echo "sim_e2e"
start --capture "$CAP" --capture-seconds 120
if [ -n "$(field blocks)" ]; then ok "the host runs on the simulator; Move Remote's core took its session"
else bad "the host did not start"; cat "$T/host.log"; exit 1; fi
wait_for 5 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state" && grep -q "led_pending=0 " "$SIM/mr-state"' \
    && ok "rfm1cs connected in-process (session 2), the Move's screen drawn, its LEDs written ($(field led_writes))" \
    || bad "the session: $(status) / $(cat "$SIM/mr-state")"
[ "$(slot 0 id)" = felucca ] && ok "slot 1: felucca, from the bundled firmwares" || bad "slot 1 is '$(slot 0 id)'"

# ---- the pads play FM-1 keys: sound comes out of the Move's audio path, the FM-1's LEDs reach the Move
sleep 1.2
q=$(field peak); leds0=$(field led_writes)
cmd "note 84 110" "note 88 110" "note 91 110"
wait_for 3 '[ "$(field peak)" -gt 1000 ]' && ok "pads 84 88 91: sound (mix peak $(field peak), quiet before: $q)" \
    || bad "pads: no sound (peak $(field peak))"
sleep 1.1; echo "  info  CPU, Felucca playing: $(status | sed 's/.*"slots":\[\([^]]*\)\].*/\1/')"
grep -q "peak=0 " "$SIM/mr-state" && sleep 0.3
grep -q "peak=0 " "$SIM/mr-state" && bad "mr-state: no peak at Move Remote's output" \
    || ok "Move Remote's output stage sees it: $(sed 's/.*\(peak=[0-9]*\).*/\1/' "$SIM/mr-state")"
cmd "note 84 0" "note 88 0" "note 91 0"
wait_for 3 '[ "$(field led_writes)" -gt "$leds0" ]' && ok "the FM-1's LEDs reach the Move: $leds0 -> $(field led_writes) LED writes" \
    || bad "no LED writes from the keys"
cmd shot; sleep 0.4
lit=$(tail -n +3 "$SIM/mr-screen.pbm" 2>/dev/null | tr -cd 1 | wc -c)
[ "$lit" -gt 50 ] && ok "the Move's screen: rfm1cs's drawing of the FM-1 screen ($lit pixels lit)" || bad "the screen shot: $lit pixels lit"
sc0=$(field screens); press "cc 85"   # Play: the FM-1's PLAY
wait_for 3 '[ "$(field screens)" -gt "$sc0" ]' && ok "the screen follows the firmware: $sc0 -> $(field screens) screens" \
    || bad "no new screens after Play"
press "cc 85"

# ---- the page
U="http://127.0.0.1:$PORT"
code() { curl -s -o /dev/null -w '%{http_code} %{content_type}' "$@"; }
curl -s "$U/" | grep -q '<meta name="fm1-host" content="move">' && curl -s "$U/" | grep -q 'src="js/main.js"' \
    && ok "GET / : RemoteFM1's site, built for the Move (the host flag)" || bad "GET /: $(curl -s "$U/" | head -c 200)"
# every file the site refers to: index.html's href/src, the CSS's url(), each module's static imports
refs=$( { sed -n 's/.*\(href\|src\)="\([^":#]*\)".*/\2/p' build/site/index.html | grep -v '^\.\./'
          sed -n 's/.*url("\.\.\/\([^"]*\)").*/\1/p' build/site/css/site.css
          grep -ho 'from "\./[^"]*"' build/site/js/*.js | sed 's/from "\.\/\(.*\)"/js\/\1/'; } | sort -u)
n=0; miss=""
for r in $refs; do n=$((n + 1)); c=$(code "$U/$r"); case "$c" in 200*) ;; *) miss="$miss $r($c)";; esac; done
[ -z "$miss" ] && [ $n -gt 30 ] && ok "every file the site refers to loads ($n: its scripts, styles, fonts)" || bad "missing:$miss"
[ "$(code "$U/js/main.js")" = "200 text/javascript; charset=utf-8" ] && [ "$(code "$U/css/site.css")" = "200 text/css; charset=utf-8" ] \
    && [ "$(code "$U/fonts/DMSans-VF-latin.woff")" = "200 font/woff" ] && [ "$(code "$U/index.html")" = "200 text/html; charset=utf-8" ] \
    && ok "content types: text/javascript, text/css, font/woff, text/html" || bad "content types: $(code "$U/js/main.js") / $(code "$U/fonts/DMSans-VF-latin.woff")"
[ "$(code "$U/nothing" | cut -c1-3)" = 404 ] && [ "$(code --path-as-is "$U/../etc/passwd" | cut -c1-3)" = 404 ] \
    && [ "$(code --path-as-is "$U/js/../../build/fw/felucca-1.8.2.json" | cut -c1-3)" = 404 ] \
    && [ "$(code "$U/api/fw/felucca-1.8.2.rfm1m" | cut -c1-3)" = 404 ] && ok "404: unknown paths, .. paths, a firmware's object" || bad "404s"
fws=$(curl -s "$U/api/firmwares")
echo "$fws" | grep -q '"id":"choralroot"' && echo "$fws" | grep -q '"id":"felucca"' \
    && echo "$fws" | grep -q '"skin":\["rfm1skin","rfm1skin.bg","rfm1skin.bgl"\]' \
    && ok "/api/firmwares: the installed firmwares (ChoralRoot with its skin's three files)" || bad "/api/firmwares: $fws"
curl -s "$U/api/fw/choralroot-0.41.rfm1skin" | grep -q '"skin":1' && [ "$(code "$U/api/fw/choralroot-0.41.rfm1skin.bg" | cut -c1-3)" = 200 ] \
    && [ "$(code "$U/api/fw/felucca-1.8.2.licenses.txt" | cut -c1-3)" = 200 ] \
    && ok "/api/fw/: a firmware's skin (rfm1.skin JSON, its image) and licences" || bad "/api/fw/"
sleep 2.5
q=$(field peak)
out=$(build/tools/wsprobe $PORT 2500 --key 9)
if [ $? = 0 ]; then ok "the WebSocket: $(echo "$out" | head -1 | sed 's/wsprobe: //')"; else bad "the WebSocket: $out"; fi
echo "$out" | grep -q '"name":"Felucca"' && ok "the page's info names the firmware and its panel" || bad "info: $out"
p=$(field peak)
[ "$p" -gt 1000 ] && ok "a key from the page sounds (peak $q -> $p)" || bad "the page's key: peak $q -> $p"

# ---- a firmware switch from the Move: Track 1 held + step 1 (the projects view), step 2 (All), the first pad
# (ChoralRoot, A-Z), jog press (FW_LOAD)
cmd "cc 43 127"; sleep 0.15; press "note 16"; cmd "cc 43 0"; sleep 0.3
press "note 17"
press "note 92"
press "cc 3"
wait_for 10 '[ "$(slot 0 id)" = choralroot ]' && ok "the projects view switched slot 1 to ChoralRoot (FW_LOAD)" \
    || { bad "no switch: slot 1 is '$(slot 0 id)'"; tail -5 "$T/host.log"; }
[ -s "$ROOT/states/felucca/flash.bin" ] && ok "Felucca's flash saved when it left the slot" || bad "no felucca flash.bin"

# ---- states: Loop (the states), an empty pad saves, the same pad then loads
b0=$(slot 0 boot)
press "cc 58"
press "note 92"
wait_for 5 '[ -s "$ROOT/states/choralroot/state-00.bin" ] && [ "$(slot 0 states)" = 1 ]' \
    && ok "STATE_SAVE: states/choralroot/state-00.bin, the core told (states $(slot 0 states))" || bad "no state saved ($(slot 0 states))"
press "note 92"
wait_for 5 '[ "$(slot 0 boot)" -gt "$b0" ]' && ok "STATE_LOAD: ChoralRoot booted again from state 1 (boot $b0 -> $(slot 0 boot))" \
    || bad "no state load (boot $(slot 0 boot))"
# ---- a second slot: Track 2 in the view (FW_ACTIVE 2), Felucca (the second pad), jog press; Track 1 back
press "cc 51"
press "cc 42"
press "note 93"
press "cc 3"
wait_for 10 '[ "$(slot 1 id)" = felucca ]' && ok "Track 2: Felucca loaded into slot 2, beside ChoralRoot (voices 3)" \
    || bad "slot 2 is '$(slot 1 id)' (active $(field active))"
press "cc 43"
wait_for 3 '[ "$(field active)" = 0 ]' && ok "Track 1: slot 1 is the one the controls play again (FW_ACTIVE)" \
    || bad "active is $(field active)"
press "cc 51"; press "cc 51"; press "cc 51"
sleep 0.5
cmd "note 84 110" "note 88 110" "note 91 110"
wait_for 3 '[ "$(field peak)" -gt 1000 ]' && ok "ChoralRoot plays from the pads (peak $(field peak))" \
    || bad "ChoralRoot: no sound (peak $(field peak), mode $(field mode))"
cmd "note 84 0" "note 88 0" "note 91 0"
echo "  info  CPU, ChoralRoot playing, Felucca idle: $(status | sed 's/.*"slots":\[\([^]]*\)\].*/\1/')"
# ---- the page's firmware picker: POST /api/load (as rfm1cs's FW_LOAD), ChoralRoot into slot 3
post() { curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data "$1" "$U/api/load"; }
[ "$(post '{"slot":2,"id":"nothing"}')" = 404 ] && [ "$(post 'not json')" = 400 ] && [ "$(post '{"slot":7,"id":"felucca"}')" = 404 ] \
    && ok "POST /api/load refuses an unknown firmware, a bad body, a bad slot" || bad "POST /api/load's refusals"
[ "$(post '{"slot":2,"id":"choralroot"}')" = 202 ] && wait_for 10 '[ "$(slot 2 id)" = choralroot ]' \
    && ok "POST /api/load: ChoralRoot into slot 3 (the page's picker), the engine's thread loads it" || bad "the page's load: slot 3 is '$(slot 2 id)'"
late=$(field late)
stop
[ "$RC" = 0 ] && ok "quit: the host left cleanly" || bad "quit: exit $RC"
[ "$late" = 0 ] && ok "no late blocks" || bad "$late late blocks"

# ---- the audio, checked: what Move Remote's audio thread took is what the engine mixed; the first firmware's own
# blocks are what tools/fwrun renders from its input log (the same firmware, the same input, the same device ms)
osz=$(stat -c %s "$CAP/out.raw"); msz=$(stat -c %s "$CAP/mix.raw")
[ "$osz" -gt 1000000 ] && cmp -s -n "$osz" "$CAP/out.raw" "$CAP/mix.raw" \
    && ok "the audio thread's output is the engine's mix, sample for sample ($((osz / 4)) frames of $((msz / 4)))" \
    || bad "out.raw ($osz) is not mix.raw's start ($msz)"
N=$(( $(stat -c %s "$CAP/raw.raw") / 4 ))
build/tools/fwrun build/fw/felucca-1.8.2.rfm1m --replay "$CAP/inputs.log" --frames $N --pcm "$T/replay.raw" >"$T/replay.txt"
cmp -s "$T/replay.raw" "$CAP/raw.raw" \
    && ok "Felucca in the host = Felucca replayed offline from its input log, bit for bit ($N frames, $(( $(stat -c %s "$CAP/inputs.log") / 12 )) inputs; $(sed 's/.*: \(rms [^,]*\).*/\1/' "$T/replay.txt"))" \
    || bad "the replay differs from the host's Felucca blocks"

# ---- a restart: the library brings ChoralRoot back on slot 1, with its state
start
wait_for 5 '[ "$(slot 0 id)" = choralroot ] && [ "$(slot 1 id)" = felucca ]' \
    && ok "restarted: slot 1 ChoralRoot (states $(slot 0 states)), slot 2 Felucca again (the library)" \
    || bad "restart: slots '$(slot 0 id)' '$(slot 1 id)'"
[ -s "$ROOT/move/library.rmvl" ] && [ -s "$ROOT/move/settings.bin" ] && ok "move/library.rmvl and move/settings.bin kept" \
    || bad "the library or the settings were not stored"
stop

echo "sim_e2e: $checks checks, $fail failed"
[ $fail = 0 ] || { echo "---- host log"; tail -40 "$T/host.log"; }
mv "$T" "$T.done" 2>/dev/null
exit $fail
