#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# One catalog .rfm1 built and checked for the Move, in the build image (tools/build_move_catalog.py runs it, several at
# once):   tools/catalog_fw.sh IN.rfm1 OUTROOT
# OUTROOT/<sha256 of IN>/ gets files/ (tools/build_fw.sh's: the .rfm1m, its manifest .json, the licence texts, the
# skin) and result.json: ok or not, and why. The checks:
#   - it builds (wasm2c 1.0.32, gcc), and the object needs no glibc newer than the Move's 2.35;
#   - it boots and sounds: tools/fwrun (the host's own runtime) makes rfm1_run's default run (a fresh flash, init,
#     4 s idle, a chord for 5 s): no trap, no halt, the screen written, sound (or, for one silent on that chord,
#     wasm3 just as silent: noted in the result);
#   - the same run's PCM is wasm3's (FM1VST's rfm1_run on the .rfm1), byte for byte.
# A result already there for the same .rfm1 (its SHA-256), the same build (build_fw.sh, fwglue.c, rfm1m.h) and the
# same checks (this script, the runtime fwrun uses) is kept; with only the checks changed, they run again on the
# object already built.
set -u
IN="$1"
OUTROOT="$2"
cd "$(dirname "$0")/.."
SHA=$(sha256sum "$IN" | cut -d' ' -f1)
D="$OUTROOT/$SHA"
BSTAMP=$(cat tools/build_fw.sh fw/fwglue.c src/rfm1m.h | sha256sum | cut -c1-16)
STAMP=$(cat tools/catalog_fw.sh tools/fwrun.c src/fwrt.c src/fwrt.h | sha256sum | cut -c1-16)-$BSTAMP
if [ -f "$D/result.json" ] && [ "$(cat "$D/stamp" 2>/dev/null)" = "$STAMP" ]; then
    exit 0
fi
# (a result from before the build stamp: its object was built by the same build_fw.sh, unchanged since)
[ -f "$D/stamp" ] && [ ! -f "$D/build-stamp" ] && ls "$D"/files/*.rfm1m >/dev/null 2>&1 && echo "$BSTAMP" > "$D/build-stamp"
REBUILD=1
[ "$(cat "$D/build-stamp" 2>/dev/null)" = "$BSTAMP" ] && ls "$D"/files/*.rfm1m >/dev/null 2>&1 && REBUILD=0
mkdir -p "$D/files"
[ $REBUILD = 1 ] && for f in "$D"/files/*; do [ -f "$f" ] && rm -f "$f"; done
W="$D/w2c"
json_str() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g' | tr -d '\n\r'; }
result() {   # result OK REASON [FWRUN] [WASM3] [GLIBC]
    printf '{"sha256":"%s","ok":%s,"reason":"%s","fwrun":"%s","wasm3":"%s","glibc":"%s"}\n' "$SHA" "$1" \
        "$(json_str "$2")" "$(json_str "${3:-}")" "$(json_str "${4:-}")" "$(json_str "${5:-}")" > "$D/result.json"
    echo "$STAMP" > "$D/stamp"
    # the translated C (12-16 MB a firmware) is not kept
    for f in "$W"/*; do [ -f "$f" ] && rm -f "$f"; done
    for d in "$W"/*; do [ -d "$d" ] && { for f in "$d"/*; do [ -f "$f" ] && rm -f "$f"; done; rmdir "$d"; }; done
    rmdir "$W" 2>/dev/null
    exit 0
}
if [ $REBUILD = 1 ]; then
    rm -f "$D/build-stamp"
    if ! out=$(tools/build_fw.sh "$IN" "$D/files" "$W" 2>&1); then
        result false "build failed: $(echo "$out" | grep -v '^$' | tail -3 | tr '\n' ' ' | cut -c1-300)"
    fi
    echo "$BSTAMP" > "$D/build-stamp"
fi
SO=$(ls "$D"/files/*.rfm1m 2>/dev/null | head -1)
[ -n "$SO" ] || result false "build failed: no .rfm1m"
G=$(objdump -T "$SO" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
if [ -n "$G" ] && [ "$(printf '%s\nGLIBC_2.35\n' "$G" | sort -V | tail -1)" != GLIBC_2.35 ]; then
    result false "needs $G (the Move has 2.35)" "" "" "$G"
fi
run=$(timeout 300 build/tools/fwrun "$SO" --pcm "$D/fwrun.pcm" 2>"$D/fwrun.err"); rc=$?
ref=$(timeout 600 build/ref/rfm1_run --wav "$D/wasm3.wav" "$IN" 2>/dev/null | head -1)
a=$(tail -c +45 "$D/wasm3.wav" 2>/dev/null | md5sum | cut -d' ' -f1)
b=$(md5sum < "$D/fwrun.pcm" | cut -d' ' -f1)
same=0; [ -s "$D/fwrun.pcm" ] && [ "$a" = "$b" ] && same=1
rm -f "$D/fwrun.pcm" "$D/wasm3.wav"
if [ $rc != 0 ]; then
    result false "does not boot or run: $(tr '\n' ' ' < "$D/fwrun.err" | cut -c1-200) (exit $rc)" "$run" "$ref" "$G"
fi
case "$run" in *" 0 LCD writes"*) result false "boots, but never writes its screen" "$run" "$ref" "$G";; esac
[ $same = 1 ] || result false "its PCM differs from wasm3's" "$run" "$ref" "$G"
# a firmware silent on the default chord (one that plays only from its own sequencer, or needs another input) passes
# when wasm3 is just as silent: the same PCM, the screen written; the report says so
case "$run" in *"rms 0.0000"*) result true "silent on the default chord, as in wasm3" "$run" "$ref" "$G";; esac
result true "" "$run" "$ref" "$G"
