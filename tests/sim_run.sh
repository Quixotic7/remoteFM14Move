#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# The host on the simulator for a person: `make sim-run`, then http://localhost:7730/ (the page plays the firmware;
# its sound goes nowhere, the simulator has no speaker). The Move's pads and buttons: lines into /tmp/rfm14sim/mr-cmd
# (docker exec): "note 84 100", "note 84 0", "cc 85 127" .. Ctrl-C stops it.
cd "$(dirname "$0")/.."
mkdir -p /tmp/rfm14sim
exec build/sim/remotefm1-host --root /tmp/rfm14sim/root --sim-dir /tmp/rfm14sim --firmwares build/fw --site build/site "$@"
