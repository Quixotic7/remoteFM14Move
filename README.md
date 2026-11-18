# RemoteFM14Move

FM-1 firmwares (`.rfm1`) running on the Ableton Move itself, played from the Move's own pads, knobs and buttons.
A [Schwung](https://github.com/charlesvestal/schwung) standalone tool, **RemoteFM1**. Phase 1 of [PLAN.md](PLAN.md).

- **The firmwares** run as native code: each `.rfm1` is translated by wasm2c and compiled for the Move
  (`<id>-<version>.rfm1m`), with the same sound as the `.rfm1` in wasm3, bit for bit (tested).
- **The controls** are [rfm1cs-abletonmove](https://github.com/Quixotic7/rfm1cs-abletonmove) 0.4.0, in-process: FM-1
  mode on the pads and knobs, MIDI, Control and the sequencer, and the projects view (Shift + step 1, or Track 1 held +
  step 1) with three firmware slots, their levels and their states.
- **The hardware** is [Move Remote](https://github.com/Quixotic7/abletonmoveremote)'s device code, in-process: the SPI
  link and the real-time audio thread on core 3, the LEDs and the screen.
- **The page**: `http://remotefm1.local:7730/` (or `http://move.local:7730/`) is RemoteFM1's own site
  (remotefm1.com's FM-1 player), built for the Move: the faceplate, the themes and skins, the firmware's labels, the
  firmware picker. Its engine is the firmware running on the Move: the screen and LEDs live, the panel back, the
  sound out of the Move. See "The page" below.

Leave it with **Shift + touch the volume knob + click the jog wheel**: Move's own software starts again.

## Build

Needs Docker (on an Apple-silicon Mac it runs the linux/arm64 image natively) and, beside this repo, the input
firmwares and FM1VST (for the wasm3 reference). The `.rfm1` files are never in git.

```sh
git submodule update --init
make                         # build/move/remotefm1-host + remotefm1-heal, build/sim/remotefm1-host, build/fw/*.rfm1m
make FIRMWARES="a.rfm1 b.rfm1"   # other firmwares (default: ../RFM1Manager/out/felucca-1.8.2 and choralroot-0.41)
make sim-run                 # the host on Move Remote's simulator: http://localhost:7730/
make site                    # only the page: build/site (scripts/build_site.sh; git, patch, rsync on the Mac)
```

## Test

```sh
make test
```

`tests/test.sh`: Move Remote's own `make test` (its branch `in-process`), then in the image (`tests/in_image.sh`):

- `build/tools/unit`: the manifest reader, the mixer and its limiter, the mDNS answers;
- `tests/bitident.sh`: each compiled firmware against FM1VST's `rfm1_run` (wasm3, `origin/rfm1-manager`): the same
  run's PCM, the same bytes;
- the glibc each Move binary needs (the Move has 2.35);
- `tests/sim_e2e.sh`: the host on the simulator: pads through its test FIFO make sound, the FM-1's LEDs and screen
  reach the Move, the page and its WebSocket (screen, LEDs, panel input), a firmware switch and a second slot from the
  projects view, a state saved and loaded, a restart that brings the slots back; the audio thread's output is the
  engine's mix sample for sample, and the first firmware's blocks equal an offline replay of its input log
  (`tools/fwrun --replay`); the page's files (every file the site refers to, their content types, no `..`), the
  firmware list, a firmware's skin and licences, `POST /api/load`;
- `tests/page_e2e.mjs` (on the Mac: the host in the image, headless Chromium): the page at `http://remotefm1.local:PORT/`
  (not a secure context, as on a real Move): no 404, no exception or console error, nothing asked of the browser
  (AudioContext, Web MIDI, WebUSB, microphone), the panel and the live screen, a click on a drawn key sounds in the
  simulator, the picker loads ChoralRoot (and its skin shows), a load from outside the page is followed. Its
  screenshot: `docs/move-page.png`.

## Package

```sh
make package                 # dist/remotefm1-module.tar.gz
```

One `remotefm1/` folder: `module.json` (id `remotefm1`, tool, standalone), `standalone` (Move Remote's launcher: the
log, the setuid real-time helper), `remotefm1-host`, `remotefm1-heal`, `firmwares/` (the bundled compiled firmwares
with their manifests, licence texts and skins), `site/` (the page, ~0.9 MB), `FIRMWARES.md` (each firmware's licence
and exact source), the licences. About 4.2 MB.

## Layout

| Path | What |
|---|---|
| `src/main.c` | the program: the engine, the page, mDNS, then Move Remote's platform main (`plat_move.c` / `plat_sim.c`) |
| `src/engine.c` | the engine thread: 3 firmware slots on their device clocks, the mix and limiter, the ring the audio thread drains, the rfm1cs session over a socketpair, the actions |
| `src/fwrt.c` | the firmware runtime: a compiled firmware loaded and booted as RFM1-FORMAT.md section 7 says, its clock, its panel, the WASI imports, traps |
| `src/storage.c` | the RemoteFM1 folder and the storage thread (firmwares, states, the Move's settings, library and projects) |
| `src/web.c` | the page on :7730: the site's files, the API, the WebSocket (src/web.h) |
| `web/move/`, `scripts/build_site.sh` | the page: RemoteFM1's web/site with `site.patch` and `js/host.js`, `js/movehost.js` -> `build/site` |
| `src/mdns.c` | `remotefm1.local` |
| `fw/fwglue.c`, `tools/build_fw.sh` | `.rfm1` -> `.rfm1m` (wasm2c, then a shared object with one entry table) |
| `tools/rfm1info.c`, `tools/fwrun.c` | an `.rfm1`'s manifest and checks; a compiled firmware run offline (rfm1_run's run, or a replay) |
| `third_party/abletonmoveremote` | Move Remote, branch `in-process` (its embedding hooks) |
| `third_party/rfm1cs-abletonmove` | rfm1cs-abletonmove, tag `v0.4.0` |
| `third_party/RemoteFM1` | RemoteFM1 at 80f1f84 (its `web/site`: the page) |
| `docker/Dockerfile` | the build and test image (debian bookworm, the Move's toolchain, wabt 1.0.32) |

On the Move, the RemoteFM1 folder is `/data/UserData/RemoteFM1`:

```
firmwares/   <id>-<version>.rfm1m, .json (the manifest, with the .rfm1's SHA-256), .licenses.txt, and its skin when the
             .rfm1 has one: .rfm1skin (rfm1.skin), .rfm1skin.bg, .rfm1skin.bgl (the images)
states/<id>/ flash.bin (the flash as it was left), state-NN.bin (the 32 state slots), each with a .json
move/        settings.bin, library.rmvl, open-project, projects/move-project-NN.rfmmove
```

## The page

The page on :7730 is RemoteFM1's own `web/site` (third_party/RemoteFM1, pinned), not a lookalike.
`scripts/build_site.sh` copies its `index.html`, `css/`, `fonts/` and `js/` into `build/site`, applies
`web/move/site.patch`, adds `web/move/js/host.js` and `movehost.js`, and sets the host flag
(`<meta name="fm1-host" content="move">`). The host serves that folder (`site/` beside the program, or `--site DIR`).

**The seam: a host flag and one back end.** `js/host.js` reads the flag (`ON_MOVE`). On the Move, `main.js` makes a
`MoveHostPlayer` (`js/movehost.js`) where it would make `audio.js`'s `Player`: the same calls (`load`, `panel`,
`start`, ...) and the same events (`frame`, `leds`, `info`, `labels`), so the panel, themes, skins and labels are the
site's own code. Underneath it is the host's API:

- `GET /api/firmwares` -> the catalog (`moveCatalog()`: each installed firmware as an entry with `engines: ["move"]`;
  `catalog.active` is what the Move's active slot runs);
- `/ws` -> `frame` (the screen's rows, RGB565 big-endian, to host order), `leds`, and the info JSON -> `info`
  (`buttonNames`, `keymap`) and `labels` (the firmware's live labels, `lseq` / `labels`);
- `panel(p)` -> the changes as the WebSocket's ops: KEY (with velocity), LEAN, BTN, ENC, MASTER (the first MASTER is
  not sent, so the Move's volume knob stands until the page's MASTER moves);
- `load(entry)` -> `POST /api/load {slot: active, id}`: the engine queues it on its own thread and does what
  rfm1cs's FW_LOAD does (the storage worker opens it, the slot boots it, the Move is told: the library keeps it);
- `GET /api/fw/<base>.licenses.txt`, `.rfm1skin`, `.rfm1skin.bg`, `.rfm1skin.bgl` -> the info's `credits` and `skin`
  (the .rfm1's own sections, kept beside the compiled firmware by `tools/build_fw.sh`), so a firmware's skin shows
  as on remotefm1.com.

When the Move switches firmware itself (its projects view, another track), the page follows (`moved`). The page
needs no secure context: no AudioWorklet, Web MIDI, WebUSB, microphone, IndexedDB saves or folder access.

**On the Move the page shows:** the faceplate at the site's layouts, the themes and background image, firmware
skins, the firmware's button names and live labels, the screen protector, the computer keyboard and mouse/touch
panel, the header's firmware picker (the Move's firmwares), Source and Site source links, the sheet's Firmware,
Look, Background, View, Engine (one line) and About pages. **Hidden:** the emulated and native browser engines,
opening or dropping an .rfm1, saves in the browser, Use my FM-1 / Connect FM-1, Use my OMX-27, Use my Ableton Move,
the RemoteFM1 folder, audio input, the recorder, volume / sound output / MIDI input / MIDI learn, Install RemoteFM1
and the app links, the Editor (the firmware editors talk to the firmware's MIDI, which the page has no path to).

**Upstreaming.** Every change is behind `ON_MOVE`; with `content="web"` the site is RemoteFM1's unchanged. So
RemoteFM1 can take `site.patch`, `host.js` and `movehost.js` as they are; RemoteFM14Move's build then only sets the
flag (and later fetches RemoteFM1's built site instead of patching it).
