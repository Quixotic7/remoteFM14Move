#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The FM-1's Wi-Fi set up from the page (src/fm1link.c fm1link_wifi_*, src/web.c /api/fm1/wifi, js/movewifi.js), end
# to end on the simulator, in the build image: a fake FM-1 with RemoteFM1 Wi-Fi on the simulated USB-A (fm1sim.c
# "wifi": libremotefm1's fake unit, cfg.wifi; the bridge loses what a burst does not fit), the page's API as the phone
# drives it. Checks: the unit on USB-A is seen and is the Wi-Fi build; a scan's networks arrive (several pages over the
# slow link, each small); a bad password is refused before anything is sent; a wrong one fails on the unit and says
# so; a right one: sent, the unit joins and keeps it, the password arrived whole, it is in no log and no file of the
# Move's; a network forgotten; then RemoteFM1 Wi-Fi's own simulator comes up as the joined FM-1 (127.0.0.1:7746) and the
# Move links to it over TCP; with nothing on USB-A, set up says so (409).
#   tests/fm1wifisetup_e2e.sh [host options...]   (after make; the TCP part after make wifisim)
set -u
cd "$(dirname "$0")/.."
WSIM=build/sim/remotefm1-wifi_host
echo "fm1wifisetup_e2e"
T=$(mktemp -d /tmp/rfm14wfs.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17734; TCP=7746
PASS=test-pass-1234                 # (fake credentials: fm1sim.c FM1SIM_WIFI_PASS)
mkdir -p "$SIM" "$ROOT" "$T/fm1"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
U="http://127.0.0.1:$PORT"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
W() { curl -s "$U/api/fm1/wifi"; }
has() { W | grep -q "$1"; }                       # has '"usb":true'
ff() { curl -s "$U/api/test/fm1" | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
lf() { curl -s "$U/api/status" | sed 's/.*"fm1":\({[^}]*}\).*/\1/' | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
post() { curl -s -o "$T/post.out" -w '%{http_code}' -X POST -H 'Content-Type: application/json' --data-binary "$2" "$U/api/fm1/wifi/$1"; }
fm1op() { curl -s -X POST -H 'Content-Type: application/json' -d "$1" "$U/api/test/fm1" >/dev/null; }
now_ms() { date +%s%3N; }
wait_for() {   # wait_for SECONDS 'test'
    _n=$(( $1 * 20 )); shift
    while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.05; _n=$((_n - 1)); done; return 1
}
FM=""
cleanup() { [ -n "$FM" ] && kill "$FM" 2>/dev/null; [ -n "${HOST:-}" ] && kill "$HOST" 2>/dev/null; }
trap cleanup EXIT

build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --test-api --fake-fm1 "$@" >>"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    || { bad "the host did not start"; cat "$T/host.log"; exit 1; }

# ---- a USB build on USB-A (the fake as before): seen, not the Wi-Fi build; set up refused
wait_for 10 '[ "$(lf linked)" = true ] && [ "$(lf link)" = usb ]' || bad "USB-A did not link"
W >/dev/null
wait_for 6 'has "\"usb\":true" && has "\"usb_wifi\":false"' \
    && ok "an FM-1 without RemoteFM1 Wi-Fi on USB-A: seen, said so" || bad "the USB build: $(W | cut -c1-300)"
c=$(post scan '{}'); [ "$c" = 409 ] && ok "a scan refused (409): $(cat "$T/post.out")" || bad "scan on a USB build: $c"

# ---- RemoteFM1 Wi-Fi on USB-A (a fresh fake unit plugged in): its state, then a scan
fm1op "{\"op\":\"wifi\",\"a\":1,\"b\":$TCP}"
a=$(now_ms)
wait_for 10 'has "\"usb\":true" && has "\"usb_wifi\":true" && has "\"fm1\":{\"known\":true,\"state\":\"on\""' \
    && ok "RemoteFM1 Wi-Fi on USB-A, on with no network, $(( $(now_ms) - a )) ms after it was plugged in" \
    || bad "the Wi-Fi build on USB-A: $(W | cut -c1-400)"
c=$(post scan '{}')
[ "$c" = 202 ] && ok "a scan asked (202)" || bad "scan: $c $(cat "$T/post.out")"
a=$(now_ms)
wait_for 15 'has "\"scanning\":false,\"seq\":[1-9]" && has "Yet another network nearby, far"' \
    && ok "the scan's six networks arrived over USB-A $(( $(now_ms) - a )) ms later" || bad "the scan: $(W | cut -c1-900)"
W | grep -q '"ssid":"Dogsontheroof","rssi":-52,"ch":6,"secured":true' && W | grep -q '"ssid":"Cafe open","rssi":-71,"ch":11,"secured":false' \
    && ok "strength, channel and lock for each (the strongest first)" || bad "the list: $(W | grep -o '"nets":\[.*\]' | cut -c1-500)"
[ "$(W | grep -o '"ssid":"[^"]*","rssi"' | wc -l)" = 6 ] && ok "each network once" || bad "networks: $(W | grep -o '"ssid":"[^"]*","rssi"')"
[ "$(ff wifi_scans)" -ge 1 ] && [ "$(ff wifi_reqs)" -ge 4 ] && ok "the unit took $(ff wifi_scans) SCAN, $(ff wifi_reqs) WIFI requests in all (the list in pages)" \
    || bad "requests: $(ff wifi_scans) scans, $(ff wifi_reqs) in all"

# ---- bad requests: nothing sent
s0=$(ff wifi_sets)
[ "$(post set '{"ssid":"Phone hotspot","password":"short"}')" = 400 ] && ok "a password under 8 characters refused by the Move (400)" || bad "short password: $(cat "$T/post.out")"
[ "$(post set '{"ssid":"","password":"whatever-long"}')" = 400 ] && ok "no network name: refused (400)" || bad "no ssid"
[ "$(post set '{"ssid":"Phone hotspot","password":"0123456789012345678901234567890123456789012345678901234567890123"}')" = 400 ] \
    && ok "a 64-character password refused (400)" || bad "64 chars: $(cat "$T/post.out")"
[ "$(ff wifi_sets)" = "$s0" ] && ok "none of them reached the unit" || bad "sets $s0 -> $(ff wifi_sets)"

# ---- a wrong password: the unit fails to join and the page says why (fake: anything 8+ joins, so an open
# network's name it does not hear stands in for "not found")
c=$(post set '{"ssid":"Nowhere network","password":"wrong-pass-999"}')
[ "$c" = 202 ] && ok "a network the FM-1 does not hear: sent (202)" || bad "set: $c $(cat "$T/post.out")"
wait_for 10 'has "\"phase\":\"failed\",\"ssid\":\"Nowhere network\",\"why\":\"not found\""' \
    && ok "it failed on the unit, and the page says why (not found)" || bad "the failure: $(W | grep -o '"job":{[^}]*}')"

# ---- the right password: sent, joined, kept; the Move asks that address
a=$(now_ms)
c=$(post set "{\"ssid\":\"Phone hotspot\",\"password\":\"$PASS\",\"pick\":1,\"scan_seq\":1}")
[ "$c" = 202 ] && ok "Phone hotspot with its password: sent (202)" || bad "set: $c $(cat "$T/post.out")"
wait_for 6 'has "\"phase\":\"joining\""' && ok "joining (the page shows progress)" || info "joining not seen (fast): $(W | grep -o '"job":{[^}]*}')"
wait_for 15 'has "\"phase\":\"connected\",\"ssid\":\"Phone hotspot\""' \
    && ok "the FM-1 joined Phone hotspot $(( $(now_ms) - a )) ms after Join: $(W | grep -o '"job":{[^}]*}')" \
    || bad "the join: $(W | grep -o '"job":{[^}]*}') $(W | grep -o '"fm1":{[^}]*}')"
has '"fm1":{"known":true,"state":"connected","ssid":"Phone hotspot","ip":"127.0.0.1"' && has '"same_lan":1' \
    && ok "its state: connected, 127.0.0.1, on the Move's own network" || bad "state: $(W | grep -o '"fm1":{[^}]*}')"
[ "$(ff wifi_state)" = 5 ] && [ "$(ff wifi_ssid)" = "Phone hotspot" ] && [ "$(ff wifi_pass_ok)" = 1 ] \
    && ok "the unit keeps it, with the password exactly as typed" || bad "the unit: state $(ff wifi_state) ssid $(ff wifi_ssid) pass_ok $(ff wifi_pass_ok)"
wait_for 6 'W | grep -q "\"kept\":\[{\"ssid\":\"Phone hotspot\",\"heard\":true"' && ok "the networks it keeps: Phone hotspot (in range)" \
    || bad "kept: $(W | grep -o '"kept":\[[^]]*\]')"
if grep -rq -D skip -- "$PASS" "$T/host.log" "$ROOT" "$SIM" 2>/dev/null; then bad "the password is in a log or a file: $(grep -rl -D skip -- "$PASS" "$T/host.log" "$ROOT" "$SIM")"
else ok "the password is in no log and no file of the Move's ($(grep -c 'Wi-Fi setup' "$T/host.log") set up lines logged)"; fi

# ---- a second network, then one forgotten
post set "{\"ssid\":\"Dogsontheroof\",\"password\":\"$PASS\"}" >/dev/null
wait_for 15 'has "\"phase\":\"connected\",\"ssid\":\"Dogsontheroof\""' && wait_for 6 'W | grep -q "\"kept\":\[{\"ssid\":\"Dogsontheroof\"[^]]*\"ssid\":\"Phone hotspot\""' \
    && ok "two networks kept, the most recent first" || bad "two kept: $(W | grep -o '"kept":\[[^]]*\]')"
[ "$(post forget '{"ssid":"Phone hotspot"}')" = 202 ] && wait_for 6 '! W | grep -q "\"kept\":\[[^]]*Phone hotspot"' \
    && [ "$(ff wifi_kept)" = 1 ] && ok "Phone hotspot forgotten (the unit keeps $(ff wifi_kept))" || bad "forget: $(W | grep -o '"kept":\[[^]]*\]') unit $(ff wifi_kept)"
[ "$(post forget '{"ssid":"Never kept"}')" = 404 ] && ok "forgetting one it does not keep: 404" || bad "forget unknown"

# ---- the joined FM-1 comes up on the network (RemoteFM1 Wi-Fi's simulator at the address the unit reported): linked
if [ -x "$WSIM" ]; then
    printf 'wait 300\nrf serve 40 %s\n' $TCP > "$T/serve.txt"
    "$WSIM" "$T/serve.txt" "$T/fm1" >> "$T/fm1.log" 2>&1 &
    FM=$!
    a=$(now_ms)
    wait_for 15 '[ "$(lf linked)" = true ] && [ "$(lf link)" = wifi ] && [ "$(lf addr)" = "127.0.0.1:'$TCP'" ]' \
        && ok "the Move links to it over Wi-Fi $(( $(now_ms) - a )) ms later ($(lf addr))" || bad "Wi-Fi link: $(lf link) $(lf addr) $(lf state)"
    wait_for 4 'has "\"link\":\"wifi\""' && ok "the panel says so (link wifi)" || bad "link in /api/fm1/wifi: $(W | cut -c1-200)"
    has '"usb":true' && ok "the FM-1 on USB-A still answers beside the TCP session" || bad "USB-A while on Wi-Fi: $(W | cut -c1-200)"
    kill "$FM" 2>/dev/null; wait "$FM" 2>/dev/null; FM=""
else
    info "no $WSIM (make wifisim): the TCP part skipped"
fi

# ---- nothing on USB-A: said, refused
fm1op '{"op":"plug","a":0}'
wait_for 12 'has "\"usb\":false"' && ok "unplugged: the panel says setup needs USB-A" || bad "unplugged: $(W | cut -c1-200)"
c=$(post set "{\"ssid\":\"Cafe open\",\"password\":\"\"}"); [ "$c" = 409 ] && ok "a set with nothing on USB-A: 409" || bad "set unplugged: $c"

printf 'quit\n' > "$SIM/mr-cmd"
wait $HOST; HOST=""
grep -q "remotefm1-host: done\|FM-1 lost (stop)" "$T/host.log" && ok "the host stopped cleanly" || bad "the stop: $(tail -5 "$T/host.log")"
if [ $fail != 0 ]; then echo "---- host.log"; grep "fm1link\|web" "$T/host.log" | tail -40; fi
echo "fm1wifisetup_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
