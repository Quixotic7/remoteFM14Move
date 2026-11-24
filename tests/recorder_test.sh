#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The recorder (src/recorder.c), natively: tests/recorder_test.c built three ways into build/recorder-test/ and run
# each: plain, with AddressSanitizer + UndefinedBehaviorSanitizer, with ThreadSanitizer. Every run ends with
# ALL RECORDER TESTS PASSED, or this exits non-zero.
#   tests/recorder_test.sh      (no make needed: cc, libc, pthreads)
set -u
cd "$(dirname "$0")/.."
CC=${CC:-cc}
B=build/recorder-test
mkdir -p "$B"
SRC="tests/recorder_test.c src/recorder.c src/wav.c"
FLAGS="-std=c99 -Wall -Wextra -O2 -g"
fail=0
run() {   # name, extra flags
    echo "== $1"
    if ! $CC $FLAGS $2 -o "$B/recorder_test-$1" $SRC -lpthread -lm; then
        echo "FAIL  $1: does not build"; fail=1; return
    fi
    if ! RECORDER_TEST_DIR="$B" ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            TSAN_OPTIONS=halt_on_error=1 "$B/recorder_test-$1"; then
        echo "FAIL  $1"; fail=1
    fi
}
run plain ""
run asan-ubsan "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined"
run tsan "-fsanitize=thread"
if [ $fail -ne 0 ]; then echo "recorder_test: FAILED"; exit 1; fi
echo "recorder_test: plain, ASan + UBSan, TSan: ALL RECORDER TESTS PASSED"
