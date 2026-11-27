#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The tests that need Linux (the build image, linux/arm64): unit, the firmwares against wasm3, the glibc the Move's
# binaries need, the host on the simulator, the slots and Schwung FX (tests/fx_e2e.sh), the catalog's downloads (tests/dl_e2e.sh), the FM-1 on USB-A
# (tests/fm1_e2e.sh) and over Wi-Fi (tests/fm1wifi_e2e.sh, after make wifisim), its Wi-Fi set up from the page
# (tests/fm1wifisetup_e2e.sh), Fantasy CFWs: Elasticat and Ripple with the RemoteFM1 folder (tests/fantasy_e2e.sh), and the
# Move's audio input for them (ain1: tests/ain_e2e.sh), the page's file browser (tests/files_e2e.sh),
# latch per slot (tests/latch_e2e.sh), the slots' render threads and core 1 (tests/threads_e2e.sh; FIZZ's and
# ChoralRoot's cpu2 builds when among the inputs: tests/cpu2fw_e2e.sh), readouts (an rd1 ChoralRoot among
# them: tests/rd1_e2e.sh), session view (tests/sess_e2e.sh), projects that hold their firmwares and the States
# (tests/projfw_e2e.sh), the States and projects holding the firmware's working memory, played (tests/states_e2e.sh), the page's States and projects through their API, played the same way (tests/store_e2e.sh), a project or a state opened while the Move is stopped silent and stopped (tests/loadstop_e2e.sh), a new random project (tests/projnew_e2e.sh), the takeover (tk1, tests/tkdemo: tests/tk_e2e.sh; ChoralRoot's CHORDS when a tk1 build of it is among them: tests/cr_tk_e2e.sh), engine parameters (ep1: an ep1 build of ChoralRoot among them, the ENGINE knob page: tests/ep1_e2e.sh), Schwung's screen reader (tests/speech_e2e.sh; RemoteFM1's Speech mode: tests/speechmode_e2e.sh) and the firmwares' own words through it (the
# FM-1 screen reader protocol: tests/fmreader_e2e.sh), Out: network MIDI and USB-A (tests/midiout_e2e.sh), the recorder
# as the sampler app (tests/rec_e2e.sh), the take library as its Takes and Tools tabs (tests/takes_e2e.sh), the kit
# (tests/kit_e2e.sh), the looper (tests/looper_e2e.sh), the power button's Shut Down? (tests/power_e2e.sh), Listen here (tests/listen_e2e.sh), the page's Update (tests/update_e2e.sh).
cd "$(dirname "$0")/.."
fail=0
build/tools/unit || fail=1
tests/bitident.sh || fail=1
echo "glibc (the Move has 2.35)"
for b in build/move/remotefm1-host build/fw/*.rfm1m; do
    v=$(objdump -T "$b" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
    if [ -n "$v" ] && [ "$(printf '%s\nGLIBC_2.35\n' "$v" | sort -V | tail -1)" = GLIBC_2.35 ]; then
        echo "  ok    $(basename "$b"): $v"
    else echo "  FAIL  $(basename "$b"): needs $v"; fail=1; fi
done
# the Move's host: the catalog's real key only (not build/testkey's), no libssl linked (it is dlopen'd)
echo "the Move host's catalog key and TLS"
hexof() { xxd -p "$1" | tr -d '\n'; }
real=$(sed -n '/CAT_SIGNING_KEY\[32\]/,/};/p' src/catalog_key.h | grep -o '0x[0-9a-f][0-9a-f]' | sed 's/0x//' | tr -d '\n')
test=$(grep -o '0x[0-9a-f][0-9a-f]' build/testkey/catalog_test_key.h | sed 's/0x//' | tr -d '\n')
mh=$(hexof build/move/remotefm1-host); sh_=$(hexof build/sim/remotefm1-host)
if [ ${#real} = 64 ] && [ ${#test} = 64 ] && echo "$mh" | grep -q "$real" && ! echo "$mh" | grep -q "$test" \
    && echo "$sh_" | grep -q "$test" && ! strings build/move/remotefm1-host | grep -q -- "--ca-dir\|RFM14_TEST_INDEX_MS\|RFM14_TEST_LIBSSL"; then
    echo "  ok    remotefm1-host (Move): the release key ${real%${real#????????}}.. only; the test key and test options only in the simulator's"
else echo "  FAIL  the Move host's keys (real ${#real}, test ${#test} hex digits) or test options"; fail=1; fi
# the same for the page's Update (src/update_key.h; build/testkey/update_test_key.h)
ureal=$(sed -n '/UPD_SIGNING_KEY\[32\]/,/};/p' src/update_key.h | grep -o '0x[0-9a-f][0-9a-f]' | sed 's/0x//' | tr -d '\n')
utest=$(grep -o '0x[0-9a-f][0-9a-f]' build/testkey/update_test_key.h | sed 's/0x//' | tr -d '\n')
if [ ${#ureal} = 64 ] && [ ${#utest} = 64 ] && echo "$mh" | grep -q "$ureal" && ! echo "$mh" | grep -q "$utest" \
    && echo "$sh_" | grep -q "$utest" && ! strings build/move/remotefm1-host | grep -q "RFM14_TEST_UPDATE_INSIDE"; then
    echo "  ok    remotefm1-host (Move): the update key ${ureal%${ureal#????????}}.. only; the test key only in the simulator's"
else echo "  FAIL  the Move host's update keys (real ${#ureal}, test ${#utest} hex digits) or test options"; fail=1; fi
if objdump -p build/move/remotefm1-host | grep NEEDED | grep -q "libssl\|libcrypto"; then
    echo "  FAIL  remotefm1-host links libssl/libcrypto (it must dlopen them)"; fail=1
else echo "  ok    remotefm1-host does not link libssl or libcrypto (dlopen at the first https fetch)"; fi
file build/move/remotefm1-heal | grep -q "statically linked" && echo "  ok    remotefm1-heal: static" \
    || { echo "  FAIL  remotefm1-heal is not static"; fail=1; }
tests/sim_e2e.sh || fail=1
tests/fx_e2e.sh || fail=1
tests/dl_e2e.sh || fail=1
tests/fm1_e2e.sh || fail=1
tests/fm1plug_e2e.sh || fail=1
tests/fm1wifi_e2e.sh || fail=1
tests/fm1wifisetup_e2e.sh || fail=1
tests/fantasy_e2e.sh || fail=1
tests/ain_e2e.sh || fail=1
tests/files_e2e.sh || fail=1
tests/latch_e2e.sh || fail=1
tests/threads_e2e.sh || fail=1
tests/cpu2fw_e2e.sh || fail=1
tests/rd1_e2e.sh || fail=1
tests/sess_e2e.sh || fail=1
tests/projfw_e2e.sh || fail=1
tests/states_e2e.sh || fail=1
tests/store_e2e.sh || fail=1
tests/loadstop_e2e.sh || fail=1
tests/projnew_e2e.sh || fail=1
tests/tk_e2e.sh || fail=1
tests/cr_tk_e2e.sh || fail=1
tests/ep1_e2e.sh || fail=1
tests/speech_e2e.sh || fail=1
tests/speechmode_e2e.sh || fail=1
tests/fmreader_e2e.sh || fail=1
tests/midiout_e2e.sh || fail=1
tests/mroute_e2e.sh || fail=1
tests/rec_e2e.sh || fail=1
tests/takes_e2e.sh || fail=1
tests/kit_e2e.sh || fail=1
tests/looper_e2e.sh || fail=1
tests/power_e2e.sh || fail=1
tests/listen_e2e.sh || fail=1
if [ -f build/update-test/good.rfm1u ]; then tests/update_e2e.sh || fail=1   # (its fixtures: tests/update_test.sh, on the Mac)
else echo "update_e2e: FAIL  no build/update-test fixtures (tests/update_test.sh)"; fail=1; fi
exit $fail
