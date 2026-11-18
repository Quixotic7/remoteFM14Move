#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The page the Move serves (src/web.c, :7730): RemoteFM1's own site, built for the Move, into build/site.
#   1. RemoteFM1's web/site (third_party/RemoteFM1, pinned): index.html, css/, fonts/, js/ (not its catalog, its
#      firmware files, tests or tools); js/move-core.js and move-seq.js from rfm1cs-abletonmove at the commit RemoteFM1
#      pins for them (as RemoteFM1's Pages build copies them), from our own rfm1cs-abletonmove submodule's history
#   2. web/move/site.patch over it (everything behind js/host.js ON_MOVE), web/move/js/* beside it
#   3. the host flag: <meta name="fm1-host" content="move">
#   4. LICENSE.txt (GPL-3.0, RemoteFM1's) and SOURCE.txt (where this exact source is)
# On the Mac (git, patch, rsync): make runs it before the image's build.
set -eu
cd "$(dirname "$0")/.."
RF=third_party/RemoteFM1
CS=third_party/rfm1cs-abletonmove
SRC=$RF/web/site
OUT=build/site
[ -f "$SRC/index.html" ] || { echo "build_site: $SRC is missing: git submodule update --init" >&2; exit 1; }
mkdir -p "$OUT"
rsync -a --delete --checksum --include='/index.html' --include='/css/***' --include='/fonts/***' --include='/js/***' \
    --exclude='*' "$SRC/" "$OUT/"
CS_AT=$(git -C "$RF" ls-tree HEAD web/vendor/rfm1cs-abletonmove | awk '{print $3}')
for f in move-core.js move-seq.js; do
    git -C "$CS" show "$CS_AT:web/$f" > "$OUT/js/$f.tmp" || { echo "build_site: rfm1cs-abletonmove $CS_AT has no web/$f" >&2; exit 1; }
    mv "$OUT/js/$f.tmp" "$OUT/js/$f"
done
patch -p1 -d "$OUT" --forward --quiet < web/move/site.patch
cp web/move/js/*.js "$OUT/js/"
sed -i.bak 's|<meta name="fm1-host" content="web">|<meta name="fm1-host" content="move">|' "$OUT/index.html"
rm -f "$OUT/index.html.bak"
grep -q '<meta name="fm1-host" content="move">' "$OUT/index.html" || { echo "build_site: no host flag in index.html" >&2; exit 1; }
cp "$RF/LICENSE" "$OUT/LICENSE.txt"
{
    echo "This page is RemoteFM1's web site (GPL-3.0-only, LICENSE.txt) built for the Ableton Move by RemoteFM14Move."
    echo "  RemoteFM1:              https://github.com/Quixotic7/RemoteFM1/tree/$(git -C "$RF" rev-parse HEAD)/web/site"
    echo "  rfm1cs-abletonmove:     https://github.com/Quixotic7/rfm1cs-abletonmove/tree/$CS_AT/web (js/move-core.js, move-seq.js)"
    echo "  the changes and the Move's back end: https://github.com/Quixotic7/remoteFM14Move/tree/$(git rev-parse HEAD)/web/move"
    echo "  fonts: SIL Open Font License 1.1 (fonts/OFL-*.txt)"
} > "$OUT/SOURCE.txt"
echo "build_site: $OUT ($(du -sk "$OUT" | cut -f1) KB, $(find "$OUT" -type f | wc -l | tr -d ' ') files)"
