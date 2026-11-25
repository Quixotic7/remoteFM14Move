#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The kit (src/kit.c), natively: tests/kit_test.c (with src/takes.c) built three ways into build/kit-test/ and run
# each: plain, with AddressSanitizer + UndefinedBehaviorSanitizer, with ThreadSanitizer. Every run ends with
# ALL KIT TESTS PASSED, or this exits non-zero.
#   tests/kit_test.sh      (no make needed: cc, libc, pthreads)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
B=build/kit-test
mkdir -p "$B"
SRC="tests/kit_test.c src/kit.c src/takes.c src/minijson.c"
FLAGS="-std=gnu11 -Wall -Wextra -O2 -g"   # (gnu11 as the Makefile: gcc on Linux wants it for the POSIX calls)
fail=0
run() {   # name, extra flags
    echo "== $1"
    if ! $CC $FLAGS $2 -o "$B/kit_test-$1" $SRC -lpthread -lm; then
        echo "FAIL  $1: does not build"; fail=1; return
    fi
    if ! KIT_TEST_DIR="$B" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            TSAN_OPTIONS=halt_on_error=1 "$B/kit_test-$1"; then
        echo "FAIL  $1"; fail=1
    fi
}
run plain ""
run asan-ubsan "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
run tsan "-fsanitize=thread"
if [ $fail -ne 0 ]; then echo "kit_test: FAILED"; exit 1; fi
echo "kit_test: plain, ASan + UBSan, TSan: ALL KIT TESTS PASSED"
