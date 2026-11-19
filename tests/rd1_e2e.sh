#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Readouts (rd1: FM1VST docs/RFM1.md "Readouts (rd1)", README "Readouts"), in the build image, after make with an rd1
# build of ChoralRoot among the development builds (make DEV_FIRMWARES=.../choralroot-<v>.rfm1: build/fw-dev): the host
# on the simulator with it in slot 1; the Move's knob 5 (the FM-1's K1) turned one detent: the readout reaches the page
# (/api/info "readout": ChoralRoot's "Voicing" and its value) and the Move's screen (rfm1cs 0.5.7+: the knob's popup,
# the name then the value, full screen by default since rfm1cs 0.5.9); then PRESETS turned:
# the sound's name; the slot switched: the readout forgotten. Then FIZZ (when among them): its labels in rfm1cs are
# its own (not all the last one read), MASTER's VOLUME, K1's readout names K1's job.
#   tests/rd1_e2e.sh
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14rd.XXXXXX)
fail=0; checks=0
ok() { checks=$((checks + 1)); echo "  ok    $*"; }
bad() { checks=$((checks + 1)); fail=$((fail + 1)); echo "  FAIL  $*"; }
echo "rd1_e2e"
CR=$(ls build/fw-dev/choralroot-*.rfm1m 2>/dev/null | sort -V | tail -1)
if [ -z "$CR" ] || ! nm -D "$CR" | grep -q rfm1m_rd1; then echo "  skip  no rd1 build of ChoralRoot in build/fw-dev"; exit 0; fi
# the development build over the catalog's ChoralRoot (README "Development builds"): the module's firmwares/ with the
# catalog's (dist/move-catalog, when there), its firmwares-dev/ with ours: the same <id>-<version> replaced, or a
# newer one ("0.42-r2" over "0.42": the older swept at the start)
FWS=$T/fws; DEV=$T/dev; mkdir -p "$FWS" "$DEV"
cp "${CR%.rfm1m}".* "$DEV/"
FZ=$(ls build/fw-dev/fizz-*.rfm1m 2>/dev/null | sort -V | tail -1)
[ -n "$FZ" ] && cp "${FZ%.rfm1m}".* "$DEV/"
cp build/fw/felucca-1.8.2.* "$FWS/"
CAT=$(ls dist/move-catalog/choralroot-[0-9]*.rfm1m 2>/dev/null | sort -V | tail -1)
CAT=${CAT%.rfm1m}
if [ -n "$CAT" ]; then cp "$CAT".* "$FWS/"; fi
sha_of() { sed -n 's/^ *"sha256": "\([0-9a-f]*\)".*/\1/p' "$1" | head -1; }
SIM=$T/sim; R=$T/root; mkdir -p "$SIM" "$R"
echo "http://127.0.0.1:1" > "$R/catalog-url"
PORT=17797; U="http://127.0.0.1:$PORT"
status() { curl -s "$U/api/status"; }
info() { curl -s "$U/api/info"; }
field() { status | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
rdf() { info | grep -o '"readout":{[^}]*}' | tr ',{}' '\n\n\n' | sed -n "s/^\"$1\":\"*\([^\"]*\)\"*$/\1/p" | head -1; }
wait_for() { _n=$(( $1 * 10 )); shift; while [ $_n -gt 0 ]; do eval "$1" && return 0; sleep 0.1; _n=$((_n - 1)); done; return 1; }
cmd() { printf '%s\n' "$@" > "$SIM/mr-cmd"; }
load() { curl -s -o /dev/null -X POST --data "{\"slot\":$1,\"id\":\"$2\",\"activate\":$3}" "$U/api/load"; }
# lit pixels in rows y0..y1 of the whole screen (rfm1cs 0.5.9: popups full screen by default)
lit_all() { tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | sed -n "$(($1 + 1)),$(($2 + 1))p" | tr -cd 1 | wc -c; }
# the left half's lit pixels in rows y0..y1 of the simulator's screen shot (P1 PBM, 128 x 64)
lit_left() { tail -n +3 "$SIM/mr-screen.pbm" | tr -cd 01 | fold -w 128 | sed -n "$(($1 + 1)),$(($2 + 1))p" | cut -c1-64 | tr -cd 1 | wc -c; }
shot() { rm -f "$SIM/mr-screen.pbm"; cmd shot; wait_for 3 '[ -s "$SIM/mr-screen.pbm" ]'; }
build/sim/remotefm1-host --root "$R" --port $PORT --sim-dir "$SIM" --firmwares "$FWS" --site build/site \
    --dev-firmwares "$DEV" --fw choralroot --test-api --no-fm1 >"$T/host.log" 2>&1 &
HOST=$!
wait_for 10 '[ -p "$SIM/mr-cmd" ] && grep -q "host=RemoteFM1 on Move" "$SIM/mr-state" 2>/dev/null' \
    && wait_for 10 '[ "$(field session)" = 2 ] && grep -q "drew=1" "$SIM/mr-state"' || { bad "the host did not start"; tail -20 "$T/host.log"; exit 1; }
want=$(sha_of "${CR%.rfm1m}.json"); got=$(sha_of "$R/firmwares/$(basename "${CR%.rfm1m}").json")
if [ -z "$CAT" ]; then how=""
elif [ "$(basename "$CAT")" = "$(basename "${CR%.rfm1m}")" ]; then how=" over the catalog's same version ($(sha_of "$CAT.json" | cut -c1-12))"
elif [ ! -f "$R/firmwares/$(basename "$CAT").json" ]; then how=", newer than the catalog's $(basename "$CAT") (swept)"
else how=" BESIDE $(basename "$CAT") (not swept)"; fi
[ -n "$want" ] && [ "$got" = "$want" ] && [ "${how#* BESIDE}" = "$how" ] \
    && ok "the development build $(basename "${CR%.rfm1m}") installed$how ($(echo "$want" | cut -c1-12))" \
    || bad "the installed $(basename "$CR"): '$got', not the development build's $want$how"
sleep 2.5   # (the firmware's splash and the session's own popups gone)
mkdir -p build/test-out
shot; cp "$SIM/mr-screen.pbm" build/test-out/rd1_before.pbm
# the Move's knob 5 is the FM-1's K1: two ticks a detent (RFM_MOVE_KNOB_TICKS), clockwise
cmd "note 4 127"; sleep 0.05; cmd "cc 75 1"; sleep 0.05; cmd "cc 75 1"
wait_for 3 '[ "$(rdf title)" = Voicing ]' && ok "K1 turned: the page's readout \"$(rdf title)\" \"$(rdf value)\" (control $(rdf control), bar $(rdf frac))" \
    || bad "no readout on the page: $(info | grep -o '"readout":{[^}]*}')"
sleep 0.15; shot; after=$(lit_all 0 17); val=$(lit_all 20 52)
# (the popup: the title at 2x in the top rows, the value big under it (full screen, 0.5.9); build/test-out/rd1_k1.pbm)
[ "$after" -gt 60 ] && [ "$val" -gt 150 ] && ! cmp -s build/test-out/rd1_before.pbm "$SIM/mr-screen.pbm" \
    && ok "the Move's screen: the knob's popup, full screen (title rows $after, value rows $val pixels lit)" \
    || bad "the Move's screen: title rows $after, value rows $val pixels lit"
cp "$SIM/mr-screen.pbm" build/test-out/rd1_k1.pbm   # (the shots kept: build/test-out/rd1_*.pbm)
cmd "note 4 0"; sleep 1.5
# PRESETS (Move knob 1): the sound's name (a PRESET readout: ChoralRoot's engine and sound)
seq0=$(rdf seq)
cmd "note 0 127"; sleep 0.05; cmd "cc 71 1"; sleep 0.05; cmd "cc 71 1"
wait_for 3 '[ "$(rdf seq)" != "$seq0" ] && [ "$(rdf kind)" = 2 ]' && ok "PRESETS turned: \"$(rdf title)\" \"$(rdf value)\" (a sound's name)" \
    || bad "PRESETS: $(info | grep -o '"readout":{[^}]*}')"
cmd "note 0 0"; sleep 0.3
# a slot switch: the readout forgotten (the page's goes with it)
load 1 felucca 1 >/dev/null
wait_for 15 'info | grep -q "\"id\":\"felucca\""' && sleep 0.5
info | grep -q '"readout"' && bad "the readout outlived the slot switch" || ok "a slot switch: no readout until the firmware gives one"
# FIZZ (an rd1 build among the development builds): its 49 live labels as rfm1cs holds them are the firmware's, each
# its own (the owner's 2026-11-19: every knob and MASTER showed "SEL/KEYS", label 48, the last one read); MASTER is
# VOLUME; K1's readout names what K1 controls
lab() { info | grep -o "\"$1\":\[[^]]*\]" | sed "s/^\"$1\"://"; }
if [ -n "$FZ" ]; then
    load 1 fizz 1 >/dev/null
    wait_for 20 'info | grep -q "\"id\":\"fizz\""'
    sleep 3; cmd "note 68 100"; sleep 0.2; cmd "note 68 0"; sleep 1.5   # (a pad wakes FIZZ's idle screen)
    L=$(lab labels); M=$(lab move_labels)
    [ -n "$L" ] && [ "$L" = "$M" ] && ok "FIZZ: rfm1cs holds the firmware's 49 labels ($(echo "$L" | cut -c1-60)..)" \
        || bad "FIZZ: rfm1cs's labels $(echo "$M" | cut -c1-90) are not the firmware's $(echo "$L" | cut -c1-90)"
    k=$(echo "$M" | tr -d '[]' | cut -d, -f8)
    [ "$k" = '"VOLUME"' ] && ok "FIZZ: MASTER's label is VOLUME" || bad "FIZZ: MASTER's label is $k"
    k1=$(echo "$L" | tr -d '[]' | cut -d, -f4 | tr -d '"')
    seq0=$(rdf seq)
    cmd "note 4 127"; sleep 0.05; cmd "cc 75 1"; sleep 0.05; cmd "cc 75 1"
    wait_for 3 '[ "$(rdf seq)" != "$seq0" ]' && [ "$(rdf title)" = "$k1" ] && ok "FIZZ: K1's readout \"$(rdf title)\" \"$(rdf value)\" names what K1 controls" \
        || bad "FIZZ: K1's readout '$(rdf title)', its label '$k1'"
    cmd "note 4 0"; sleep 0.3
fi
printf 'quit\n' > "$SIM/mr-cmd"; wait $HOST
mv "$T" "$T.done" 2>/dev/null
echo "rd1_e2e: $checks checks, $fail failed"
exit $fail
