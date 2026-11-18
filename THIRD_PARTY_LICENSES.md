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
