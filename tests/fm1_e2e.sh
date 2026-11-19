#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The FM-1 on the Move's USB-A (src/fm1link.c) on the simulator, end to end, in the build image: a fake FM-1
# (libremotefm1's fake unit, src/fm1sim.c) behind Move Remote's simulated USB-A, whose bridge loses what a burst from
# the unit does not fit (plat_sim.c), driven through the page's test API (POST/GET /api/test/fm1).
#   tests/fm1_e2e.sh [host options...]      (after make; e.g. --fm1-bps 9000 --fm1-cap 8 to try other rates)
# (--fm1-usba-screen: the firmware's screen over USB-A, as before; the default plug screen: tests/fm1plug_e2e.sh)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14fm1.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17732
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
U="http://127.0.0.1:$PORT"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
status() { curl -s "$U/api/status"; }
fm1() { status | sed 's/.*"fm1":\({[^}]*}\).*/\1/'; }
fake() { curl -s "$U/api/test/fm1"; }
# a field of a one-level JSON object on stdin
get() { tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
ff() { fake | get "$1"; }          # the fake unit's
lf() { fm1 | get "$1"; }           # the link's (the host's status)
sf() { status | get "$1"; }        # the host's status
mrs() { sed -n "s/.* $1=\([0-9-]*\).*/\1/p" "$SIM/mr-state"; }   # Move Remote's state line
unit() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"op\":\"$1\",\"a\":$2,\"b\":$3}" "$U/api/test/fm1"; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
now_ms() { date +%s%3N; }
wait_for() {   # wait_for SECONDS 'test'
    _n=$(( $1 * 20 )); shift
    while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.05; _n=$((_n - 1)); done; return 1
}

echo "fm1_e2e"
build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --test-api --fake-fm1 --fm1-usba-screen "$@" >>"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    || { bad "the host did not start"; cat "$T/host.log"; exit 1; }

# ---- discovery, the session
t0=$(now_ms)
wait_for 8 '[ "$(lf linked)" = true ] && [ "$(ff session)" = 1 ]' \
    && ok "a session: HELLO answered, SESSION started ($(lf name) $(lf fw); the unit took $(ff hellos) HELLO)" \
    || bad "no session: $(fm1) / $(fake)"
curl -s "$U/api/info" | grep -q '"fm1":{"linked":true' && curl -s "$U/api/mix" | grep -q '"fm1":{"linked":true' \
    && ok "/api/info and /api/mix say linked" || bad "info / mix: no fm1 linked"
[ "$(lf rate)" -gt 0 ] && info "slow link: $(lf rate) B/s of screen; $(grep -o 'packets a transfer' "$T/host.log" >/dev/null && sed -n 's/.*fm1link: on USB-A, \(.*\)/\1/p' "$T/host.log")"

# ---- the screen and the LEDs converge
wait_for 15 '[ "$(ff screen_exact)" = 1 ] && [ "$(ff holding)" = 0 ]' \
    && ok "the unit's screen is the active slot's, exactly, $(( $(now_ms) - t0 )) ms after the link started ($(ff frames) frames, $(ff held_frames) held)" \
    || bad "the screen: $(ff screen_pct) % the same ($(fake))"
wait_for 5 '[ "$(ff leds_match)" = 1 ] && [ "$(ff leds)" -gt 0 ]' && ok "the LEDs arrived (the slot's levels as the unit's planes)" \
    || bad "the LEDs: $(fake)"

# ---- the unit's keys, encoders and MIDI in play the active slot
sleep 1
q=$(sf peak)
unit key 10 110 >/dev/null
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' && ok "a key on the unit plays slot 1 (mix peak $(sf peak), $q before)" \
    || bad "the unit's key: no sound (peak $(sf peak))"
unit press 10 90 >/dev/null; sleep 0.2
unit key 10 0 >/dev/null
wait_for 4 '[ "$(sf peak)" -lt 300 ]' && ok "its release lets it go (peak $(sf peak))" || bad "the key's release: peak $(sf peak)"
[ "$(ff leds_match)" = 1 ] || sleep 1
[ "$(ff leds_match)" = 1 ] && ok "the LEDs follow the slot" || bad "the LEDs after the key: $(fake)"
unit trs 60 100 >/dev/null
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' && ok "MIDI into the unit's 5-pin jack (MIDI_IN) plays slot 1 (peak $(sf peak))" \
    || bad "MIDI_IN: no sound ($(lf midi_in) messages)"
unit trs 60 0 >/dev/null
wait_for 4 '[ "$(sf peak)" -lt 300 ]' || true

# small changes: an encoder turn, the time until the unit shows the slot's new screen (its tiles reduced to 4 colours
# at first), then exactly (the refinement, once the screen has been still for 1 s); 10 times
lat=""; ex=""; worst=0; sum=0; n=0; esum=0
for i in 1 2 3 4 5 6 7 8 9 10; do
    wait_for 4 '[ "$(ff screen_exact)" = 1 ]'
    u0=$(ff panel_updates); a=$(now_ms)
    unit enc 3 $(( i % 2 * 2 - 1 )) >/dev/null
    if wait_for 3 '[ "$(ff panel_updates)" -gt "$u0" ]'; then
        d=$(( $(now_ms) - a )); lat="$lat $d"; sum=$((sum + d)); n=$((n + 1)); [ $d -gt $worst ] && worst=$d
        wait_for 4 '[ "$(ff screen_exact)" = 1 ]' && { e=$(( $(now_ms) - a )); ex="$ex $e"; esum=$((esum + e)); }
    fi
done
[ $n -ge 8 ] && [ $((sum / n)) -lt 400 ] \
    && ok "an encoder on the unit changes the slot's screen, shown on the unit $((sum / n)) ms later on average ($worst ms at worst, $n of 10), exact $((esum / n)) ms later" \
    || bad "encoder turns shown: $n of 10, $((sum / (n ? n : 1))) ms ($(fake))"
info "encoder -> the unit's screen, ms:$lat; exact:$ex (the test's polling adds up to ~50 ms)"

# a busy screen: an encoder turned 20 times a second for 6 s; the unit's screen updates a second, the link's rate, and
# the Move's own LEDs meanwhile (its pads lit by presses through the simulator)
u0=$(ff panel_updates); f0=$(ff frames); w0=$(ff usba_pkts); l0=$(ff led_pkts); a=$(now_ms); late0=$(mrs late)
pend=0; i=0
while [ $i -lt 120 ]; do
    unit enc 3 $(( i / 10 % 2 * 2 - 1 )) >/dev/null
    [ $((i % 10)) = 0 ] && cmd "note $((68 + i / 10 % 8)) 100"
    [ $((i % 10)) = 5 ] && cmd "note $((68 + i / 10 % 8)) 0"
    p=$(mrs led_pending); [ "${p:-0}" -gt $pend ] && pend=$p
    sleep 0.05; i=$((i + 1))
done
dt=$(( $(now_ms) - a ))
ups=$(( ($(ff panel_updates) - u0) * 1000 / dt )); fps=$(( ($(ff frames) - f0) * 1000 / dt ))
wire=$(( ($(ff usba_pkts) - w0) * 3 * 1000 / dt )); leds=$(( $(ff led_pkts) - l0 ))
info "busy screen for $((dt / 1000)) s: $ups screens a second shown on the unit, $fps frames a second, ~$wire B/s on the USB-A wire (link: $(lf bps) B/s, $(lf fps) frames acknowledged a second, ack $(lf ack_ms) ms avg / $(lf ack_max) ms max)"
[ "$leds" -gt 50 ] && [ "$pend" -lt 40 ] && ok "the Move's own LEDs kept up while the unit streamed: $leds LED packets out, at most $pend pending" \
    || bad "the Move's LEDs: $leds packets, $pend pending at most"
[ "$ups" -ge 2 ] && ok "the unit's screen kept changing ($ups a second)" || bad "the busy screen: $ups updates a second"
wait_for 10 '[ "$(ff screen_exact)" = 1 ] && [ "$(ff holding)" = 0 ]' && ok "and converges once still" || bad "after the busy screen: $(ff screen_pct) %"
[ "$(lf timeouts)" = 0 ] && [ "$(ff decode_errs)" = 0 ] && [ "$(ff bridge_lost)" = 0 ] && [ "$(lf usb_in_dropped)" = 0 ] \
    && ok "nothing lost: no FRAME_ACK timeout, no decode error, nothing lost in the bridge or the USB-A queue" \
    || bad "losses: timeouts $(lf timeouts), decode errors $(ff decode_errs), bridge lost $(ff bridge_lost), in dropped $(lf usb_in_dropped)"

# ---- a slot switch on the Move: the unit shows and plays the other slot
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST --data '{"slot":1,"id":"choralroot","activate":1}' "$U/api/load")
a=$(now_ms)
wait_for 15 '[ "$(sf active)" = 1 ] && [ "$(status | get id1)" = choralroot ]' && ok "slot 2: choralroot, active ($code)" \
    || bad "slot 2 did not load: $(sf active)"
wait_for 15 '[ "$(ff screen_exact)" = 1 ] && [ "$(ff holding)" = 0 ]' \
    && ok "the unit shows slot 2's screen ($(( $(now_ms) - a )) ms after the load)" || bad "slot 2's screen: $(ff screen_pct) %"
sleep 1; q=$(sf peak)
unit key 12 110 >/dev/null
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' && ok "a key on the unit plays slot 2 (peak $(sf peak), $q before)" || bad "slot 2: no sound"
curl -s -o /dev/null -X POST --data '{"op":16,"a":0,"b":0}' "$U/api/edit"   # back to slot 1, the key still held
a=$(now_ms)
wait_for 10 '[ "$(sf active)" = 0 ] && [ "$(ff screen_exact)" = 1 ] && [ "$(ff holding)" = 0 ]' \
    && ok "back on slot 1: the unit follows ($(( $(now_ms) - a )) ms)" || bad "the switch back: active $(sf active), $(ff screen_pct) %"
unit key 12 0 >/dev/null
wait_for 5 '[ "$(sf peak)" -lt 300 ]' && ok "the key held across the switch was let go on slot 2 (peak $(sf peak))" \
    || bad "a stuck note after the switch: peak $(sf peak)"

# ---- unplugged with a key held, plugged in again
unit key 5 120 >/dev/null
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' || true
unit plug 0 0 >/dev/null
a=$(now_ms)
wait_for 8 '[ "$(lf linked)" = false ]' && b=$(( $(now_ms) - a )) && wait_for 10 '[ "$(lf state)" = discover ]' \
    && ok "unplugged: lost after $b ms (no PANEL_R), back to discovery after $(( $(now_ms) - a )) ms (SESSION tried 4 times)" \
    || bad "the unplug: $(fm1)"
wait_for 5 '[ "$(sf peak)" -lt 300 ]' && ok "nothing stuck: the held key let go (peak $(sf peak))" || bad "a stuck note: peak $(sf peak)"
curl -s "$U/api/mix" | grep -q '"fm1":{"linked":false' && ok "the page's mix says not linked" || bad "mix: still linked"
sleep 2
unit plug 1 0 >/dev/null
a=$(now_ms)
wait_for 10 '[ "$(lf linked)" = true ] && [ "$(ff session)" = 1 ]' && ok "plugged in again: a new session after $(( $(now_ms) - a )) ms ($(lf sessions) sessions)" \
    || bad "the replug: $(fm1)"
wait_for 15 '[ "$(ff screen_exact)" = 1 ] && [ "$(ff holding)" = 0 ] && [ "$(ff leds_match)" = 1 ]' \
    && ok "its screen and LEDs converge again ($(( $(now_ms) - a )) ms)" || bad "after the replug: $(fake)"

# ---- the audio: no late block all along
# (the engine's late blocks are the audio's; Move Remote's "late" on the simulator is its SPI thread waking late in the
# container, not the audio)
[ "$(sf late)" = 0 ] && ok "no late audio block (the engine's ring never ran dry; the simulator's SPI thread woke late $(mrs late) times)" \
    || bad "late blocks: engine $(sf late)"
info "the link at the end: $(fm1)"

cmd quit
wait $HOST
grep -q "FM-1 lost (stop)\|remotefm1-host: done" "$T/host.log" && ok "the host stopped cleanly" || bad "the stop: $(tail -5 "$T/host.log")"
if [ $fail != 0 ]; then echo "---- host.log"; grep -v "^engine: slot\|lines dropped" "$T/host.log" | tail -60; fi
echo "fm1_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
