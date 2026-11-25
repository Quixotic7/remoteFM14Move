#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The sampler's tests alone (make test-sampler, after make): a few minutes where make test takes twenty. For work on
# the recorder, the take library, the sampler app and the page's Sampler window; make test stays the whole proof
# before anything is installed or released.
#   on the Mac:    tests/recorder_test.sh, tests/takes_test.sh, tests/kit_test.sh (plain, ASan + UBSan, TSan)
#   in the image:  the unit tests, tests/rec_e2e.sh, tests/takes_e2e.sh, tests/files_e2e.sh (the takes' link),
#                  tests/kit_e2e.sh (the kit, rfm1cs stage 3a)
#   the page:      node tests/page_e2e.mjs --only=sampler (the Sampler window on the desktop and on the phone)
# The core's own (rfm1cs-abletonmove: ctest -R move_sampler) run in its tree.
set -u
cd "$(dirname "$0")/.."
fail=0
t0=$(date +%s)
echo "==== the recorder and the take library (on the Mac)"
tests/recorder_test.sh >build/sampler-recorder.log 2>&1 && tail -1 build/sampler-recorder.log \
    || { cat build/sampler-recorder.log; fail=1; }
tests/takes_test.sh >build/sampler-takes.log 2>&1 && tail -1 build/sampler-takes.log \
    || { cat build/sampler-takes.log; fail=1; }
tests/kit_test.sh >build/sampler-kit.log 2>&1 && tail -1 build/sampler-kit.log \
    || { cat build/sampler-kit.log; fail=1; }
echo "==== in the image"
docker run --rm -v "$(pwd)":/w -w /w "${IMAGE:-rfm14move-build}" sh -c \
    'f=0; build/tools/unit || f=1; tests/rec_e2e.sh || f=1; tests/takes_e2e.sh || f=1; tests/files_e2e.sh || f=1;
     tests/kit_e2e.sh || f=1; exit $f' \
    || fail=1
echo "==== the page's Sampler window in headless Chromium (on the Mac; the host in the image)"
node tests/page_e2e.mjs --only=sampler || fail=1
echo
echo "sampler suite: $(( $(date +%s) - t0 )) s"
[ $fail = 0 ] && echo "ALL SAMPLER TESTS PASSED" || echo "SAMPLER TESTS FAILED"
exit $fail
