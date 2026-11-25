#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page's Update's parts, natively (on the Mac, after make: the tests' keys in build/testkey, build/move's host):
#   1. the fixtures (tests/update_fixtures.py: tools/make_update.py with the tests' key) into build/update-test;
#   2. tests/update_test.c (src/update.c, src/gz.c) built plain and with AddressSanitizer + UBSan, and run each.
# Every run ends with ALL UPDATE TESTS PASSED, or this exits non-zero. tests/update_e2e.sh uses the same fixtures.
#   tests/update_test.sh
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
B=build/update-test
for f in build/testkey/update-test-key.pem build/testkey/update_test_key.h build/testkey/test-key.pem \
         build/testkey/catalog_test_key.h build/move/remotefm1-host build/site/index.html; do
    [ -f "$f" ] || { echo "update_test: $f is missing: run make"; exit 1; }
done
mkdir -p "$B"
python3 tests/update_fixtures.py "$B" --key build/testkey/update-test-key.pem --catkey build/testkey/test-key.pem \
    || { echo "update_test: FAILED (the fixtures)"; exit 1; }
SRC="tests/update_test.c src/update.c src/gz.c src/sha256.c src/ed25519.c src/minijson.c"
FLAGS="-std=gnu11 -Wall -Wextra -O2 -g -Isrc -Ibuild/testkey -DRFM14_TEST_BUILD"
fail=0
run() {   # name, extra flags
    echo "== $1"
    if ! $CC $FLAGS $2 -o "$B/update_test-$1" $SRC -lpthread; then
        echo "FAIL  $1: does not build"; fail=1; return
    fi
    if ! UPD_FIX="$B" UPD_WORK="$B/work-$1" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$B/update_test-$1"; then
        echo "FAIL  $1"; fail=1
    fi
}
run plain ""
run asan-ubsan "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
if [ $fail -ne 0 ]; then echo "update_test: FAILED"; exit 1; fi
echo "update_test: plain, ASan + UBSan: ALL UPDATE TESTS PASSED"
