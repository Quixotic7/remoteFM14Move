#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Every test (make test runs this after make): Move Remote's own on its branch (on the Mac: its simulator, mrctl,
# the page), then the rest in the build image (tests/in_image.sh).
set -u
cd "$(dirname "$0")/.."
fail=0
echo "==== Move Remote (third_party/abletonmoveremote, branch $(git -C third_party/abletonmoveremote rev-parse --abbrev-ref HEAD 2>/dev/null || echo '?'))"
make -C third_party/abletonmoveremote test >build/moveremote-test.log 2>&1 && tail -1 build/moveremote-test.log \
    || { cat build/moveremote-test.log; fail=1; }
echo "==== in the image"
docker run --rm -v "$(pwd)":/w -w /w "${IMAGE:-rfm14move-build}" tests/in_image.sh || fail=1
echo
[ $fail = 0 ] && echo "ALL TESTS PASSED" || echo "TESTS FAILED"
exit $fail
