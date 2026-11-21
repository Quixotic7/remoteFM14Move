#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Out (src/midiout.c, README "MIDI out") on the simulator, in the build image, the messages sent from the engine's block
# (POST /api/test/midiout, --test-api):
#   - network MIDI: tests/rtpmidi_peer.c as the Mac: the session found by mDNS ("RemoteFM1 Move", _apple-midi._udp, its
#     SRV's port), both ports invited, the clock synced; a note on, a short SysEx, a note off and a 1500-byte SysEx (two
#     segments) arrive as sent; the clock byte (F8) does not; RS and BY seen;
#   - USB-A: with the fake FM-1 (RemoteFM1) linked there Out skips it (the unit gets no channel message, the link stays);
#     a plain MIDI device in its place gets Out's note.
#   - (rfm1cs 0.9.0) the sequencer's own clock: Play in a note layout sends MIDI Start, clock and Stop to every loaded
#     firmware (their input logs, --capture).
#   tests/midiout_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14midiout.XXXXXX)
ROOT=$T/root; SIM=$T/sim; PORT=17794; RTP=15004
mkdir -p "$SIM" "$ROOT"
echo "http://127.0.0.1:1" > "$ROOT/catalog-url"
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
get() { tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
mo() { status | sed 's/.*"midiout":\({[^}]*}\).*/\1/' | get "$1"; }   # Out's
ff() { curl -s "$U/api/test/fm1" | get "$1"; }                         # the fake unit's
send() { curl -s -o /dev/null -w '%{http_code}' -X POST --data "{\"hex\":\"$1\"}" "$U/api/test/midiout"; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
up() { wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
       && wait_for 5 '[ "$(status | get session)" = 2 ]'; }

echo "midiout_e2e"
# ---- network MIDI (no FM-1 link: USB-A is Move Remote's, Out leaves it)
build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --no-fm1 --test-api --mdns --rtp-port $RTP >>"$T/host.log" 2>&1 &
HOST=$!
up || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
[ "$(mo rtp_port)" = $RTP ] && [ "$(mo usba)" = off ] && grep -q "mdns: answering for remotefm1.local and \"RemoteFM1 Move\"" "$T/host.log" \
    && ok "Out: RTP-MIDI on UDP $RTP/$((RTP + 1)), advertised by mDNS; USB-A off (--no-fm1)" \
    || bad "Out's start: rtp_port '$(mo rtp_port)', usba '$(mo usba)'; $(grep -E 'midiout|mdns' "$T/host.log" | head -4)"
LONG=$(i=0; while [ $i -lt 1500 ]; do printf '%02x' $((i % 128)); i=$((i + 1)); done)
WANT="903c64f07d0102f7803c00f0${LONG}f7"
build/tools/rtpmidi-peer --mdns-port 5353 --name "RemoteFM1 Move" --port $RTP --ready "$T/ready" --timeout 20 \
    --expect "$WANT" >"$T/peer.log" 2>&1 &
PEER=$!
wait_for 15 '[ -f "$T/ready" ] && [ "$(mo rtp_peers)" = 1 ]' \
    && ok "the initiator: $(grep -m1 '^mdns:' "$T/peer.log"); invited, $(grep -m1 '^CK:' "$T/peer.log" | cut -d, -f1); a peer of the session" \
    || bad "no session: $(cat "$T/peer.log"); peers '$(mo rtp_peers)'"
grep -q 'midiout: "rtpmidi-peer" (127.0.0.1) joined the session' "$T/host.log" && ok "the host: \"rtpmidi-peer\" joined" \
    || bad "the host's log: $(grep midiout "$T/host.log" | tail -3)"
[ "$(send f8903c64f07d0102f7803c00)" = 202 ] && sleep 0.3 && [ "$(send "f0${LONG}f7")" = 202 ] \
    || bad "POST /api/test/midiout refused"
wait $PEER; rc=$?
[ $rc = 0 ] && ok "the messages arrived as sent, the clock byte not: $(grep '^got' "$T/peer.log")" \
    || bad "the initiator got: $(grep '^got\|damaged\|no session' "$T/peer.log")"
wait_for 3 '[ "$(mo rtp_peers)" = 0 ]' && grep -q 'left (BY)' "$T/host.log" && ok "BY: the peer gone" \
    || bad "after BY: peers '$(mo rtp_peers)'"
[ "$(mo rtp_sent)" = 4 ] && [ "$(mo filtered)" = 1 ] && [ "$(mo dropped)" = 0 ] && [ "$(mo rtp_ck)" -ge 1 ] \
    && [ "$(mo rtp_rs)" -ge 1 ] && [ "$(mo rtp_packets)" -ge 3 ] \
    && ok "counted: 4 messages in $(mo rtp_packets) packets, 1 real-time byte filtered, nothing dropped, $(mo rtp_ck) CK, $(mo rtp_rs) RS" \
    || bad "counters: $(status | sed 's/.*\("midiout":{[^}]*}\).*/\1/')"
cmd quit; wait $HOST

# ---- USB-A: the fake FM-1 (RemoteFM1) linked there, then a plain MIDI device
build/sim/remotefm1-host --root "$ROOT" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --test-api --fake-fm1 --no-fm1-wifi --no-rtp >>"$T/host2.log" 2>&1 &
HOST=$!
up || { bad "the host did not start (USB-A)"; tail -20 "$T/host2.log"; exit 1; }
wait_for 10 'status | grep -q "\"fm1\":{\"linked\":true" && [ "$(mo usba)" = fm1 ]' \
    && ok "the FM-1 linked on USB-A: Out says usba \"fm1\"" || bad "no link: usba '$(mo usba)'"
[ "$(send 903c64)" = 202 ] && wait_for 3 '[ "$(mo usba_skipped)" = 1 ]' && sleep 0.5 && [ "$(ff dev_msgs)" = 0 ] \
    && status | grep -q "\"fm1\":{\"linked\":true" \
    && ok "a note for Out while the FM-1 is on USB-A: skipped there, the unit got none, still linked" \
    || bad "with the FM-1: skipped '$(mo usba_skipped)', the unit got $(ff dev_msgs) channel message(s)"
curl -s -o /dev/null -X POST --data '{"op":"plain","a":1}' "$U/api/test/fm1"
wait_for 15 '[ "$(mo usba)" = on ]' && ok "a plain MIDI device in its place: Out says usba \"on\" (the link gone, no HELLO_R)" \
    || bad "the plain device: usba '$(mo usba)'"
[ "$(send 913e50)" = 202 ] && wait_for 3 '[ "$(ff dev_last)" = 913e50 ] && [ "$(mo usba_out_sent)" = 1 ]' \
    && ok "Out's note on USB-A: the device got 91 3e 50 (cable 2, CIN 9)" \
    || bad "USB-A out: the device's last '$(ff dev_last)', sent '$(mo usba_out_sent)'"
cmd quit; wait $HOST

# ---- (rfm1cs 0.9.0) the sequencer's clock into the firmwares: Felucca in slot 1, ChoralRoot in slot 2; slot 1's Scale
# layout (Track 1 held + the top row's pad 3: the sequencer), Play, ~1.5 s, Play: MIDI Start, clock and Stop reach both
# loaded firmwares (RFM_MOVE_DEST_ALL), read back from their input logs (--capture: inputs-slotN.log, 12-byte records
# ms, op a b, c; op 6 FWI_MIDI, c the USB-MIDI packet: 0xFA0F Start, 0xF80F clock, 0xFC0F Stop)
CAP=$T/cap; mkdir -p "$CAP"
build/sim/remotefm1-host --root "$T/root3" --port $PORT --sim-dir "$SIM" --firmwares build/fw --site build/site --fw felucca \
    --no-fm1 --no-rtp --capture "$CAP" --capture-seconds 30 >>"$T/host3.log" 2>&1 &
HOST=$!
up || { bad "the host did not start (clock)"; tail -20 "$T/host3.log"; exit 1; }
slot1() { status | grep -o '"slots":\[.*\]' | sed 's/},{/}\n{/g;s/null,/null\n/g' | sed -n 2p | get id; }
curl -s -o /dev/null -X POST --data '{"slot":1,"id":"choralroot","activate":0}' "$U/api/load"
wait_for 20 '[ "$(slot1)" = choralroot ]' || bad "ChoralRoot did not load into slot 2: '$(slot1)'"
sleep 1.5
cmd "cc 43 127"; sleep 0.2; cmd "note 94 127"; sleep 0.12; cmd "note 94 0"; sleep 0.15; cmd "cc 43 0"; sleep 0.4
[ "$(status | get move_mode)" = 3 ] || bad "Track 1 held + pad 3: move_mode '$(status | get move_mode)' (3: the sequencer)"
cmd "cc 85 127"; sleep 0.1; cmd "cc 85 0"; sleep 1.5; cmd "cc 85 127"; sleep 0.1; cmd "cc 85 0"; sleep 0.5
cmd quit; wait $HOST
rt() { od -An -v -tx4 -w12 "$1" 2>/dev/null | awk '$2 ~ /06$/ && ($3 == "0000fa0f" || $3 == "0000f80f" || $3 == "0000fc0f") \
       { sub(/^0000/, "", $3); sub(/0f$/, "", $3); print $3 }'; }
for s in 1 2; do
    L=$CAP/inputs-slot$s.log
    seq_=$(rt "$L" | uniq -c | awk '{printf "%s%s", (NR > 1 ? " " : ""), $2 "x" $1}')
    f8=$(rt "$L" | grep -c f8)
    [ -s "$L" ] && [ "$(rt "$L" | head -1)" = fa ] && [ "$(rt "$L" | tail -1)" = fc ] && [ "$(rt "$L" | grep -c fa)" = 1 ] \
        && [ "$(rt "$L" | grep -c fc)" = 1 ] && [ "$f8" -ge 50 ] && [ "$f8" -le 100 ] \
        && ok "slot $s ($([ $s = 1 ] && echo Felucca, the slot played || echo ChoralRoot, not played)): the sequencer's Start, $f8 clocks, Stop in its MIDI input ($seq_)" \
        || bad "slot $s's MIDI input: '$seq_' ($(stat -c %s "$L" 2>/dev/null) bytes of log)"
done
echo "midiout_e2e: $checks checks, $fail failed"
[ $fail = 0 ]
