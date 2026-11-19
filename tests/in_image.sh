#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The tests that need Linux (the build image, linux/arm64): unit, the firmwares against wasm3, the glibc the Move's
# binaries need, the host on the simulator, the catalog's downloads (tests/dl_e2e.sh).
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
file build/move/remotefm1-heal | grep -q "statically linked" && echo "  ok    remotefm1-heal: static" \
    || { echo "  FAIL  remotefm1-heal is not static"; fail=1; }
tests/sim_e2e.sh || fail=1
tests/dl_e2e.sh || fail=1
exit $fail
