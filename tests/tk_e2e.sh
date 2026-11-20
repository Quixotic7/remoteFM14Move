#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The takeover (tk1: FM1VST docs/RFM1-TAKEOVER.md, rfm1cs 0.7.0, README "Takeover"), in the build image after make:
# the host on the simulator with tkdemo (tests/tkdemo, built into build/tkdemo: claims SCREEN PADS STEPS KNOBS JOG
# BUTTONS = 0x5F, Shift toggles ALL = 0xFF; draws "TKDEMO", a rule at y = 9 and the last input at 2x; lights a pressed
# pad 255,120,0) in slot 1 and Felucca (no tk1) beside it. Track 1 opens the takeover (the screen is the firmware's), a
# pad reaches it (its LED lit, its screen changed), unclaimed Track 2 still switches mode and Track 1 comes back to it;
# Shift: ALL claimed, Track 1 then the firmware's; volume touched + Back: FM-1 mode; re-entered; Back + Track 2 (a slot
# switch) leaves it, Back + Track 1 reopens it; Felucca ignores Track 1; unloading or replacing tkdemo clears it.
#   tests/tk_e2e.sh
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14tk.XXXXXX)
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "tk_e2e"
TK=build/tkdemo/tkdemo-1.0.rfm1m
if [ ! -f "$TK" ] || ! nm -D "$TK" | grep -q rfm1m_tk1; then bad "no tk1 build of tkdemo in build/tkdemo (make)"; exit 1; fi
FWS=$T/fws; mkdir -p "$FWS"
cp build/tkdemo/tkdemo-1.0.* "$FWS/"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17798; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
# slot n's record in the status (0-based; "-" when empty)
slotrec() { status | sed 's/.*"slots":\[//; s/\],"threads".*//; s/null/{"id":"-"}/g' | awk -v RS='[{]"id":' -v n="$1" 'NR == n + 2 { print "\"id\":" $0 }'; }
sfield() { slotrec "$1" | tr ',{}' '\n\n\n' | sed -n "s/^\"$2\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
padrgb() { field move_pads | cut -c$(($1 * 6 + 1))-$(($1 * 6 + 6)); }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }   # a button (Track 1 CC 43, Track 2 CC 42, Shift 49, Back 51)
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":$3}" "$U/api/load"; }
edit() { curl -s -o /dev/null -X POST --data "{\"op\":$1,\"a\":$2,\"b\":0}" "$U/api/edit"; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
rows() { tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | sed -n "$(($1 + 1)),$(($2 + 1))p"; }
lit() { rows "$1" "$2" | tr -cd 1 | wc -c; }
# tkdemo's screen: the rule at y = 9 the whole width, "TKDEMO" over it (rows 0..6, columns 0..35)
tkscreen() { [ "$(lit 9 9)" = 128 ] && [ "$(rows 0 6 | cut -c1-36 | tr -cd 1 | wc -c)" -gt 40 ]; }
mode() { field move_mode; }   # rfm_move_mode: 0 FM-1, 1 MIDI, 2 Control, 3 Sequencer, 4 the takeover
build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site \
    --fw tkdemo --test-api --no-fm1 >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 10 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
wait_for 5 '[ "$(sfield 0 tk)" = 95 ]' && [ "$(sfield 0 tk_name)" = TKDEMO ] && [ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ] \
    && ok "tkdemo in slot 1: its record read (claims 0x5F, \"TKDEMO\"), FM-1 mode" \
    || bad "tkdemo's takeover: claims '$(sfield 0 tk)' name '$(sfield 0 tk_name)' mode '$(mode)' shown '$(field tk_shown)'"
sleep 2   # (the session's own popups gone)
mkdir -p build/test-out
shot; cp "$SIM/mr-screen.pbm" build/test-out/tk_fm1.pbm
tkscreen && bad "FM-1 mode shows tkdemo's Move screen already" || ok "FM-1 mode: RemoteFM1's screen"
# ---- Track 1: the takeover
press 43
wait_for 4 '[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' && [ "$(sfield 0 tk_states)" -ge 1 ] \
    && ok "Track 1: the takeover (mode 4, slot 1 shown, rfm1_tk_state called $(sfield 0 tk_states)x)" \
    || bad "Track 1: mode '$(mode)' shown '$(field tk_shown)' states '$(sfield 0 tk_states)'"
sleep 1.6   # (its name popup gone)
shot; cp "$SIM/mr-screen.pbm" build/test-out/tk_ready.pbm
tkscreen && ok "the Move's screen is the firmware's (\"TKDEMO\", its rule: $(lit 0 63) pixels lit)" \
    || bad "the Move's screen is not tkdemo's (row 9: $(lit 9 9) lit; build/test-out/tk_ready.pbm)"
# ---- a pad: to the firmware, its LED and screen follow
in0=$(sfield 0 tk_in)
cmd "note 73 100"   # (pad 5: notes 68..99)
wait_for 4 '[ "$(padrgb 5)" = ff7800 ]' && [ "$(sfield 0 tk_in)" -gt "$in0" ] \
    && ok "pad 5 pressed: rfm1_tk_input ($in0 -> $(sfield 0 tk_in)), its LED the firmware's 255,120,0 ($(padrgb 5))" \
    || bad "pad 5: LED '$(padrgb 5)', tk_in $in0 -> '$(sfield 0 tk_in)'"
sleep 0.2; shot; cp "$SIM/mr-screen.pbm" build/test-out/tk_pad5.pbm
tkscreen && ! cmp -s build/test-out/tk_ready.pbm build/test-out/tk_pad5.pbm \
    && ok "the screen shows the press (\"PAD 5\" over \"READY\")" || bad "the screen did not change with the pad"
cmd "note 73 0"
wait_for 4 '[ "$(padrgb 5)" = 000000 ]' && ok "pad 5 let go: its LED dark (the firmware's)" || bad "pad 5 released: LED '$(padrgb 5)'"
cmd "note 74 100"; cmd "note 74 0"; sleep 0.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/tk_pad6.pbm
cmd "note 73 100"; cmd "note 73 0"; sleep 0.3; shot
! cmp -s build/test-out/tk_pad5.pbm build/test-out/tk_pad6.pbm && cmp -s build/test-out/tk_pad5.pbm "$SIM/mr-screen.pbm" \
    && ok "pad 6 then pad 5 again: \"PAD 6\", then the same picture as the first \"PAD 5\"" \
    || bad "pads 6, 5: the pictures do not follow the input"
# ---- Track 2 is not claimed: MIDI mode; Track 1 back to the takeover
press 42
wait_for 4 '[ "$(mode)" = 1 ] && [ "$(field tk_shown)" = -1 ]' && ok "Track 2 (not claimed): MIDI mode, the takeover hidden" \
    || bad "Track 2: mode '$(mode)' shown '$(field tk_shown)'"
press 43
wait_for 4 '[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' && ok "Track 1 from MIDI mode: back to the takeover the slot was left in" \
    || bad "Track 1 from MIDI mode: mode '$(mode)' shown '$(field tk_shown)'"
# ---- Shift: ALL claimed; Track 1 and Track 2 are the firmware's
press 49
wait_for 4 '[ "$(sfield 0 tk)" = 255 ]' && ok "Shift: the firmware claims ALL (rfm_move_set_takeover with 0xFF)" \
    || bad "Shift: claims '$(sfield 0 tk)'"
in0=$(sfield 0 tk_in)
press 43; press 42
sleep 1.2
[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ] && [ "$(sfield 0 tk_in)" -ge $((in0 + 4)) ] \
    && ok "Track 1, Track 2 under ALL: the firmware's ($in0 -> $(sfield 0 tk_in) inputs), still in the takeover" \
    || bad "Track 1/2 under ALL: mode '$(mode)' shown '$(field tk_shown)' tk_in $in0 -> '$(sfield 0 tk_in)'"
# ---- volume touched + Back: FM-1 mode
in0=$(sfield 0 tk_in)
cmd "note 8 127"; cmd "cc 51 127"; cmd "cc 51 0"; cmd "note 8 0"
wait_for 4 '[ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ]' && ok "volume touched + Back: FM-1 mode (claims ALL)" \
    || bad "volume + Back: mode '$(mode)' shown '$(field tk_shown)'"
sleep 1.6; shot; tkscreen && bad "FM-1 mode after volume + Back shows the firmware's screen" || ok "FM-1 mode's screen again"
# ---- re-enter, ALL dropped again (Shift), then a slot switch: Back + Track 2 leaves it, Back + Track 1 reopens it
press 43
wait_for 4 '[ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' && ok "Track 1: the takeover again" || bad "re-enter: mode '$(mode)'"
press 49
wait_for 4 '[ "$(sfield 0 tk)" = 95 ]' && ok "Shift: claims 0x5F again" || bad "Shift back: claims '$(sfield 0 tk)'"
load 1 felucca 0
wait_for 20 '[ "$(sfield 1 id)" = felucca ]' && ok "Felucca loaded in slot 2 (no tk1)" \
    || bad "Felucca not loaded in slot 2"
[ -z "$(sfield 1 tk)" ] || [ "$(sfield 1 tk)" = 0 ] || bad "Felucca has claims '$(sfield 1 tk)'"
[ "$(mode)" = 4 ] || bad "loading another slot left the takeover: mode '$(mode)'"
cmd "cc 51 127"; press 42; cmd "cc 51 0"   # (Back held back: Back + Track 2 still switches slots)
wait_for 6 '[ "$(field active)" = 1 ] && [ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ]' \
    && ok "Back + Track 2: slot 2 (Felucca), FM-1 mode, the takeover closed" \
    || bad "Back + Track 2: active '$(field active)' mode '$(mode)' shown '$(field tk_shown)'"
in0=$(sfield 0 tk_in)
press 43
sleep 1.2
[ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ] && [ "$(sfield 0 tk_in)" = "$in0" ] \
    && ok "Felucca (no tk1): Track 1 is FM-1 mode as ever" || bad "Felucca, Track 1: mode '$(mode)' shown '$(field tk_shown)'"
cmd "cc 51 127"; press 43; cmd "cc 51 0"
wait_for 6 '[ "$(field active)" = 0 ] && [ "$(mode)" = 4 ] && [ "$(field tk_shown)" = 0 ]' \
    && ok "Back + Track 1: slot 1, its takeover reopened (left open)" \
    || bad "Back + Track 1: active '$(field active)' mode '$(mode)' shown '$(field tk_shown)'"
sleep 1.6; shot; tkscreen && ok "the firmware's screen again" || bad "reopened, the screen is not tkdemo's"
# ---- unloading tkdemo clears the takeover; loaded again, it waits in FM-1 mode for Track 1
edit 17 0
wait_for 6 '[ "$(sfield 0 id)" = - ] && [ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ]' \
    && ok "slot 1 emptied: the takeover gone, FM-1 mode" || bad "unload: slot 1 '$(sfield 0 id)' mode '$(mode)' shown '$(field tk_shown)'"
press 43; sleep 1.2
[ "$(mode)" = 0 ] && ok "empty slot: Track 1 offers no takeover" || bad "empty slot, Track 1: mode '$(mode)'"
load 0 tkdemo 1
wait_for 20 '[ "$(sfield 0 tk)" = 95 ]' && [ "$(mode)" = 0 ] && ok "tkdemo loaded again: its claims, FM-1 mode until Track 1" \
    || bad "tkdemo again: claims '$(sfield 0 tk)' mode '$(mode)'"
press 43
wait_for 4 '[ "$(mode)" = 4 ]' && ok "Track 1: its takeover" || bad "tkdemo again, Track 1: mode '$(mode)'"
# ---- replacing it (Felucca loaded over it while shown) clears it too
load 0 felucca 1
wait_for 20 '[ "$(sfield 0 id)" = felucca ] && [ "$(mode)" = 0 ] && [ "$(field tk_shown)" = -1 ]' \
    && ok "Felucca over tkdemo in slot 1: the takeover gone, FM-1 mode" \
    || bad "replace: slot 1 '$(sfield 0 id)' mode '$(mode)' shown '$(field tk_shown)'"
press 43; sleep 1.2
[ "$(mode)" = 0 ] && ok "Felucca in slot 1: Track 1 ignores the takeover tkdemo had" || bad "after replace, Track 1: mode '$(mode)'"
grep -q "fwrt: .*tk1\|trap" "$T/host.log" && bad "the host's log: $(grep -m1 'tk1\|trap' "$T/host.log")"
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
mv "$T" "$T.done" 2>/dev/null
echo "tk_e2e: $checks checks, $fail failed"
exit $fail
