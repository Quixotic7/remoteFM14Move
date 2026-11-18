#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Each compiled firmware (build/fw/*.rfm1m) against its .rfm1 in wasm3: FM1VST's rfm1_run (origin/rfm1-manager, built
# here as build/ref/rfm1_run) and tools/fwrun (the host's own runtime, src/fwrt.c) make the same default run (a fresh
# flash, init, 4 s idle, D4 F#4 A4 for 5 s, 200 ms more); the whole run's PCM must be the same bytes.
set -u
cd "$(dirname "$0")/.."
T=$(mktemp -d /tmp/rfm14bit.XXXXXX)
fail=0; n=0
echo "bitident"
for so in build/fw/*.rfm1m; do
    base=$(basename "$so" .rfm1m)
    in=build/in/$base.rfm1
    [ -f "$in" ] || { echo "  skip  $base: no $in"; continue; }
    n=$((n + 1))
    ref=$(build/ref/rfm1_run --wav "$T/$base.wav" "$in" 2>/dev/null | head -1)
    got=$(build/tools/fwrun "$so" --pcm "$T/$base.pcm" 2>/dev/null)
    a=$(tail -c +45 "$T/$base.wav" | md5sum | cut -d' ' -f1)
    b=$(md5sum < "$T/$base.pcm" | cut -d' ' -f1)
    if [ -s "$T/$base.pcm" ] && [ "$a" = "$b" ]; then
        echo "  ok    $base: the same PCM as wasm3 ($a, $(( $(stat -c %s "$T/$base.pcm") / 4 )) frames)"
        echo "        wasm3:  ${ref#*: }"
        echo "        wasm2c: ${got#*: }"
    else
        echo "  FAIL  $base: wasm3 $a, wasm2c $b"; echo "        $ref"; echo "        $got"; fail=$((fail + 1))
    fi
done
[ $n -gt 0 ] || { echo "  FAIL  no firmwares to compare"; fail=1; }
mv "$T" "$T.done" 2>/dev/null
echo "bitident: $n firmware(s), $fail failed"
exit $fail
