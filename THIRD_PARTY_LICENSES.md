# Third-party licences

RemoteFM14Move itself is MPL-2.0 (LICENSE). What it is built from:

- **Move Remote** (third_party/abletonmoveremote, https://github.com/Quixotic7/abletonmoveremote, MPL-2.0): the device
  code linked into remotefm1-host (the SPI link, the real-time audio thread, the LEDs and screen, the simulator).
  - Its `device/heal.c` (remotefm1-heal) is MIT: after schwung-movy (https://github.com/DimaDake/schwung-movy, MIT,
    Copyright (c) 2026 megadake) by way of evilgeniusmove. The SPI layout follows Schwung
    (https://github.com/charlesvestal/schwung, MIT).
  - `common/font5x7.c`: the classic HD44780-style 5x7 glyph set.
- **rfm1cs-abletonmove** (third_party/rfm1cs-abletonmove, https://github.com/Quixotic7/rfm1cs-abletonmove, MPL-2.0):
  the Move's controls for an FM-1 firmware, linked into remotefm1-host. It vendors Move Remote's client half
  (MPL-2.0).
- **wasm2c's runtime** (wabt 1.0.32, https://github.com/WebAssembly/wabt, Apache-2.0): `wasm-rt-impl.c` and the
  headers, built into remotefm1-host; the package carries Debian's copyright file for wabt as
  `licenses/wabt-wasm2c-runtime.txt`.
- **The firmwares** (firmwares/*.rfm1m in the package): each is its own work under its own licence (FIRMWARES.md,
  each firmware's `.licenses.txt`), with its exact source at its `source_url`. A firmware object is a separate
  program loaded at run time; it is the .rfm1 file translated by wasm2c, with `fw/fwglue.c` (MPL-2.0) as its entry
  table.
- **RemoteFM1's web site** (third_party/RemoteFM1, https://github.com/Quixotic7/RemoteFM1, GPL-3.0-only): the page the
  Move serves (`site/` in the package, scripts/build_site.sh) is its `web/site`, with `web/move/site.patch` over it and
  `web/move/js/host.js` and `movehost.js` beside it (both GPL-3.0-only, so they can go upstream as they are). The
  page carries the GPL's text (`site/LICENSE.txt`) and where its exact source is (`site/SOURCE.txt`). Its
  `js/move-core.js` and `move-seq.js` are rfm1cs-abletonmove's (MPL-2.0) at the commit RemoteFM1 pins. Its fonts
  (Chakra Petch, DM Sans, Unbounded, DM Mono) are under the SIL Open Font License 1.1 (`site/fonts/OFL-*.txt`).
  The page is a separate program the browser runs; remotefm1-host only serves its files.
