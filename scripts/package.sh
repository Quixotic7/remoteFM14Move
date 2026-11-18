#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The module's folder, build/pkg/remotefm1, staged in the build image after `make in-all`; `make package` then tars it
# on the Mac (as Move Remote's package.sh does) into dist/remotefm1-module.tar.gz: one remotefm1/ folder, as Schwung
# Manager installs a tool (schwung docs/MODULES.md).
#   remotefm1/module.json, standalone          the tool "RemoteFM1" (standalone: Move's software stops while it runs)
#   remotefm1/remotefm1-host, remotefm1-heal   the host (linux-aarch64, glibc <= 2.35) and its real-time helper
#   remotefm1/firmwares/                       the bundled compiled firmwares, each with its manifest and licences
#   remotefm1/site/                            the page (scripts/build_site.sh: RemoteFM1's web/site for the Move)
#   remotefm1/LICENSE, THIRD_PARTY_LICENSES.md, FIRMWARES.md, licenses/
set -eu
cd "$(dirname "$0")/.."
for f in build/move/remotefm1-host build/move/remotefm1-heal; do
    [ -x "$f" ] || { echo "package: $f is missing: run make" >&2; exit 1; }
done
ls build/fw/*.rfm1m >/dev/null 2>&1 || { echo "package: no compiled firmwares in build/fw" >&2; exit 1; }
[ -f build/site/index.html ] || { echo "package: no build/site: run make (scripts/build_site.sh)" >&2; exit 1; }
[ -d build/pkg ] && mv build/pkg "build/pkg.old.$(date +%s)"
P=build/pkg/remotefm1
mkdir -p "$P/firmwares" "$P/licenses" dist
cp module/module.json module/standalone build/move/remotefm1-host build/move/remotefm1-heal LICENSE \
    THIRD_PARTY_LICENSES.md "$P/"
cp /usr/share/doc/wabt/copyright "$P/licenses/wabt-wasm2c-runtime.txt"
cp /usr/share/common-licenses/Apache-2.0 "$P/licenses/Apache-2.0.txt"
chmod 755 "$P/standalone" "$P/remotefm1-host" "$P/remotefm1-heal"
cp -R build/site "$P/site"
{
    echo "# The bundled firmwares"
    echo
    echo "Each is an FM-1 firmware's .rfm1 file translated to C by wasm2c 1.0.32 and compiled for the Move"
    echo "(<id>-<version>.rfm1m). Its manifest (<id>-<version>.json) names the .rfm1 it came from and that file's"
    echo "SHA-256; <id>-<version>.licenses.txt holds the licence and credit sections the .rfm1 carries. The source each"
    echo "firmware's licence promises is at its source_url (the exact commit), with the build glue at FM1VST."
    echo
    echo "| Firmware | Version | Licence | Source | .rfm1 SHA-256 |"
    echo "|---|---|---|---|---|"
} > "$P/FIRMWARES.md"
for so in build/fw/*.rfm1m; do
    base=$(basename "$so" .rfm1m)
    in=build/in/$base.rfm1
    cp "$so" "build/fw/$base.json" "build/fw/$base.licenses.txt" "$P/firmwares/"
    for x in rfm1skin rfm1skin.bg rfm1skin.bgl; do
        if [ -f "build/fw/$base.$x" ]; then cp "build/fw/$base.$x" "$P/firmwares/"; fi
    done
    chmod 755 "$P/firmwares/$base.rfm1m"
    if [ -f "$in" ]; then
        I=build/tools/rfm1info
        echo "| $($I "$in" --field name) | $($I "$in" --field version) | $($I "$in" --field license) |" \
             "$($I "$in" --field source_url) | $(sha256sum "$in" | cut -d' ' -f1) |" >> "$P/FIRMWARES.md"
    fi
done
echo "package: staged $P"
