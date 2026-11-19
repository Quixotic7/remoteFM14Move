#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The FM-1 over Wi-Fi (src/fm1link.c's TCP link) on the simulator, end to end, in the build image: RemoteFM1 Wi-Fi's
# own simulator (build/sim/remotefm1-wifi_host, make wifisim: the firmware's app and net/rfm_net.c on this machine's
# sockets, "rf serve" on 127.0.0.1:7745 and UDP 7741) as the FM-1, its panel driven through its standard input (a FIFO:
# key, spin, drop, end), and a fake FM-1 on the simulated USB-A beside it (--fake-fm1). Checks: the USB-A unit links
# first; the Wi-Fi FM-1 is found by the broadcast and takes over; the screen's frames arrive and are acknowledged;
# the Wi-Fi FM-1's keys and encoders reach the slot (PANEL_EV); a connection the FM-1 drops comes back; the FM-1 gone,
# the USB-A unit is the panel again; a new FM-1 at the remembered address is connected to again.
#   tests/fm1wifi_e2e.sh [host options...]      (after make and make wifisim; skipped without the simulator)
set -u
cd "$(dirname "$0")/.."
WSIM=build/sim/remotefm1-wifi_host
echo "fm1wifi_e2e"
if [ ! -x "$WSIM" ]; then
    echo "  info  skipped: no $WSIM (make wifisim: RemoteFM1 Wi-Fi's simulator)"
    echo "fm1wifi_e2e: 0 checks, 0 failed"
    exit 0
fi
T=$(mktemp -d /tmp/rfm14wifi.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17733; TCP=7745
mkdir -p "$SIM" "$ROOT" "$T/fm1"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
U="http://127.0.0.1:$PORT"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
status() { curl -s "$U/api/status"; }
fm1() { status | sed 's/.*"fm1":\({[^}]*}\).*/\1/'; }
get() { tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
lf() { fm1 | get "$1"; }           # the link's (the host's status)
sf() { status | get "$1"; }        # the host's status
ff() { curl -s "$U/api/test/fm1" | get "$1"; }   # the fake USB-A unit's
now_ms() { date +%s%3N; }
wait_for() {   # wait_for SECONDS 'test'
    _n=$(( $1 * 20 )); shift
    while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.05; _n=$((_n - 1)); done; return 1
}
# the Wi-Fi FM-1: the simulator serving for 120 s, its panel from the FIFO (fd 3 keeps it open)
mkfifo "$T/ctl"
exec 3<>"$T/ctl"
printf 'wait 300\nrf serve 120 %s stdin\n' $TCP > "$T/serve.txt"
FM=""
fm1_start() {
    "$WSIM" "$T/serve.txt" "$T/fm1" < "$T/ctl" >> "$T/fm1.log" 2>&1 &
    FM=$!
}
pan() { echo "$*" >&3; }           # a line to the Wi-Fi FM-1's panel
cleanup() { [ -n "$FM" ] && kill "$FM" 2>/dev/null; [ -n "${HOST:-}" ] && kill "$HOST" 2>/dev/null; }
trap cleanup EXIT

build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --test-api --fake-fm1 "$@" >>"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    || { bad "the host did not start"; cat "$T/host.log"; exit 1; }

# ---- USB-A first (no Wi-Fi FM-1 yet): the slow link as before
wait_for 10 '[ "$(lf linked)" = true ] && [ "$(lf link)" = usb ]' \
    && ok "the USB-A FM-1 links first (link $(lf link), Wi-Fi $(lf wifi))" || bad "USB-A: $(fm1)"

# ---- the Wi-Fi FM-1 comes: found by the broadcast, it takes over
fm1_start
a=$(now_ms)
wait_for 15 '[ "$(lf linked)" = true ] && [ "$(lf link)" = wifi ]' \
    && ok "the Wi-Fi FM-1 found and linked over TCP $(( $(now_ms) - a )) ms after it started: $(lf addr) ($(lf wifi_name)), $(lf wifi_answers) answers" \
    || bad "Wi-Fi: $(fm1); $(tail -3 "$T/fm1.log")"
[ "$(lf addr)" = "127.0.0.1:$TCP" ] && ok "the address the answer gave (port $TCP from its port=)" || bad "addr $(lf addr)"
curl -s "$U/api/mix" | grep -q '"link":"wifi","addr":"127.0.0.1:'$TCP'"' \
    && ok "the page's mix says Wi-Fi and the address (its FM-1 pill)" || bad "mix: $(curl -s "$U/api/mix" | grep -o '"fm1":{[^}]*}')"
[ "$(lf rate)" = 0 ] && ok "not paced over TCP (rate 0)" || bad "paced over TCP: rate $(lf rate)"
wait_for 6 '[ "$(ff session)" = 0 ]' && ok "the USB-A unit's session ended (keepalive): one FM-1 at a time" \
    || bad "the USB-A unit still in a session"

# ---- the screen: frames go out and are acknowledged
a0=$(lf acks)
pan "spin K2 3"
wait_for 5 '[ "$(lf acks)" -gt "$a0" ]' && [ "$(lf in_flight)" -le 2 ] \
    && ok "the slot's screen goes out: $(lf frames) frames, $(lf acks) acknowledged (ack $(lf ack_ms) ms avg, $(lf ack_max) max)" \
    || bad "frames $(lf frames), acks $(lf acks)"
[ "$(lf timeouts)" = 0 ] && ok "no FRAME_ACK timeout" || bad "FRAME_ACK timeouts: $(lf timeouts)"

# ---- the Wi-Fi FM-1's panel plays the slot (PANEL_EV)
sleep 1
e0=$(lf events); q=$(sf peak)
pan "key 10 down"
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' && ok "a key on the Wi-Fi FM-1 plays slot 1 (peak $(sf peak), $q before; $(lf panel_ev) PANEL_EV)" \
    || bad "the Wi-Fi FM-1's key: no sound (peak $(sf peak), events $(lf events), PANEL_EV $(lf panel_ev))"
pan "key 10 up"
wait_for 4 '[ "$(sf peak)" -lt 300 ]' && ok "its release lets it go (peak $(sf peak))" || bad "the release: peak $(sf peak)"
e1=$(lf events)
pan "spin K2 -2"
wait_for 3 '[ "$(lf events)" -gt "$e1" ]' && ok "an encoder turn arrives ($(( $(lf events) - e0 )) events since the key)" \
    || bad "the encoder: events $(lf events)"

# ---- the FM-1 drops the connection (as a new host would replace it): back by itself
d0=$(lf wifi_drops); s0=$(lf sessions)
pan "drop"
wait_for 10 '[ "$(lf wifi_drops)" -gt "$d0" ]' && b=ok || b=""
a=$(now_ms)
wait_for 15 '[ "$(lf linked)" = true ] && [ "$(lf link)" = wifi ] && [ "$(lf sessions)" -gt "$s0" ]' && [ -n "$b" ] \
    && ok "dropped by the FM-1, connected again: a new session over Wi-Fi $(( $(now_ms) - a )) ms later ($(lf wifi_connects) connections)" \
    || bad "after the drop: $(fm1)"
pan "key 12 down"
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' && ok "its keys play again" || bad "keys after the reconnect: peak $(sf peak)"
pan "key 12 up"
wait_for 4 '[ "$(sf peak)" -lt 300 ]' || true

# ---- the Wi-Fi FM-1 gone (switched off): the USB-A unit is the panel again; nothing stuck
pan "key 7 down"
wait_for 3 '[ "$(sf peak)" -gt 1000 ]' || true
pan "end"
wait "$FM" 2>/dev/null; FM=""
a=$(now_ms)
wait_for 15 '[ "$(lf linked)" = true ] && [ "$(lf link)" = usb ] && [ "$(ff session)" = 1 ]' \
    && ok "the Wi-Fi FM-1 gone: the USB-A unit linked again $(( $(now_ms) - a )) ms later" || bad "fallback to USB-A: $(fm1)"
wait_for 5 '[ "$(sf peak)" -lt 300 ]' && ok "the key held on the Wi-Fi FM-1 was let go (peak $(sf peak))" \
    || bad "a stuck note: peak $(sf peak)"
grep -q "127.0.0.1:$TCP" "$ROOT/fm1-wifi.txt" 2>/dev/null && ok "its address remembered ($ROOT/fm1-wifi.txt)" \
    || bad "fm1-wifi.txt: $(cat "$ROOT/fm1-wifi.txt" 2>/dev/null)"

# ---- it comes back (switched on again): Wi-Fi takes over again
fm1_start
a=$(now_ms)
wait_for 15 '[ "$(lf linked)" = true ] && [ "$(lf link)" = wifi ]' \
    && ok "switched on again: linked over Wi-Fi $(( $(now_ms) - a )) ms later ($(lf sessions) sessions)" || bad "the return: $(fm1)"
info "the link at the end: $(fm1)"
pan "end"
wait "$FM" 2>/dev/null; FM=""

printf 'quit\n' > "$SIM/mr-cmd"
wait $HOST; HOST=""
grep -q "remotefm1-host: done\|FM-1 lost (stop)" "$T/host.log" && ok "the host stopped cleanly" || bad "the stop: $(tail -5 "$T/host.log")"
if [ $fail != 0 ]; then
    echo "---- host.log"; grep "fm1link" "$T/host.log" | tail -40
    echo "---- fm1.log"; tail -20 "$T/fm1.log"
fi
echo "fm1wifi_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
