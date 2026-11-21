#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Session view on the simulator (rfm1cs-abletonmove 0.9.2; the owner's report of 2026-11-21: "Scenes don't launch,
# clicking a clip won't show as selected, the screen and the pads don't seem aligned. I cannot cut and copy clips in
# the same way you can cut and copy steps"), in the build image after make. The host on the simulator with Felucca
# in slot 1: Track 1 held + pad 3 (the Scale layout: the sequencer), two steps, Menu: session view. A clip pressed
# is the selected one on the pad (white) and on the screen (framed); playing, a pressed clip is framed at once and
# plays within a bar; Shift + a clip launches its row; the pads lit are the clips the screen draws; a playing
# session view sends few LED writes (the pulse in steps); Delete held: a clip cut (an X), the next press pastes;
# Copy held: a clip copied into an empty slot.
#   tests/sess_e2e.sh
set -u
cd "$(dirname "$0")/.."
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "sess_e2e"
T=$(mktemp -d /tmp/rfm14sess.XXXXXX)
FWS=$T/fws; mkdir -p "$FWS"
cp build/fw/felucca-1.8.2.* "$FWS/"
SIM=$T/sim; R=$T/root; mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17798; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
padrgb() { field move_pads | cut -c$(($1 * 6 + 1))-$(($1 * 6 + 6)); }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; sleep 0.06; }
press() { cmd "cc $1 127"; cmd "cc $1 0"; }
hit() { cmd "note $1 100"; sleep 0.08; cmd "note $1 0"; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; SCR=$(tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01); }
px() { printf '%s' "$SCR" | cut -c$(($2 * 128 + $1 + 1)); }              # x y
# the session grid: column c (0..7), row r (0 the top): its cell 14 x 13 at (c * 16 + 1, 4 + r * 15)
clip() { [ "$(px $(($1 * 16 + 1)) $((4 + $2 * 15 + 6)))" = 1 ] || [ "$(px $(($1 * 16 + 2)) $((4 + $2 * 15 + 6)))" = 1 ]; }
framed() { [ "$(px $(($1 * 16)) $((3 + $2 * 15)))" = 1 ] && [ "$(px $(($1 * 16 + 15)) $((3 + $2 * 15 + 14)))" = 1 ]; }
pad() { echo $(( (3 - $2) * 8 + $1 )); }                                 # c r: the pad's index (its note: 68 + it)
note() { echo $(( 68 + (3 - $2) * 8 + $1 )); }
white() { _c=$(padrgb "$1"); [ "$(echo "$_c" | cut -c1-2)" = "$(echo "$_c" | cut -c3-4)" ] && [ "$(echo "$_c" | cut -c3-4)" = "$(echo "$_c" | cut -c5-6)" ] && [ "$_c" != 000000 ]; }
build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site --fw felucca \
    --test-api --no-fm1 >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 15 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
sleep 2.5   # (the firmware's splash and the session's own popups gone)
mkdir -p build/test-out
# ---- the way in: Track 1 held + pad 3 (Scale): the sequencer; two steps; Menu
cmd "cc 43 127"; sleep 0.4; hit 94; cmd "cc 43 0"
wait_for 4 '[ "$(field move_mode)" = 3 ]' && ok "Track 1 held + pad 3: the Scale layout (the sequencer, mode 3)" || bad "mode '$(field move_mode)'"
sleep 1.3; hit 16; hit 20; sleep 0.3
press 50; sleep 1.3
shot; cp "$SIM/mr-screen.pbm" build/test-out/sess_rest.pbm
clip 0 0 && framed 0 0 && ! clip 1 0 && ! framed 1 0 && wait_for 3 'white $(pad 0 0)' \
    && ok "Menu: session view; track 1's clip is the selected one (its pad white, its cell framed)" \
    || bad "session at rest: pad '$(padrgb $(pad 0 0))' (build/test-out/sess_rest.pbm)"
# ---- stopped: a slot pressed: a clip there, selected on the pad and on the screen
hit $(note 2 1); sleep 0.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/sess_selected.pbm
clip 2 1 && framed 2 1 && ! framed 0 0 && [ "$(px 33 0)" = 1 ] && [ "$(px 1 0)" = 0 ] && wait_for 3 'white $(pad 2 1)' && ! white $(pad 0 0) \
    && ok "column 3 row 2 pressed: its pad white, its cell framed, the bar over column 3; track 1's clip no longer" \
    || bad "a clip pressed: pad '$(padrgb $(pad 2 1))', the old one '$(padrgb $(pad 0 0))' (build/test-out/sess_selected.pbm)"
# ---- playing: a pressed clip is framed at once and plays within a bar
press 85
wait_for 3 '[ "$(field seq_playing)" = 1 ]' && ok "Play: the sequencer plays" || bad "Play: seq_playing '$(field seq_playing)'"
hit $(note 0 1); sleep 0.15; shot; cp "$SIM/mr-screen.pbm" build/test-out/sess_queued.pbm
framed 0 1 && ! framed 2 1 && ok "playing, column 1 row 2 pressed: framed on the screen at once" \
    || bad "the pressed clip is not framed while it waits (build/test-out/sess_queued.pbm)"
wait_for 5 'white $(pad 0 1)' && ! white $(pad 0 0) && [ "$(padrgb $(pad 0 0))" != 000000 ] \
    && ok "within a bar it plays: its pad white ($(padrgb $(pad 0 1))), the clip it replaced in its track's colour" \
    || bad "the pressed clip did not play: pad '$(padrgb $(pad 0 1))', the old one '$(padrgb $(pad 0 0))'"
# ---- a scene: Shift + a clip of row 1: track 1's first clip plays again, track 3 (no clip in that row) stops
cmd "cc 49 127"; sleep 0.3; hit $(note 0 0); sleep 0.2; cmd "cc 49 0"
wait_for 5 'white $(pad 0 0)' && ! white $(pad 0 1) && ok "Shift + a clip of row 1: the scene launches (track 1's clip 1 plays: $(padrgb $(pad 0 0)))" \
    || bad "the scene: pad row 1 '$(padrgb $(pad 0 0))', row 2 '$(padrgb $(pad 0 1))'"
# ---- a playing session view's LED writes (the pulse in 6 steps a beat; a new level every tick before 0.9.2)
sleep 1.2; a=$(field led_writes); sleep 4; b=$(field led_writes)
[ $(( (b - a) / 4 )) -lt 60 ] && [ "$b" -gt "$a" ] && ok "playing: $(( (b - a) / 4 )) LED writes a second" || bad "playing: $(( (b - a) / 4 )) LED writes a second"
press 85
wait_for 3 '[ "$(field seq_playing)" = 0 ]' || bad "Play again: seq_playing '$(field seq_playing)'"
# ---- the screen and the pads: a pad is lit where the screen draws a clip (or the selected slot), over the 8 x 4
sleep 1.2; shot; p=$(field move_pads); mis=""
for r in 0 1 2 3; do for c in 0 1 2 3 4 5 6 7; do
    i=$(pad $c $r); v=$(echo "$p" | cut -c$((i * 6 + 1))-$((i * 6 + 6)))
    if clip $c $r || framed $c $r; then [ "$v" != 000000 ] || mis="$mis c$((c + 1))r$((r + 1))"
    else [ "$v" = 000000 ] || mis="$mis c$((c + 1))r$((r + 1))"; fi
done; done
[ -z "$mis" ] && clip 0 0 && clip 0 1 && clip 2 1 && ok "the pads lit are the clips the screen draws (3 clips, 8 x 4)" || bad "the screen and the pads differ at:$mis"
# ---- Delete held: a clip cut (an X in its slot); the press right after a cut pastes
cmd "cc 119 127"; sleep 0.2; hit $(note 2 1); sleep 0.3; shot; cp "$SIM/mr-screen.pbm" build/test-out/sess_cut.pbm
[ "$(px 36 22)" = 1 ] && [ "$(px 37 23)" = 1 ] && ok "Delete held + a clip: cut, an X in its slot" || bad "the cut (build/test-out/sess_cut.pbm)"
hit $(note 5 3); sleep 0.2; cmd "cc 119 0"; sleep 0.4; shot
clip 5 3 && ! clip 2 1 && wait_for 3 '[ "$(padrgb $(pad 5 3))" != 000000 ]' && ok "the next press (Delete still held): pasted into column 6 row 4" \
    || bad "the paste after a cut: pad '$(padrgb $(pad 5 3))'"
# ---- Copy held: a clip copied, an empty slot gets it; the source stays
cmd "cc 60 127"; sleep 0.2; hit $(note 0 0); sleep 0.2; hit $(note 4 2); sleep 0.2; cmd "cc 60 0"; sleep 0.4; shot
cp "$SIM/mr-screen.pbm" build/test-out/sess_copied.pbm
clip 4 2 && clip 0 0 && wait_for 3 '[ "$(padrgb $(pad 4 2))" != 000000 ]' && ok "Copy held + a clip, then an empty slot: copied there, the source stays" \
    || bad "the copy: pad '$(padrgb $(pad 4 2))' (build/test-out/sess_copied.pbm)"
grep -q "trap" "$T/host.log" && bad "the host's log: $(grep -m1 'trap' "$T/host.log")"
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
mv "$T" "$T.done" 2>/dev/null
echo "sess_e2e: $checks checks, $fail failed"
exit $fail
