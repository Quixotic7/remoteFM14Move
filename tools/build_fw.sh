#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# One .rfm1 -> a compiled firmware object for the Move (src/rfm1m.h), in the build image (docker/Dockerfile):
#   tools/build_fw.sh IN.rfm1 OUTDIR [WORKDIR]
# OUTDIR gets <id>-<version>.rfm1m (wasm2c 1.0.32's C plus fw/fwglue.c, -O2 -ffp-contract=off, a shared object),
# <id>-<version>.json ({"rfm1m":1, "file", "sha256" of the .rfm1, "arch", "wasm2c", "memory_pages" (its linear
# memory, 64 KiB pages: the host checks it fits before making an instance), "manifest": the .rfm1's own}) and
# <id>-<version>.licenses.txt (the .rfm1's credits and licence sections), and its skin's sections when it has one
# (RFM1-FORMAT.md section 8): <id>-<version>.rfm1skin (JSON), .rfm1skin.bg, .rfm1skin.bgl (the images).
set -eu
IN="$1"
OUT="$2"
WORK="${3:-build/w2c}"
CC="${CC:-gcc}"
INFO="${RFM1INFO:-build/tools/rfm1info}"
W2C_RT="${W2C_RT:-/usr/src/wasm2c}"
mkdir -p "$OUT" "$WORK"
"$INFO" "$IN" --check >/dev/null
ID=$("$INFO" "$IN" --field id)
VER=$("$INFO" "$IN" --field version)
SHA=$(sha256sum "$IN" | cut -d' ' -f1)
BASE="$ID-$VER"
# (the C is kept per SHA: the same file is not translated twice)
W="$WORK/$SHA"
if [ ! -f "$W/done" ]; then
    mkdir -p "$W"
    wasm2c -n "$ID" "$IN" -o "$W/$ID.c"     # (and $ID.h beside it)
    : > "$W/done"
fi
DIR=$(cd "$(dirname "$0")/.." && pwd)
# the import modules' instances instantiate takes, in wasm2c's order (fwglue.c RFM1M_INST_ARGS): WASI's, and a
# Fantasy CFW's rfm1_host_1 (rfm1info --check allowed it: "virtual", "fs1" in "needs"); anything else: refused
PROTO=$(grep "^void Z_${ID}_instantiate(" "$W/$ID.h" | head -1)
[ -n "$PROTO" ] || { echo "build_fw: no Z_${ID}_instantiate in $W/$ID.h" >&2; exit 1; }
ARGS=$(printf '%s' "$PROTO" | sed -e 's/^[^,)]*//' -e 's/);$//' \
    -e 's/struct Z_wasi_snapshot_preview1_instance_t\*/WASII/g' -e 's/struct Z_rfm1_host_1_instance_t\*/HOSTI/g' | tr -d ' ')
case "$ARGS" in *[!,A-Z]*) echo "build_fw: $ID imports a module this host does not give: $PROTO" >&2; exit 1;; esac
# ain1 (VIRTUAL-CARTS.md 8): a module that exports rfm1_audio_in gets rfm1m_ain() (fwglue.c RFM1M_AIN1); others none
AIN=""
grep -q "Z_${ID}Z_rfm1_audio_in(" "$W/$ID.h" && AIN="-DRFM1M_AIN1"
PAGES=$(grep -o "wasm_rt_allocate_memory([^,]*, [0-9]*" "$W/$ID.c" | head -1 | sed 's/.*, //')
"$CC" -O2 -ffp-contract=off -fPIC -shared -fvisibility=hidden -w \
    -DWASM_RT_MEMCHECK_SIGNAL_HANDLER=0 -I"$W2C_RT" -I"$W" \
    -DMOD="$ID" -DRFM1M_ID="\"$ID\"" -DRFM1M_SHA="\"$SHA\"" -DRFM1M_INST_ARGS="$ARGS" $AIN \
    "$DIR/fw/fwglue.c" "$W/$ID.c" -o "$OUT/$BASE.rfm1m.tmp"
mv "$OUT/$BASE.rfm1m.tmp" "$OUT/$BASE.rfm1m"
{
    printf '{\n  "rfm1m": 1,\n  "file": "%s",\n  "sha256": "%s",\n  "arch": "linux-%s",\n  "wasm2c": "%s",\n  "memory_pages": %s,\n  "manifest": ' \
        "$(basename "$IN")" "$SHA" "$(uname -m)" "$(wasm2c --version)" "${PAGES:-0}"
    "$INFO" "$IN" --manifest
    printf '\n}\n'
} > "$OUT/$BASE.json"
"$INFO" "$IN" --licenses "$OUT/$BASE.licenses.txt"
rm -f "$OUT/$BASE.rfm1skin" "$OUT/$BASE.rfm1skin.bg" "$OUT/$BASE.rfm1skin.bgl"
"$INFO" "$IN" --skin "$OUT/$BASE"
echo "$OUT/$BASE.rfm1m"
