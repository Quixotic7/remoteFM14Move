#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The RemoteFM1 Wi-Fi simulator's sources for the build image (tests/fm1wifi_e2e.sh): RemoteFM1's own host simulator
# of its Wi-Fi build (fm1-base's host/plat_host.c with the app; net/rfm_net.c on the computer's sockets), whose
# generated headers need Python and Pillow (on the Mac). This runs RemoteFM1's `./build.sh --wifi --host` there and
# copies what the compiler needs into build/wifisim-src/ (fm1-base patched, the app with its generated headers, the
# protocol header), so that `make in-wifisim` compiles the same simulator for linux/arm64 in the image.
#   RFM1_WIFI_DIR   a RemoteFM1 checkout with its submodules (default third_party/RemoteFM1: its base/ needs
#                   `git -C third_party/RemoteFM1 submodule update --init --recursive base`)
set -eu
cd "$(dirname "$0")/.."
RF=${RFM1_WIFI_DIR:-third_party/RemoteFM1}
[ -f "$RF/base/tools/build.py" ] || { echo "wifisim_src: $RF/base is empty (RFM1_WIFI_DIR, or init its base submodule)" >&2; exit 1; }
[ -f "$RF/firmware/fm1-wifi.toml" ] || { echo "wifisim_src: $RF has no Wi-Fi build (firmware/fm1-wifi.toml)" >&2; exit 1; }
(cd "$RF" && env -u CC ./build.sh --wifi --host >/dev/null)
OUT=build/wifisim-src
mkdir -p "$OUT"
rsync -a --delete --checksum --exclude .git --exclude /assets/ --exclude /bt/ --exclude /tests/ --exclude /docs/ \
    --exclude /examples/ --exclude /template/ --exclude /web/ "$RF/build/fm1-base/" "$OUT/fm1-base/"
rsync -a --delete --checksum --include='/build/' --include='/build/gen/***' --exclude='/build/*' \
    "$RF/build/app-wifi/" "$OUT/app-wifi/"
rsync -a --delete --checksum "$RF/build/proto/" "$OUT/proto/"
git -C "$RF" rev-parse HEAD > "$OUT/REMOTEFM1_COMMIT"
echo "wifisim_src: $OUT from $RF $(cut -c1-7 "$OUT/REMOTEFM1_COMMIT")"
