#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Listen here (src/listen.c, web.c /ws/audio; README "Listening on the page") on the simulator, in the build image
# after make: Felucca played through the simulator's command pipe, pages played by build/tools/listenprobe
# (tests/listenprobe.c), the Move's output read three ways: /api/listen's out_peak (the audio thread's, after the
# mute), Move Remote's own peak (mr-state), and --capture's out.raw (what went to the Move) against mix.raw (the
# engine's mix: what the recorder and the meters see):
#   - no listener: the Move plays, the status says 0 listeners;
#   - a listener: frames of 882 frames at 44100 Hz with sound, numbered without a gap; the Move silent (out_peak 0,
#     Move Remote's peak 0) while the tap has sound; "listen: 1 listener, the Move silent" logged; the status's
#     listeners and listen_muted;
#   - it closes: the Move's sound back, logged;
#   - a listener that keeps the speaker on (?spk=1): the Move plays; "spk 0" from it: silent; two listeners, one
#     asking for silence: silent until that one leaves;
#   - a listener that stops answering (no heartbeat, no reads): closed by the host after 3 s, the sound back;
#   - a take recorded while a listener has the Move silent: it has the sound;
#   - out.raw against mix.raw: the Move's frames the mix's, or zero, or a ramp between (never anything else); a
#     silenced run of the first listener's length; the mix's own sound in every phase.
#   tests/listen_e2e.sh      (after make)
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
info() { echo "  info  $*"; }
echo "listen_e2e"
T=$(mktemp -d /tmp/rfm14listen.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; CAP=$T/cap; LIB=$T/Samples/RemoteFM1
mkdir -p "$SIM" "$R" "$CAP" "$T/Samples"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17861; U="http://127.0.0.1:$PORT"; L=$T/host.log
mkdir -p build/test-out
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
lis() { curl -s "$U/api/listen" | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\([0-9-]*\)$/\1/p"; }
mrpeak() { sed -n 's/.* peak=\([0-9]*\) .*/\1/p' "$SIM/mr-state"; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
jog() { _i=0; while [ $_i -lt "$1" ]; do cmd "cc 14 $2"; sleep 0.05; _i=$((_i + 1)); done; }
click() { cmd "cc 3 127"; cmd "cc 3 0"; sleep 0.1; }
set_() { jog 6 127; jog 1 1; jog "$1" 1; click; if [ "$2" -ge 0 ]; then jog "$2" 1; else jog $((-$2)) 127; fi; click; }
probe() { build/tools/listenprobe $PORT "$@"; }
num() { echo "$2" | sed -n "s/.* $1 \([0-9]*\) .*/\1/p"; }   # num frames "$out"
# Move Remote's peak: it is reset each time the state line is written, so a few looks, all quiet (or one loud)
mr_quiet() { _i=0; while [ $_i -lt 5 ]; do [ "$(mrpeak)" = 0 ] || return 1; sleep 0.1; _i=$((_i + 1)); done; return 0; }
mr_loud() { wait_for 2 '[ "$(mrpeak)" -gt 1000 ]'; }
# Felucca played all along (a chord, struck again each second) until $T/play goes
play() { while [ -f "$T/play" ]; do printf '%s\n' "note 84 110" "note 88 110" > "$SIM/mr-cmd"; sleep 0.9
    printf '%s\n' "note 84 0" "note 88 0" > "$SIM/mr-cmd"; sleep 0.1; done; }

build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
    --test-api --no-fm1 --takes-dir "$LIB" --capture "$CAP" --capture-seconds 120 >>"$L" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' \
    || { bad "the host did not start"; tail -20 "$L"; exit 1; }
sleep 2.5   # (the splash, the session's popups)
touch "$T/play"; play & PLAYER=$!

# ---------------------------------------------------------------- no listener
sleep 1.5
[ "$(lis listeners)" = 0 ] && [ "$(lis muted)" = 0 ] && [ "$(field listeners)" = 0 ] && mr_loud \
    && ok "no listener: the Move plays (Move Remote's peak $(mrpeak)), 0 listeners in /api/listen and the status" \
    || bad "no listener: listeners '$(lis listeners)' muted '$(lis muted)', status '$(field listeners)', mr peak '$(mrpeak)'"

# ---------------------------------------------------------------- a listener: the Move silent
probe 4000 > "$T/p1" &
P1=$!
sleep 2
lp=$(lis tap_peak); op=$(lis out_peak)
[ "$(lis listeners)" = 1 ] && [ "$(lis muted)" = 1 ] && [ "$(lis silent)" = 1 ] && [ "$op" = 0 ] && [ "$lp" -gt 1000 ] \
    && ok "a listener: the Move silent (out_peak $op) while the tap has sound (tap_peak $lp)" \
    || bad "a listener: listeners '$(lis listeners)' muted '$(lis muted)' silent '$(lis silent)' out_peak '$op' tap_peak '$lp'"
mr_quiet && ok "Move Remote's own output peak: 0 while listening" || bad "Move Remote's peak while listening: $(mrpeak)"
wait_for 2 '[ "$(field listeners)" = 1 ] && [ "$(field listen_muted)" = 1 ]' \
    && ok "the status: listeners 1, listen_muted 1" || bad "the status: listeners '$(field listeners)' muted '$(field listen_muted)'"
wait $P1; out=$(cat "$T/p1"); info "$out"
fr=$(num frames "$out"); ld=$(num loud "$out")
[ "${fr:-0}" -ge 150 ] && [ "${ld:-0}" -ge $((fr * 3 / 4)) ] && [ "$(num gaps "$out")" = 0 ] \
    && [ "$(num rate "$out")" = 44100 ] && [ "$(num size "$out")" = 882 ] && echo "$out" | grep -q '"muted":1' \
    && ok "the listener's 4 s: $fr frames of 882 at 44100 Hz, $ld with sound, no gap, its state muted" \
    || bad "the listener's frames: $out"
grep -q "listen: 1 listener, the Move silent" "$L" && ok "logged: listen: 1 listener, the Move silent" \
    || bad "no 'listen: 1 listener' in the log"
wait_for 2 '[ "$(lis listeners)" = 0 ] && [ "$(lis muted)" = 0 ] && [ "$(lis out_peak)" -gt 1000 ]' && mr_loud \
    && grep -q "listen: no listener, the Move's sound back" "$L" \
    && ok "the listener closed: the Move's sound back (out_peak $(lis out_peak), Move Remote's $(mrpeak)), logged" \
    || bad "after the close: listeners '$(lis listeners)' muted '$(lis muted)' out_peak '$(lis out_peak)' mr '$(mrpeak)'"
sleep 1

# ---------------------------------------------------------------- the speaker kept on, then "spk 0"
probe 4000 --spk --spk-at 2000:0 > "$T/p2" &
P2=$!
sleep 1.2
op=$(lis out_peak)
[ "$(lis listeners)" = 1 ] && [ "$(lis muted)" = 0 ] && [ "$op" -gt 1000 ] && mr_loud \
    && ok "a listener with the speaker on (?spk=1): the Move plays (out_peak $op)" \
    || bad "?spk=1: listeners '$(lis listeners)' muted '$(lis muted)' out_peak '$op'"
sleep 1.4
[ "$(lis muted)" = 1 ] && [ "$(lis out_peak)" = 0 ] && ok "then \"spk 0\": the Move silent" \
    || bad "after spk 0: muted '$(lis muted)' out_peak '$(lis out_peak)'"
wait $P2; out=$(cat "$T/p2"); info "$out"
[ "$(num loud "$out")" -ge 100 ] && ok "its frames had sound throughout ($(num loud "$out") of $(num frames "$out"))" \
    || bad "the speaker-on listener's frames: $out"
wait_for 2 '[ "$(lis listeners)" = 0 ] && [ "$(lis out_peak)" -gt 1000 ]' || bad "after the second listener: no sound back"
sleep 1

# ---------------------------------------------------------------- two listeners, one asking for silence
probe 5000 --spk > "$T/p3" &
P3=$!
sleep 0.8
probe 2000 > "$T/p4" &
P4=$!
sleep 1
[ "$(lis listeners)" = 2 ] && [ "$(lis muted)" = 1 ] && [ "$(lis out_peak)" = 0 ] \
    && ok "two listeners, one asking for silence: the Move silent" \
    || bad "two listeners: listeners '$(lis listeners)' muted '$(lis muted)' out_peak '$(lis out_peak)'"
wait $P4
wait_for 2 '[ "$(lis listeners)" = 1 ] && [ "$(lis muted)" = 0 ] && [ "$(lis out_peak)" -gt 1000 ]' \
    && ok "that one gone: the Move plays again, the other still listening" \
    || bad "one left: listeners '$(lis listeners)' muted '$(lis muted)' out_peak '$(lis out_peak)'"
wait $P3; info "$(cat "$T/p3")"
grep -q "listen: 2 listeners, the Move silent" "$L" && grep -q "listen: 1 listener, the Move playing too" "$L" \
    && ok "logged: 2 listeners, the Move silent; 1 listener, the Move playing too" || bad "the two listeners' log lines"
sleep 1

# ---------------------------------------------------------------- a listener that stops answering
t0=$(date +%s.%N)
probe 6500 --quiet-after 1000 > "$T/p5" &
P5=$!
sleep 1.5
[ "$(lis muted)" = 1 ] || bad "the quiet listener: not muted at first ('$(lis muted)')"
wait_for 5 '[ "$(lis listeners)" = 0 ] && [ "$(lis out_peak)" -gt 1000 ]'
t1=$(date +%s.%N); dt=$(echo "$t0 $t1" | awk '{printf "%.1f", $2 - $1}')
[ "$(lis listeners)" = 0 ] && [ "$(lis muted)" = 0 ] && grep -q "listen: a listener quiet for" "$L" \
    && ok "a listener quiet after 1 s (no heartbeat, no reads): closed by the host, the sound back ${dt} s after it came" \
    || bad "the quiet listener: listeners '$(lis listeners)' muted '$(lis muted)' after $dt s"
wait $P5; out=$(cat "$T/p5"); info "$out"
[ "$(num closed "$out")" = 1 ] && ok "the quiet listener's connection: closed by the host" || bad "not closed: $out"
sleep 1

# ---------------------------------------------------------------- a take recorded while the Move is silent
rm -f "$T/play"; wait $PLAYER
cmd "note 84 0" "note 88 0"
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"   # Track 1 held + pad 3: a note layout (as tests/rec_e2e.sh)
sleep 1.3
probe 9000 > "$T/p6" &
P6=$!
press 118; sleep 0.5                                # Sample: the sampler app
cmd "cc 43 127"; sleep 0.3; press 118; sleep 0.1; cmd "cc 43 0"; sleep 0.3   # Track 1 held + Sample: slot 1
set_ 1 2                                            # Mode: Manual
cmd "note 68 110"; sleep 0.2
press 118
wait_for 3 '[ "$(field rec_state)" = 2 ]' || bad "the take did not start (rec_state '$(field rec_state)')"
sleep 0.5
[ "$(lis muted)" = 1 ] && [ "$(lis out_peak)" = 0 ] || bad "recording: the Move not silent (muted '$(lis muted)')"
sleep 1; cmd "note 68 0"; sleep 0.3
press 118
wait_for 5 '[ "$(field rec_takes)" = 1 ]'
F=$(ls -t "$LIB"/*/*.wav 2>/dev/null | head -1)
p=$(build/tools/ainchk info "$F" 2>/dev/null | sed -n 's/.* peak \([0-9]*\)$/\1/p')
[ -n "$F" ] && [ "${p:-0}" -gt 1000 ] && ok "a take recorded while a listener had the Move silent: peak $p ($(basename "$F"))" \
    || bad "the take while listening: '$F' peak '$p'"
wait $P6; info "$(cat "$T/p6")"
cmd "note 68 110"; sleep 2.5; cmd "note 68 0"; sleep 0.2   # (the Move's sound after the last listener, for out.raw)

# ---------------------------------------------------------------- what went to the Move against the mix
cmd quit; wait $HOST; RC=$?
[ "$RC" = 0 ] && ok "the host stopped (exit 0)" || bad "the host's exit: $RC"
r=$(build/tools/ainchk listen "$CAP/out.raw" "$CAP/mix.raw"); info "out.raw / mix.raw: $r"
run=$(num run "$r "); oth=$(num other "$r"); rmp=$(num ramp "$r"); aft=$(num after "$r ")
[ "${oth:-1}" = 0 ] && [ "${rmp:-99999}" -le 4096 ] && [ "${run:-0}" -ge $((44100 * 3)) ] && [ "${aft:-0}" -gt 44100 ] \
    && ok "out.raw: the mix's frames, zero, or a ramp between ($rmp frames); a silenced run of $(echo "$run" | awk '{printf "%.1f", $1 / 44100}') s, the mix's sound after it" \
    || bad "out.raw against mix.raw: $r"
mp=$(build/tools/ainchk rawpeak "$CAP/mix.raw"); info "mix.raw: $mp"
cp "$L" build/test-out/listen_host.log
echo "listen_e2e: $checks checks, $fail failed (the host's log: build/test-out/listen_host.log)"
[ $fail = 0 ]
