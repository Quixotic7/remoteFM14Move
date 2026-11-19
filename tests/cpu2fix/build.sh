#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# tests/cpu2fix/cpu2fix-1.0.rfm1 from cpu2fix.c, on the Mac (Emscripten: EMCC, default ~/emsdk's), as FM1VST's
# fm1_add_rfm1 builds an .rfm1: a standalone module with the format's WebAssembly features, then its manifest as the
# "rfm1" custom section. The file is kept in git (the build image has no Emscripten); run this after changing the C.
set -eu
cd "$(dirname "$0")"
EMCC="${EMCC:-$HOME/emsdk/upstream/emscripten/emcc}"
"$EMCC" -O2 -mcpu=mvp -msign-ext -mnontrapping-fptoint -mbulk-memory -mmutable-globals \
    -sSTANDALONE_WASM -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=4194304 -sALLOW_MEMORY_GROWTH=0 \
    -sSTACK_SIZE=262144 --no-entry -o cpu2fix.wasm cpu2fix.c
python3 - <<'EOF'
import json
m = {"rfm1": 1, "abi": 6, "id": "cpu2fix", "name": "CPU2 fixture", "version": "1.0", "license": "MPL-2.0",
     "source_url": "https://github.com/Quixotic7/remoteFM14Move", "authors": ["RemoteFM14Move tests"],
     "wants": ["cpu2"]}
def leb(n):
    out = bytearray()
    while True:
        b = n & 0x7f
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)
w = open("cpu2fix.wasm", "rb").read()
payload = leb(4) + b"rfm1" + json.dumps(m).encode()
open("cpu2fix-1.0.rfm1", "wb").write(w + b"\x00" + leb(len(payload)) + payload)
EOF
rm -f cpu2fix.wasm
ls -l cpu2fix-1.0.rfm1
