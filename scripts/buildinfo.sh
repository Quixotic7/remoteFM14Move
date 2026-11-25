#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# build/gen/buildinfo.h: the build's commits and date, for the host (src/main.c: --version, the page's Update status)
# and tools/make_update.py (the .rfm1u's manifest names the same). On the Mac (git), before the image's build (make).
# Rewritten only when the commits change (a "-dirty" mark for uncommitted changes counts), so an unchanged tree does
# not rebuild the host; the date is when that happened.
set -eu
cd "$(dirname "$0")/.."
OUT=build/gen/buildinfo.h
mkdir -p build/gen
git_of() {   # the short commit of a tree, "-dirty" when tracked files differ from it
    c=$(git -C "$1" rev-parse --short=12 HEAD 2>/dev/null || echo unknown)
    if [ "$c" != unknown ] && ! git -C "$1" diff --quiet HEAD -- 2>/dev/null; then c="$c-dirty"; fi
    echo "$c"
}
HOST=$(git_of .)
CORE=$(git_of third_party/rfm1cs-abletonmove)
if [ -f "$OUT" ] && grep -q "RFM14_GIT \"$HOST\"" "$OUT" && grep -q "RFM14_GIT_CORE \"$CORE\"" "$OUT"; then
    exit 0
fi
{
    echo "/* made by scripts/buildinfo.sh (make): never edited, never in git */"
    echo "#define RFM14_GIT \"$HOST\""
    echo "#define RFM14_GIT_CORE \"$CORE\""
    echo "#define RFM14_BUILT \"$(date -u '+%Y-%m-%d %H:%M UTC')\""
} > "$OUT.tmp"
mv "$OUT.tmp" "$OUT"
echo "buildinfo: host $HOST, core $CORE"
