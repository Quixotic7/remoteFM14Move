#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The take library (src/takes.c), natively: tests/takes_test.c built three ways into build/takes-test/ and run each:
# plain, with AddressSanitizer + UndefinedBehaviorSanitizer, with ThreadSanitizer. Every run ends with
# ALL TAKES TESTS PASSED, or this exits non-zero.
#   tests/takes_test.sh      (no make needed: cc, libc, pthreads)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
B=build/takes-test
mkdir -p "$B"
SRC="tests/takes_test.c src/takes.c"
FLAGS="-std=gnu11 -Wall -Wextra -O2 -g"
fail=0
run() {   # name, extra flags
    echo "== $1"
    if ! $CC $FLAGS $2 -o "$B/takes_test-$1" $SRC -lpthread; then
        echo "FAIL  $1: does not build"; fail=1; return
    fi
    if ! TAKES_TEST_DIR="$PWD/$B" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            TSAN_OPTIONS=halt_on_error=1 "$B/takes_test-$1"; then
        echo "FAIL  $1"; fail=1
    fi
}
run plain ""
run asan-ubsan "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
run tsan "-fsanitize=thread"
if [ $fail -ne 0 ]; then echo "takes_test: FAILED"; exit 1; fi
echo "takes_test: plain, ASan + UBSan, TSan: ALL TAKES TESTS PASSED"
