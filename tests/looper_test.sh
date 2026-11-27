#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The looper (src/looper.c), natively: tests/looper_test.c (with src/wav.c) built three ways into build/looper-test/
# and run each: plain, with AddressSanitizer + UndefinedBehaviorSanitizer, with ThreadSanitizer; every allocation
# looper.c makes is counted by thread (tests/looper_alloc.h: none on the engine's). Every run ends with ALL LOOPER
# TESTS PASSED, or this exits non-zero.
#   tests/looper_test.sh      (no make needed: cc, libc, pthreads)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
B=build/looper-test
mkdir -p "$B"
SRC="tests/looper_test.c src/looper.c src/wav.c"
FLAGS="-std=gnu11 -Wall -Wextra -O2 -g -DLOOPER_TEST_ALLOC"
fail=0
run() {   # name, extra flags
    echo "== $1"
    if ! $CC $FLAGS $2 -o "$B/looper_test-$1" $SRC -lpthread -lm; then
        echo "FAIL  $1: does not build"; fail=1; return
    fi
    if ! LOOPER_TEST_DIR="$B" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            TSAN_OPTIONS=halt_on_error=1 "$B/looper_test-$1"; then
        echo "FAIL  $1"; fail=1
    fi
}
run plain ""
run asan-ubsan "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
run tsan "-fsanitize=thread"
if [ $fail -ne 0 ]; then echo "looper_test: FAILED"; exit 1; fi
echo "looper_test: plain, ASan + UBSan, TSan: ALL LOOPER TESTS PASSED"
