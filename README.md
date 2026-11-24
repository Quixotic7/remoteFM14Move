# RemoteFM14Move

FM-1 firmwares (`.rfm1`) running on the Ableton Move itself, played from the Move's own pads, knobs and buttons.
A [Schwung](https://github.com/charlesvestal/schwung) standalone tool, **RemoteFM1**, version **0.1.0**
([CHANGES.md](CHANGES.md)). Phase 1 of [PLAN.md](PLAN.md).

- **The firmwares** run as native code: each `.rfm1` is translated by wasm2c and compiled for the Move
  (`<id>-<version>.rfm1m`), with the same sound as the `.rfm1` in wasm3, bit for bit (tested).
- **The controls** are [rfm1cs-abletonmove](https://github.com/Quixotic7/rfm1cs-abletonmove) 0.8.0, in-process. The
  Track buttons (proposal 2): Track 1, 2, 3 are the three firmware slots (a tap plays one in its own layout: FM-1, its
  takeover, or a note layout with the Control CCs on the knobs; held: its layout menu; a double tap: its firmware
  browser); Track 4 is the sequencer side (a tap: the Mixer / Routing; held: the Shift shortcuts; a double tap: the
  projects); Back flips to the Sequencer and back.
- **The hardware** is [Move Remote](https://github.com/Quixotic7/abletonmoveremote)'s device code, in-process: the SPI
  link and the real-time audio thread on core 3, the LEDs and the screen.
- **The page**: `http://remotefm1.local:7730/` (or `http://move.local:7730/`) is RemoteFM1's own site
  (remotefm1.com's FM-1 player), built for the Move: the faceplate, the themes and skins, the firmware's labels, the
  firmware picker. Its engine is the firmware running on the Move: the screen and LEDs live, the panel back, the
  sound out of the Move. See "The page" below.

- **On the page too:** the firmware's own **Editor** (as on remotefm1.com: its SysEx goes to the firmware on the
  Move and back), and a **file browser** for the RemoteFM1 folder (download, a folder as a .zip, upload, rename,
  delete; Export all projects, Export states). See "The page".
- **Audio input** for Fantasy CFWs (`ain1`): the Move's line in / mic reaches a cart that takes it (Elasticat's INPUT
  machine and REC). See "Fantasy CFWs".

- **Schwung FX**: the MIDI FX and audio FX the user installed in Schwung (`/data/UserData/schwung/modules`) run inside
  RemoteFM1: 3 MIDI FX in series on the sequencer's MIDI, 3 audio FX on send / return buses, set up from the Move's FX
  setup (Session in the projects view) or the page's Mix & FX view. See "Schwung FX" below.

Leave it with **Shift + touch the volume knob + click the jog wheel**: Move's own software starts again.

## Install

You need a Move with [Schwung](https://schwung.dev) **1.6.0 or later** installed.

**From Schwung Manager** (on a computer on the same Wi-Fi as the Move):

1. Open [http://move.local:7700](http://move.local:7700) and go to **Modules**.
2. Open **Install Custom Module**. Under **From GitHub URL**, enter `Quixotic7/remoteFM14Move` and press
   **Install from GitHub**.

Or download `remotefm1-module.tar.gz` from the
[v0.1.0 release](https://github.com/Quixotic7/remoteFM14Move/releases/tag/v0.1.0) and use **From Tarball**.
Schwung Manager reads [release.json](release.json) on `main` for the version and the download, so the same page
shows an update when there is a new release.

**Start it:** on the Move, open Schwung's Tools menu (**Shift + Step 13**, or **Shift + touch Volume + Step 13**,
depending on your Schwung shortcut setting), choose **RemoteFM1** and press the jog wheel. Move's own software stops;
the first start copies the bundled firmwares into the RemoteFM1 folder (~45 MB) before any sound. The page is
`http://remotefm1.local:7730/`. **Leave it** with Shift + touch the volume knob + click the jog wheel.

Releasing: [docs/RELEASING.md](docs/RELEASING.md).

## Build

Needs Docker (on an Apple-silicon Mac it runs the linux/arm64 image natively) and, beside this repo, the input
firmwares and FM1VST (for the wasm3 reference). The `.rfm1` files are never in git.

```sh
git submodule update --init
make                         # build/move/remotefm1-host + remotefm1-heal, build/sim/remotefm1-host, build/fw/*.rfm1m
make FIRMWARES="a.rfm1 b.rfm1"   # other firmwares (default: ../RFM1Manager/out/felucca-1.8.2 and choralroot-0.41)
make sim-run                 # the host on Move Remote's simulator: http://localhost:7730/
make site                    # only the page: build/site (scripts/build_site.sh; git, patch, rsync on the Mac)
make catalog                 # dist/move-catalog: the Move catalog (below; ~20 min the first time, then cached)
```

## The Move catalog

`make catalog` (tools/build_move_catalog.py, on the Mac with Docker) builds every .rfm1 RemoteFM1's catalog
(https://remotefm1.com/catalog.json) hosts for the Move: `dist/move-catalog/` holds `index.json` and, for each
firmware, `<id>-<version>.rfm1m`, its manifest `.json`, `.licenses.txt` and skin files. The report is
`dist/move-catalog-report.md` (what was tried, built, passed, left out and why, the sizes).

- **Inputs:** the catalog's `rfm1` entries (hosted-only ones included: they are hosted, not bundled; gated or
  unpublished ones are not in it), each file fetched from the site and checked against the catalog's SHA-256, else
  RFM1Manager/out's copy with that SHA-256. The thumbnails come from RemoteFM1FW's catalog images (a centre square,
  64 x 64, autocontrast, an Atkinson dither on luma: the design mockup's method); the hosted-only marks from its
  README. Firmwares the owner excludes are never listed; never a raw .syx.
- **The checks** (tools/catalog_fw.sh in the image, cached per .rfm1 SHA-256): it builds; its object needs glibc
  <= 2.35; it boots and runs rfm1_run's default run through the host's own runtime (tools/fwrun) with sound and the
  screen written; and that run's PCM is wasm3's (FM1VST's rfm1_run), byte for byte. A firmware silent on that chord
  passes only when wasm3 is just as silent (the report says so). One that fails is left out.
- **index.json** (schema 1): per firmware the id, name, version, categories (rfm1cs's five: names, `cats` bits, `cat`
  the main one), the ASCII description (the catalog's `what`), licence, source_url, beta, hosted_only, the .rfm1's
  name and SHA-256, the files to install (name, SHA-256, size; the manifest last) and `thumb` (64 x 64, 1 bit, rows
  top first, MSB left, base64).
- **index.json.sig:** the index's signature: the base64 (one line) of the 64-byte Ed25519 signature (RFC 8032) over
  index.json's exact bytes. `make catalog` makes it (tools/sign_index.py) with the catalog's signing key,
  `~/.config/remotefm14move/catalog-signing-key.pem` on the owner's Mac (mode 600, never in git;
  `CATALOG_SIGNING_KEY=path` names another, e.g. for a CI). Its public half is compiled into the host
  (src/catalog_key.h): `38561cc131df9c7875b9524229f5dfa446b86bd0cb57dfab9757a3875c8dfbb0` (hex). The script refuses a
  key whose public half is not that one. Signing uses Python's cryptography when installed, else an OpenSSL 3 command
  line with Ed25519 (Homebrew's; macOS's LibreSSL has none).
- **Hosting:** the files go up flat (GitHub release assets work), index.json.sig beside index.json. The Move fetches
  `<base>/index.json`, `<base>/index.json.sig` and `<base>/<file>`; the base is `/data/UserData/RemoteFM1/catalog-url`
  (first line), else `--catalog-url`, else `https://github.com/Quixotic7/remoteFM14Move/releases/download/move-catalog`
  (src/catalog.h `CAT_URL_DEFAULT`). Nothing is published by the build.

**On the Move** (src/catalog.c): the catalog is the index (the downloaded `catalog/index.json`, or the module's
snapshot, whichever was generated later) merged with what is installed; every firmware goes into rfm1cs's
`rfm_move_fw_list` with its installed version, categories, description and thumbnail. Mute in the projects view is
the catalog: a jog press is `FW_DOWNLOAD`, which the storage module's network thread does with the host's own client
(src/https.c): each file into `firmwares/.dl/`, its size and SHA-256 checked against the signed index, then renamed
into `firmwares/` with the manifest last; progress
through `rfm_move_fw_progress`, then `rfm_move_fw_installed`. Any failure is -1 with nothing installed and nothing
left behind. An update leaves the old version running in its slot (and its files on disk) until the slot loads the
new one; the flash and states are keyed by id, so they carry over. The index is fetched again at start (when online)
and every 6 hours (10 minutes after a failure).

**Trust** (why the Move runs only what the owner signed: an .rfm1m is native code the host dlopens):

- **The index is signed** (src/catsig.c, src/ed25519.c). index.json is used only when index.json.sig beside it is a
  good Ed25519 signature by src/catalog_key.h's key: a downloaded one (checked before it is kept: one that is
  unsigned, badly signed or changed is refused, not cached, not used), the kept `catalog/index.json` (checked again at
  every read) and the module's bundled snapshot (`catalog/index.json.sig` in the package; `make package` refuses an
  index without a good one). The verifier is TweetNaCl's Ed25519 arithmetic (public domain; verification only, S < L
  added, tested with RFC 8032's vectors) with our SHA-512, in the host: it needs no library, so the bundled snapshot is
  checked even on a system with no libcrypto, and the check does not depend on the system's OpenSSL build.
- **Every file is checked** against the SHA-256 and size the signed index gives, before anything is installed.
- **HTTPS** (src/https.c) uses the Move's own OpenSSL 3 (`libssl.so.3`, `libcrypto.so.3`), opened with dlopen at the
  first https fetch, not linked: without it the host runs as before and downloads say "no TLS on this system". The
  chain is verified against `/etc/ssl/certs` (and its ca-certificates.crt when there), the host name with
  SSL_set1_host (and SNI), TLS 1.2 at least. HTTP/1.1 GET with Content-Length, chunked or close; redirects (GitHub's
  asset downloads go to another host) up to 5 and never from https to http; a size cap (the index 8 MB, each file
  its own size); 30 s with no byte (15 s to connect) is a failure. BusyBox's wget is not used any more (its TLS
  fails against GitHub and never checks certificates).
- **Plain http://** is fetched only when the base URL is plain http, which only the owner can set (`catalog-url`,
  `--catalog-url`; the default is https). The signature and SHA-256s still protect what comes.
- **Test builds:** the simulator's host (`build/sim`, the tests' and `make sim-run`'s) is built with
  `RFM14_TEST_BUILD`: it also trusts the test key the Makefile makes under `build/testkey/` (never the real one),
  takes `--ca-dir DIR` (the tests' CA) and the tests' hooks (`RFM14_TEST_INDEX_MS`, `RFM14_TEST_LIBSSL`). The
  Move's host (`build/move`) has none of them: `tests/in_image.sh` checks that it carries the real key's bytes and not
  the test key's.

## Test

```sh
make test
```

`tests/test.sh`: Move Remote's own `make test` (its branch `in-process`), then in the image (`tests/in_image.sh`):

- `build/tools/unit`: the manifest reader, the mixer and its limiter, the mDNS answers; SHA-512 and Ed25519 (RFC 8032's
  vectors, a changed bit refused, S + L refused); the test catalog's index.json.sig (good; a changed index, a short
  one, no .sig, a damaged one refused; not the release key's); a Fantasy CFW's file API (src/fsvc.c): the path rules
  (.., ., //, a trailing /, \\, NUL, bad UTF-8, outside the three areas), the manifest's folders, symlinks out, every
  op, LIST's order, paging and never-split entries, the limits (1 MiB, 32 in flight, a buffer past the memory),
  cancel, answers only in the pump (inline one ms late; from another thread only at the next pump), an instance freed
  while its request runs; the page's file browser (src/files.c): the path rules (.., ., hidden, //, a trailing /,
  absolute, \, control characters, bad UTF-8, 255 / 1024 bytes, a new name's characters), %-unescaping, what may
  change (the five folders, firmwares/ and catalog/ read-only, a running firmware's states/ and data/, the open
  project), CRC-32, a zip read in 64 KB and 7-byte pieces the same, files_rmtree (a link unlinked, not followed);
- `tests/bitident.sh`: each compiled firmware against FM1VST's `rfm1_run` (wasm3, `origin/rfm1-manager`): the same
  run's PCM, the same bytes;
- the glibc each Move binary needs (the Move has 2.35);
- `tests/sim_e2e.sh`: the host on the simulator: pads through its test FIFO make sound, the FM-1's LEDs and screen
  reach the Move, the page and its WebSocket (screen, LEDs, panel input), a firmware switch and a second slot from the
  projects view, a state saved and loaded, a restart that brings the slots back; the audio thread's output is the
  engine's mix sample for sample, and the first firmware's blocks equal an offline replay of its input log
  (`tools/fwrun --replay`); the page's files (every file the site refers to, their content types, no `..`), the
  firmware list, a firmware's skin and licences, `POST /api/load`; the page's MIDI (`/ws` op 32: an editor's SysEx in
  5-byte pieces) reaches Felucca and its INFO reply comes back (binary 5);
- `tests/fx_e2e.sh`: the slots and Schwung FX on the simulator, with Schwung's arp, chord and freeverb (and crashfx,
  hogfx) built as the installed modules: the page's slot loads, switches and unloads (into the core), the Move's switch
  followed; the arp turns a held chord into a stream on its track and passes other tracks through; a firmware's track
  filter; a freeverb send changes the output; a crash at load and a CPU hog refused; the page's and the Move's FX setup
  edits seen on the other side (the page, the Move's knobs and pages); 3 firmwares + 3 MIDI FX + 3 audio FX playing
  with no late block; the FX setup and the modules' own state back after an autosave and a restart;
- `tests/dl_e2e.sh`: the catalog against local servers (`build/testcat`, the Move catalog of AMB-1, ChoralRoot and
  Felucca, its index signed with the test key): over plain http (BusyBox httpd, an owner's base URL) the bundled
  index, its fetch at start, Mute + a pad shows the index's thumbnail (the screen's 64 x 64, bit for bit), a jog press
  downloads (a damaged file refused, no network -1, then installed, listed and loadable); over HTTPS (`openssl
  s_server`, a test CA through `--ca-dir`, cat.test redirecting every file to objects.test) the index and an update
  of ChoralRoot 0.40 (0.40 keeps playing, the new one loads from its saved flash, 0.40's files go); refused, nothing
  kept: a certificate for another name (wrong.test), a self-signed one, an index with no signature, another index's
  signature, an index changed after signing, a redirect from https to http, a download from wrong.test, a kept index
  changed on disk; the bundled snapshot unsigned or badly signed: not used; no libssl: a clear error, the rest as
  before; three loads into slot 2 while slot 1 plays, each start held
  40 ms: no late block (the same without the preroll: late blocks); ChoralRoot's per-key lines rate-limited; the log
  moved aside past its cap;
- `tests/page_e2e.mjs` (on the Mac: the host in the image, headless Chromium): the page at `http://remotefm1.local:PORT/`
  (not a secure context, as on a real Move): no 404, no exception or console error, nothing asked of the browser
  (AudioContext, Web MIDI, WebUSB, microphone), the panel and the live screen, a click on a drawn key sounds in the
  simulator, the picker loads ChoralRoot (and its skin shows), a load from outside the page is followed; the Editor
  (remotefm1.com and an author's site stood in for by the test, CDP Fetch): Felucca's editor in the panel's sandbox
  asks the firmware on the Move for INFO, the reply reaches the editor, which answers it; the same with
  remotefm1.com's hosted copy; Files: the sheet's Add samples puts a WAV into audio/, the header's file browser
  (the root read-only, a folder made, a file dropped into it, renamed, deleted, a folder deleted with what is in it,
  firmwares/ read-only), Export states and Export all projects downloaded; States and projects (the header's caps,
  "The page's States and projects"): ChoralRoot's states saved into empty ones, loaded (a new instance, its working
  memory restored), saved over (asked first), downloaded, uploaded into an empty one (the same bytes), Felucca's state
  refused in plain words, deleted (asked first), the active one downloaded, deleted, uploaded and loaded again (the
  host's log: its working memory restored), the Move's own pads (Track held + pad 7, an empty state) seen on the page,
  the other slots' States; the open project saved by a tap ("Saved", its firmware copies), duplicated into an empty
  slot, opened, downloaded, deleted, uploaded as another and opened (ChoralRoot's copy restored), an upload over the
  open one refused; the phone's States and Projects from the slot menu. Screenshots: `docs/move-page.png`,
  `move-page-editor.png`, `move-page-files.png` and the slots' and Mix & FX's (the States and projects' in build/);
- `tests/store_e2e.sh` (the page's States and projects through their API, the sound read back as in
  `tests/states_e2e.sh`: ChoralRoot 0.43 with core 1, its voicing through its takeover): states saved from the page at
  +3 and +7 (the core's pads with them), state 1 downloaded, deleted, uploaded (the same bytes) and loaded: +3 again;
  refusals in plain words (Felucca's state into ChoralRoot's slot, a file over the active state without `load=1`, a
  state file as a project) and the file browser's own guard on a running firmware's states/; with `load=1` the
  active state replaced and loaded; the open project saved at +5 and downloaded, saved at +7, its file uploaded as
  project 3 and opened: +5 (its copy restored), project 1 again: +7, duplicated (its copies too) and opened: +5,
  deleted with its copies; an upload over the open project refused; no late block;
- `tests/fm1_e2e.sh`: an FM-1 on USB-A (below) on the simulator: libremotefm1's fake unit behind Move Remote's simulated
  USB-A, whose bridge loses what a burst from the unit does not fit (`--fake-fm1`, `/api/test/fm1`): the session, the
  screen and LEDs converge, the unit's keys and MIDI in play the slot, an encoder's change shown on the unit (~120 ms),
  a busy screen with the Move's own LEDs still written, nothing lost, a slot switch followed (a key held across it let go
  on the old slot), an unplug (nothing stuck) and a replug, no late block;
- `tests/fm1wifi_e2e.sh` (needs `make wifisim`; skipped without it): an FM-1 over Wi-Fi (below) on the simulator:
  RemoteFM1 Wi-Fi's own simulator (its firmware's app and TCP server on the image's loopback, its panel driven through
  its standard input) beside the fake USB-A unit: USB-A links first, the Wi-Fi FM-1 is found by the broadcast and takes
  over (the address its answer gave, not paced, the USB-A unit's session ends), frames acknowledged, its keys and
  encoders play the slot (PANEL_EV), a connection it drops comes back, gone: the USB-A unit again (a held key let go),
  its address remembered, and back on Wi-Fi when it returns;
- `tests/fantasy_e2e.sh` (needs `make testcat-v`: Elasticat and Ripple from the site's catalog, signed with the test
  key): the page's `/api/files/audio` (an upload with spaces and brackets in its name; refused: not a WAV, .txt,
  ../, a hidden name, a slash, over 256 MB; a delete), the Move's catalog downloads both, Elasticat in slot 1 reads
  the uploaded WAV through rfm1_host_1, writes its own data/elasticat/ (nothing else), plays the WAV from the page's
  key, Ripple in slot 2 reads its code/ripple/, no late block, both back after a restart (Elasticat from what it
  wrote), and a Move with 300 MB available refuses Elasticat (280 MB) and still loads Ripple;
- `tests/ain_e2e.sh` (audio input, `ain1`): offline, `tools/fwrun --audio-in` against FM1VST's `rfm1_run --audio-in`
  (wasm3) with Elasticat recording a take (FILE x5, REC held, OCT+, PLAY): the same PCM byte for byte, the same
  blocks fed, ui_info `ain:1 lat:1152`, the same take, which is the input sample for sample; on the simulator
  (`--sim-audio-in`: the WAV as the Move's input) Elasticat in slot 1 is told `rfm1_audio_in_info(1, 1152)`, gets
  the WAV's blocks, and its take from the page's panel (data/elasticat/rec/rec-0001.wav) is the input sample for
  sample; Ripple (no ain1) beside it gets nothing and runs; no late block; `--no-audio-in`: Elasticat runs without;
- `tests/files_e2e.sh` (the file browser): list (folders first, the root read-only, the slots' firmwares), mkdir,
  put (a space and brackets, an empty file, 409 then replace=1; audio/ WAV or AIFF only), get (an attachment with its
  name and length), a zip of states/ and of the whole folder that unzip (BusyBox) to the same files, rename (a move;
  refused onto a name, into itself, into firmwares/, a bad name), delete (a non-empty folder only with recursive=1
  and its name), the path rules, links out not followed, listed or zipped, firmwares/ and the root read-only, a
  running firmware's flash and the open project refused with the reason, 256 MB and the 64 MB floor
  (`RFM14_TEST_DISK_FREE_MB`, the test build only), no temporary file left.
- `tests/latch_e2e.sh` (rfm1cs 0.5.4, latch per slot): MIDI mode, latch on, a chord on slot 1 (Felucca); slot 2
  (ChoralRoot) made active: slot 1 keeps sounding, slot 2 plays its own note; latch off: both silent.
- `tests/threads_e2e.sh` (below, "Threads"): core 1: the test firmware's jobs on the core-1 thread, the same PCM as
  with core 1 off; an object from before the threads (dist/move-catalog's FIZZ, when there): its own call depth = the
  one lock, the same PCM; the host with three slots on their threads (Felucca, the core-1 fixture with core 1 on, FIZZ's
  old object or ChoralRoot) and a phrase on track 1: each slot's blocks equal that firmware replayed offline alone on
  one thread (core 1 off) from the host's log of its input, bit for bit; the mix is what the audio thread played;
  `--threads 0` too. `tests/threads_bench.sh [SECONDS]` (not in make test) prints the render split, serial vs threads
  vs threads without core 1 (`BENCH_DIRS`, `BENCH_CH`, `BENCH_NOTES`).
- `tests/cpu2fw_e2e.sh` (when FIZZ's and ChoralRoot's cpu2 builds are among the inputs): the host with both, chords on
  each; core 1 ran jobs for both; each slot's blocks equal the firmware replayed offline with core 1 off, bit for bit.
  `tools/fwrun` prints where a block's time went (audio, tick + idle, frame) and core 1's jobs.
- `tests/sess_e2e.sh` (session view, rfm1cs 0.10.0, 0.11.0: a scene pad from stopped, Shift + a scene's double click, a
  Track tapped stays in the view; 0.12.0: Delete never pastes, Copy held pastes the deleted clip): Felucca on the simulator, Track 1 held + pad 3 (Scale), two
  steps, Menu: the selected clip is white on its pad and framed on the screen; an empty slot pressed is selected and
  not filled, the first note makes the clip; Shift + a slot selects only; a knob touched shows its track's popup;
  playing, a pressed clip is framed at once and plays within a bar (green); a scene pad (column 8) launches its row;
  a playing session view's LED writes a second; step 16 stops every clip; the lit pads are the clips the screen
  draws; Delete held cuts a clip and the next press pastes it; Copy held copies one into an empty slot.
- `tests/projfw_e2e.sh` (a project holds its firmwares, the States: rfm1cs 0.13.0, "Projects that hold their
  firmwares" below): Felucca and ChoralRoot on the simulator, a firmware's memory changed through
  `POST /api/test/poke` (a byte near its end) and read back from the files the host writes. Project 1's copies follow
  its memory; an empty pad is a new project with its slots empty; Felucca changed there has project 2's own copy; an
  emptied slot's copy goes at the project's save; project 1 again: its loading screen comes and goes, Felucca's memory
  swapped back to project 1's, ChoralRoot loaded again; Copy + a project then an empty pad carries the copies; States
  (Track 1 held + pad 7): save, the active state follows the memory, load, copy / paste, delete then paste, a paste
  onto the active state loads it, step 14 New and Undo; the projects' zip holds the copies, the open project's are not
  deleted; restarted with ChoralRoot removed: missing, the project keeps its name and its copy; a version 8 project
  (made from a stored one) leaves the firmwares alone and is version 9, with its copies, after its next save; the open
  project cut moves its copies into the buffer (hidden `.clip.fwK.*`) and a paste writes them for the new slot.
- `tests/states_e2e.sh` (the States and projects hold the working memory, "The working memory" below): first offline,
  `fwrun --snap-test`: each built firmware (ChoralRoot with core 1, Felucca, the core-1 and takeover fixtures; every
  published one in `dist/move-catalog` when it is there) snapshotted mid-chord, written and read back, restored into a
  second instance: the next 1102 blocks of both the same bit for bit. Then on the simulator, ChoralRoot 0.43 with core
  1 PLAYED: its voicing turned with the takeover's knob 1 and read back with the knob only touched (its popup's words,
  through the fake TTS); state 1 saved at +3 at once, state 2 at +7 (their flash images the same bytes), each loaded
  back at its value; New: 0, New + Undo: +7; Copy / paste with the .mem; a state as an older host saved it (no .mem)
  and a .mem beside another image load from the flash; Felucca (one core) in slot 2: its PRESETS turned, its screen
  back with its state; no late block; the active state written again while played (every 4 s here) and not while left
  alone; project 1 / project 2 each bring their ChoralRoot back at its own voicing; quit and restart: as left.
- `tests/loadstop_e2e.sh` (opened while the Move is stopped: stopped, "The working memory" below): ChoralRoot's loop
  played in its takeover (a key, step 1 gets the chord, Play); while it plays project 1 saved and duplicated into 4,
  states 2 and 1 saved; stopped; project 4, project 1 and state 2 opened: the Stop sent, the restored loop stopped,
  no "chord:" line in 5 s, the Move's sequencer stopped; the Move playing: state 2 loaded with no Stop, its loop goes
  on; restarted with project 1's copy as saved while playing: opened at start, stopped and silent.
- `tests/tk_e2e.sh` (the takeover, "Takeover (tk1)"): tkdemo and Felucca on the simulator: Track 1 opens tkdemo's
  takeover (its screen on the Move), a pad reaches it (`rfm1_tk_input`, its LED 255,120,0, its screen "PAD 5"),
  Track 2 (not claimed) MIDI mode and Track 1 back, Shift: ALL claimed and the Track buttons the firmware's, volume
  touched + Back: FM-1 mode, Back + Track 2 / Track 1 leaves and reopens it, Felucca ignores Track 1, unloading or
  replacing tkdemo clears it (build/test-out/tk_*.pbm).
- `tests/midiout_e2e.sh` (Out, below): `tests/rtpmidi_peer.c` as a Mac finds "RemoteFM1 Move" by mDNS, invites both
  ports, syncs the clock; the messages sent from the engine's block (`/api/test/midiout`) arrive as sent (a 1500-byte
  SysEx in two segments), F8 does not; RS, BY; with the fake FM-1 linked on USB-A Out skips it, a plain device there
  gets Out's note.

## An FM-1 on the Move's USB-A

An FM-1 running the RemoteFM1 firmware (0.3.x), plugged into the Move's USB-A port, becomes a panel for the active slot,
as "Use my FM-1" does on remotefm1.com (src/fm1link.c, on the engine's thread; libremotefm1's session).

- **Discovery:** HELLO every 2 s while nothing answers; a "RemoteFM1" proto-2 HELLO_R starts a session (keepalive
  3 s). Lost (unplugged, or nothing for ~2.5 s): every key and button the FM-1 held is let go, discovery again.
- **The FM-1 -> the active slot:** keys (their velocity, and pressure as the firmware's key lean where the unit has
  TOUCH), buttons, encoders and MASTER (the Move's volume, as the page's), the same input the page's panel gives; its
  5-pin MIDI in (MIDI_IN) as MIDI played live (the MIDI FX of the channel's track, then the active slot).
- **The active slot -> the FM-1:** its screen and LEDs. A slot switch on the Move switches what the FM-1 shows and plays
  (a key it held on the old slot is let go there).
- **The link is slow.** USB-A MIDI goes through the Move's surface controller, not Linux: a burst of more than ~200
  bytes from the unit is lost, and the Move's side goes in its SPI transfers beside the LEDs. So the session is
  libremotefm1's slow link (`slow_link`: Bluetooth's pacing): the screen at 8000 B/s (`--fm1-bps`), its tiles reduced to
  4 colours first and made exact once the screen is still for a second, whole screen changes shown at once (HOLD), at
  most 2 KB unacknowledged; Move Remote sends at most 10 USB-A packets a transfer (`--fm1-cap`; ~10 KB/s on the wire),
  so the Move's LED writes keep the other 10. On the simulator: a small change shows on the unit ~120 ms after the
  knob, a page change in ~1-3 s, a busy screen ~3-4 times a second. The unit itself only sends small messages.
- **Status:** `fm1` in /api/status, /api/info and /api/mix (linked, state, name, fw, fps: frames acknowledged a
  second, bps: bytes a second out, lost, sessions, ack times, USB-A counters); the page shows "FM-1 linked" in its
  header (desktop) and says when it links or is lost; so does the Move's own screen (rfm1cs 0.5.4's host popup).
- **On the Move's screen** (rfm1cs 0.5.4 `rfm_move_popup`, on the engine's thread with a redraw): "FM-1 linked" /
  "FM-1 lost", big, for a second, over any screen.
- `--no-fm1` leaves USB-A to Move Remote as before.

### Over Wi-Fi (RemoteFM1 Wi-Fi)

An FM-1 running RemoteFM1's Wi-Fi build (`build.sh --wifi`, identity FM-1_729.., on the same network as the Move) is
the same panel without the cable: libremotefm1's TCP link (RemoteFM1 docs/PROTOCOL.md section 10), in the same session
as USB-A, one FM-1 at a time.

- **Finding it:** "RFM1?" broadcast on UDP 7741 every 2 s while no FM-1 is connected, and sent to `--fm1-addr
  HOST[:PORT]` and to the address last linked (`ROOT/fm1-wifi.txt`). The first that answers and has no other host is
  connected to on the port its answer gives (7740); the one asked for or remembered is taken even when busy. No session
  within 12 s, nothing from it for 12 s, a refused or timed-out connection: dropped (a session restarting on a live
  connection keeps it), and that address left alone for 30 s (the remembered one 4 s).
- **Wi-Fi first:** a Wi-Fi FM-1 found while one on USB-A is linked takes over (the session restarts on it; the USB-A
  unit's session ends by its keepalive); Wi-Fi gone, the USB-A one is the panel again.
- **Not paced:** a reliable link at hundreds of KB/s (494 KB/s measured to the FM-1): exact tiles at up to 20 frames a
  second, 2 in flight. The FM-1's panel events come as PANEL_EV in the same stream. A connection the FM-1 closes (a
  new host replaced us, or it was switched off) is reconnected by the search.
- **Status:** `fm1` gains `link` ("wifi" / "usb"), `addr`, `wifi` (off, searching, connecting, connected),
  `wifi_name`, `wifi_connects`, `wifi_drops`, `wifi_answers`, `panel_ev`; the page's pill says "FM-1 · Wi-Fi" or
  "FM-1 · USB-A" (the address in its tooltip). `--no-fm1-wifi`: USB-A only.
- **Putting the FM-1 on a network, from the page** (`js/movewifi.js`, `fm1link_wifi_*`): with the FM-1 on the Move's
  USB-A, the settings sheet's "Set up the FM-1's Wi-Fi" (or `http://remotefm1.local:7730/?wifi`) opens "FM-1 Wi-Fi":
  the FM-1's state, the networks it hears (its own scan, 2.4 GHz only: bars, a lock), a password (hidden, a show
  toggle) and Join; the FM-1 is followed until it has an address ("Joined X as a.b.c.d. The Move will find it over
  Wi-Fi: unplug it from USB-A"; a warning when the address is on none of the Move's networks); the networks it keeps,
  each with Forget. RemoteFM1's WIFI over the USB-A SysEx link (SCAN, LIST's small pages, SET, FORGET, GET; RemoteFM1
  docs/PROTOCOL.md section 10), one request at a time beside the session (which may run over TCP meanwhile). The
  joined address goes first in the Wi-Fi search.
  - API: `GET /api/fm1/wifi`; `POST /api/fm1/wifi/scan`, `/set` `{"ssid","password","pick","scan_seq"}`, `/forget`
    `{"ssid"}`; 409 with no FM-1 running RemoteFM1 Wi-Fi on USB-A, 400 for a password outside 8..63 characters.
  - The password: only in the POST body, wiped from the request buffer once read, copied once into the SET message,
    wiped again; never logged, never in a URL, never written on the Move. The FM-1 keeps it in its own flash.
  - `tests/fm1wifisetup_e2e.sh`: a fake RemoteFM1 Wi-Fi unit on the simulated USB-A (fm1sim.c `wifi`): the scan's
    pages, bad passwords refused, a failed join explained, a join with the password arriving whole and in no log or
    file, networks kept and forgotten, then RemoteFM1 Wi-Fi's simulator at the joined address linked over TCP.
  - Without the Move: RemoteFM1's `tools/wifi_probe.py setup` (or `scan`) over USB from a computer.

## MIDI out ("Out")

`src/midiout.c` (midiout.h): whole messages the engine's block hands it (`midiout_send`; a firmware's packets through
`midiout_send_pkts`) go through a lock-free ring to the engine's loop (`midiout_poll`), and from there to:

- **Network MIDI:** an RTP-MIDI session (RFC 6295, Apple's session protocol), "RemoteFM1 Move" on UDP 5004 (control)
  and 5005 (data), advertised by mDNS as `_apple-midi._udp` (src/mdns.c: PTR, SRV, TXT beside `remotefm1.local`). On
  the Mac: Audio MIDI Setup > Window > Show MIDI Studio > Network: "RemoteFM1 Move" under Directory, Connect. Over the
  USB-C link (172.16.254.x) and Wi-Fi alike. A session the Mac opens (IN, OK on both ports, CK answered, RS counted,
  BY); up to 4 peers; no journal; the Mac's own MIDI to the Move ignored. `--rtp-port N`, `--no-rtp` (the simulator:
  none unless `--rtp-port`).
- **USB-A:** Move Remote's `mr_usba_send`, the FM-1 link's own path. The surface controller bridges USB-A as one
  cable, so a device or a cable behind it (a hub) can't be picked: while an FM-1 running RemoteFM1 is there (linked
  over USB-A, or it answered HELLO in the last 5 s) Out skips USB-A and the link's traffic is untouched; any other
  device gets Out. `--no-fm1`: USB-A stays Move Remote's, Out skips it.
- Real-time bytes (F8, FA..FC, FE, FF) are dropped unless `--midi-out-clock`.
- `--midi-out`: the active slot's MIDI out to Out, until the routing names who goes there.
- `/api/status` `"midiout"`: `rtp_peers`, `rtp_sent` (messages), `rtp_packets`, `usba` (off, on, fm1), `usba_out_sent`,
  `usba_skipped`, `filtered`, `dropped` (the ring or USB-A's queue full, damaged), `peers` (their names).

## Fantasy CFWs (Elasticat, Ripple)

A Fantasy CFW (FM1VST docs/VIRTUAL-CARTS.md) is a host-only `.rfm1` (`"virtual": true`) that may use up to 512 MiB of
memory and reads the RemoteFM1 folder through the import module `rfm1_host_1` (`"needs": ["fs1"]`). The Move runs
them like the others (wasm2c, the catalog, the slots), with:

- **The folder:** the RemoteFM1 folder's `audio/` (every cart reads it: the samples), `code/<id>/` (the cart's own
  scripts, read: Ripple's `droplets/*.lua`), `data/<id>/` (its own files, read and write: Elasticat's projects and
  settings). Only what its manifest's `folders` lists; `..`, `.`, empty segments, `\`, NUL, bad UTF-8 and symlinks out
  of the area refused. A cart's `code/<id>/` and `data/<id>/` are made when it loads; nothing is installed into
  `code/` (no host does: carts ship their built-in scripts inside the .rfm1, the user adds their own).
- **The file API** (src/fsvc.c; FM1VST core-glue/common/rfm1_host.h, its tools/rfm1_run `--folder` mirrored): STAT,
  LIST, READ, WRITE, DELETE, MKDIR; at most 32 requests in flight, 1 MiB each. The imports run inside a call into the
  module and only check and note a request. Before each `rfm1_tick` (between two calls) the engine's thread hands the
  new requests to the storage module's **file thread** (`rfm1-files`: never the engine or the audio thread, never behind
  a firmware load) and copies the finished ones into the module's memory (at most 1 MiB a device ms); so a request is
  never answered during a call, and its buffer is written only between calls. A cart freed while its requests run is
  kept until they return. `fs_changed` moves when the page adds or removes a sample.
- **Memory:** wasm2c (explicit bounds checks) callocs the whole declared memory, which the host's mlockall makes real,
  locked RAM at once: Elasticat 280 MB, Ripple 16 MB. Before an instance is made the host compares `memory_pages`
  (build_fw.sh writes it in the `.json`) with MemAvailable less 160 MB and refuses with "<id> needs N MB of memory, M MB
  free" instead of being killed. Three Elasticats (840 MB) fit the Move's ~1.7 GB.
- **The catalog:** rfm1info accepts `rfm1_host_1`'s four imports only with `virtual` and `fs1`; build_fw.sh passes
  wasm2c's import instances (WASI's, rfm1_host_1's, or none: Elasticat imports no WASI at all) to its instantiate;
  catalog_fw.sh runs a virtual cart with a fresh RemoteFM1 folder for each engine (build/testaudio's WAVs when there:
  a scratch copy of a few samples, never in git), and its PCM and what it wrote must be wasm3's (rfm1_run
  `--folder`), byte for byte; index.json marks it `virtual` with `needs`, `wants`, `folders` and `memory_mib`.
  `/api/firmwares` marks it `virtual` for the page, and the core's list (`rfm_move_fw_t.virt`, rfm1cs 0.5.4) for the
  Move's own browser: its diamond badge and step 16's filter.
- **Getting samples on:** the settings sheet's **Files** section, **Add samples** (WAV or AIFF into audio/), or the
  page's file browser (below, "Files"). The older `GET /api/files/audio`, `POST` / `DELETE /api/files/audio/<name>`
  stay (WAV only, a plain name ending .wav).
- **Checked offline:** `tools/fwrun FW.rfm1m --folder DIR [--press PLAY@1000] [--ui]`: the same run as rfm1_run's
  `--folder` (each request one device ms late), with what the file API did and the cart's ui_info.
- **Audio input (`ain1`, VIRTUAL-CARTS.md section 8):** a cart whose manifest lists `ain1` in `wants` or `needs` and
  that exports `rfm1_audio_in` gets the Move's input: Move Remote's rt thread hands each SPI block's input pcm (what the
  Move's SPI mailbox carries: its line in / mic) to the host (`mr_set_audio_in_sink`, Move Remote's `in-process` branch); a lock-free ring
  takes it to the engine thread, which gives each such slot `rfm1_audio_in_info(1, latency)` when the state changes
  (before the first block), then the block through `rfm1_audio_in` right before its `rfm1_audio` (rfm1_run
  `--audio-in`'s order; the buffer `rfm1_alloc`'d after init, as rfm1_run does). The audio thread pushes one input
  block for each output block it pops and the engine pops one for each it renders, so the input queued plus the
  output queued stays constant; the engine holds it at the lead + 2 blocks (dropping input it does not need, or
  silence while a firmware's start has filled the output ring), so the **round trip is fixed: 1152 frames (26 ms)**
  at the default lead of 5 (`--lead N`: (N + 4) x 128): the queues (896 frames) plus the SPI's block each way (256,
  an estimate to measure on a Move). Elasticat uses it to line its takes up (`rshift`) and shows `ain:1 lat:1152` in
  its ui_info; its INPUT machine is the monitor. A cart without ain1 gets nothing. Unloading (or the input stopping
  for half a second) tells the cart `rfm1_audio_in_info(0, 0)`. `--no-audio-in`: none. Status: each slot's `ain`,
  `ain_lat`, `ain_blocks`, `ain_peak`; `ain_latency`, `ain_under`, `ain_dropped`. On the simulator `--sim-audio-in
  F.wav` is the Move's input (looped; else its quiet 440 Hz tone).
- **The compiled objects:** `tools/build_fw.sh` builds a module that exports `rfm1_audio_in` with `-DRFM1M_AIN1`, which
  adds `rfm1m_ain()` (src/rfm1m.h) beside `rfm1m_get()`; every other object is byte for byte what it was (Felucca,
  ChoralRoot, Ripple checked; `make catalog` rebuilds them all since its build stamp covers fwglue.c and
  build_fw.sh, with the same bytes but Elasticat's). `tools/fwrun --audio-in F.wav [--audio-in-latency N]
  [--audio-in-once]` runs the same as rfm1_run's.

## Schwung FX (phase 2b)

`src/fx.c` (Schwung's own headers, third_party/schwung-api, MIT) lists the modules at `--fx-dir` (default
`/data/UserData/schwung/modules`: `midi_fx/<id>/` with `dsp.so` and `audio_fx/<id>/` with `<id>.so`, or module.json's
`dsp`), plans each module's parameters and pages from its module.json at scan time, and gives them to rfm1cs's FX
setup (`rfm_move_fx_modules`) as 0..127 values or word lists with their pages (rfm1cs 0.5.0, "FX parameter pages"):

- **Planning** (fx.c `plan`, as Schwung's own knob grid plans: its docs/MODULES.md "Shadow UI Parameter Hierarchy"):
  `capabilities.ui_hierarchy`'s levels walked from `root`, depth first: a level's `knobs` (entry n is knob n % 8 of
  its page n / 8, `null` a gap, trailing nulls dropped), then its params on no knob after the last knob, 8 a page
  ("Name", "Name 2", ..); then each `{"level": ..}` link's level after the level that links it, once. A param's
  metadata (type, min, max, step, default, unit, options, short_name) from the level's entry, else `chain_params` by
  key; a key on several levels is one parameter, shown on each of its pages. A parameter's index (the core's, the
  project's) is its first appearance in that walk. No hierarchy: `chain_params` in order, pages of 8. At most 128
  parameters and 16 pages. Not planned (as in rfm1cs 0.5.0): `list_param` / `items_param` browsers, `menu`,
  `visible_if`, child levels, `ui_chain.js`, types other than float / int / enum / mode / bool.
- **Labels and readings**: `short_name`, else the name squeezed to about 5 characters (Schwung's per-word mnemonics,
  then initials and the last word, a trailing number kept, then cut: `fx_abbrev`); `lo` / `hi` / `decimals` / `unit` as
  Schwung's formatter reads them ("%" in 0..1 shows x100, `sec` as "s", decimals from `display_format`, else the step).
- **Values**: 0..127 (an enum: the option's index) by parameter index; `fx_wire` gives the module its value: a float
  `min + (max - min) * v / 127` snapped to its step, an int likewise rounded, an enum its option's text
  (`options_as_string`) or index; set by key. Values a version 5 project did not keep (255) are left to the module.

Wet / dry parameters of an audio FX default to 100 % wet, 0 dry (a send's FX). The modules get a
host_api_v1_t with sample_rate, frames_per_block, log, get_bpm, get_clock_status and get_beat_position (the
sequencer's tempo and transport) and NULL everything else, with a tail of NULLs past the struct.

- **MIDI FX:** the sequencer's MIDI (rfm1cs tags it with its track 1-16) runs through MIDI FX 1, 2, 3 in series; each
  acts on the tracks in its filter (one track or All) and passes the others on; `tick` once a block, its notes taking
  the slot's track (its one track, else the last it heard). Then each firmware's 16-track filter. Live pads and the page
  play the active slot directly (through the MIDI FX of their channel's track); a latched note (rfm1cs 0.5.4: latch
  per slot) stays on the slot it was played on when another one becomes active, and its note off goes there
  (`rfm_move_act_t.dest`); the host's switch lets go of what the panel holds (keys, buttons) and nothing else. On the engine thread, beside `rfm_move_seq_advance`.
- **Audio FX:** each firmware's block, after its level and pan (a balance; 64 is the old mix bit for bit), goes to the
  mix and, times its send, to the 3 buses; each bus is processed in place by its module (int16 stereo, 128 frames) and
  added back; then the limiter as before. A bus whose sends are all 0 adds nothing: the output is bit for bit the one
  without FX (tests/unit.c).
- **Loading:** FX_LOAD from the core opens the module on the storage thread (dlopen, init, `create_instance`); the
  engine swaps it in and sets the core's values. First, once per module, `remotefm1-host --fx-probe` loads it in a
  child process: a module that crashes as it loads is refused (`rfm_move_fx_refused`: "Too busy" on the Move; the
  page shows why) and the host keeps running. **A crash later, inside process_block or tick, still takes the host down**
  (as any in-process plug-in would).
- **CPU:** each module's time is measured (status `mfxN_cpu`, `afxN_cpu`, the page); one over `--fx-budget` (permille
  of a core, default 200) two seconds running is refused the same way.
- **State:** the core's project (version 6) keeps the modules by name, their 128 values, the filters, pans and sends (levels:
  the library). Each module's own `get_param("state")` goes beside the project, `move/projects/move-project-NN.fx.json`,
  written with each project save and given back (`set_param("state")`) when that project's modules load.
- Modules are listed at start: one installed while RemoteFM1 runs shows after a restart.

## Projects that hold their firmwares; States (rfm1cs 0.13.0)

The owner's "Project should save firmwares": the host sets `RFM_MOVE_CAP_PROJFW` (`rfm_move_fw_caps`, before the
projects), so a project keeps its three firmwares and each one's memory, and a firmware's states are a layer on the
Move (the core's README, "Projects that hold their firmwares; States").

- **A project's own copy of each firmware's memory** (`src/projmem.h`): `move/projects/move-project-NN.fwK.bin` with
  its `.json`. The host knows whose memory each slot holds (the project it was loaded for, or taken as) and writes the
  copy whenever it writes that memory anyway (`slot_write`: the lazy autosave, a firmware leaving its slot, the exit)
  and when the project is saved, only when the copy is behind it (a 1 MB file is not written again unchanged). A save
  whose project holds a slot empty drops that slot's copy; a missing firmware's copy stays (the project still names
  it). `states/<id>/flash.bin` stays the firmware's last memory anywhere: a firmware loaded by hand starts from it.
- **Opening a project** (a pad, or the last one at start): first every slot's memory, synced, goes into the outgoing
  project's copies; then for each slot the core's `PROJECT_FW`: the same firmware there with this project's memory
  already: nothing; the same firmware: only its memory swapped (a new instance of the same object, as a state load);
  another one: loaded with the project's copy (none: `flash.bin`; none: a fresh memory); "": unloaded; not installed:
  the slot left empty, "Felucca not installed" on the Move, the project's copy kept for when it is installed again. The
  slots load on the storage thread one after the other; the core's loading screen shows the progress
  (`rfm_move_proj_fw`). The memory loaded is compared with the firmware's stored states (their json's hash first, then
  byte for byte, on the storage thread): an equal one is the active state, else none. A project from before (blob
  version 1..8) leaves the firmwares as they are; they are its own from then, and its next save makes it version 9
  with copies of them. At start a version 9 project loads its firmwares this way instead of the library's slots.
- **The project buffer**: Copy + a project copies its copies into the buffer (`.clip.fwK.*`), a cut moves them there,
  a paste writes them for the new slot, a delete drops them. "Duplicating a project": Copy held + a project, then an
  empty pad.
- **States** (Track 1..3 held + the top row's pad 7): the active state (the last loaded, saved or pasted onto, or the
  one the project's memory equals) saves itself: every write of the memory also goes to `state-KK.bin` when it is
  behind. Before another state loads, the memory is synced into the active one. Copy and Delete put a state's bytes in
  one buffer (RAM, one firmware's; Delete removes the file), a paste writes it (onto the active one: loaded too). Step
  14 New keeps the memory and the active state in RAM (one per slot) and restarts the firmware with a fresh memory, no
  state active; Undo puts both back. The undo goes when the firmware in the slot changes. Since the working memory
  (below) a state and a project's copy are the firmware as it was, not only its flash.
- Status (`/api/status`): `proj_open` (the project whose copies the slots' memories are), `proj_cur` (the core's open
  project), `pfw_loading`, `pfw_shown` / `pfw_ended` (its loading screen came / went), `projects`, `states_open`, and
  per slot `pcproj<s>`, `stcur<s>` (the active state the host autosaves), `cstcur<s>` (the core's), `want<s>` (the
  open project's firmware), `undo<s>`.
- Upgrading from an earlier host: nothing in `states/` is deleted or rewritten; the projects load as they were (a
  version 8 project keeps the firmwares that are loaded) and get their copies at their next save.

## The working memory

A firmware's flash image alone is not the firmware as it is: most firmwares write their flash on their own schedule
(ChoralRoot only once nothing changed for a while and nothing sounds; its session only on SAVE held), so a state saved
right after a change held the old sound, every state of the owner's ChoralRoot and Melodee loaded "the same", and a
load re-applied whatever the flash last held (ChoralRoot's session: its voicing among it). So a state and a project's
copy also hold the firmware's working memory, and loading brings that back exactly.

- **What is kept** (`src/fwrt.h` `fw_snap_t`, `src/memsnap.h`): the module's whole linear memory (8 or 16 MB: the
  flash image is in it too), its globals (wasm2c's instance struct; the core-1 twin's too), and the host's view of the
  instance: its device clock, the takeover's and readout's records, the input queued and not applied yet, the keys
  held, the place of the host's own allocations in its memory (the scratch block, core 1's stack: the same in every
  instance, checked). Taken on the engine's thread between two blocks (core 1's job in flight finished first); a
  restore replaces `fw_boot` on a new instance of the same object (`_initialize`, the layout, the scratch, core 1's
  stack as always, then the memory and globals copied over: no `rfm1_init`, nothing re-applied from the flash). After
  it the firmware's takeover is told it is not shown (as after a boot, until rfm1cs shows it), and what was held when
  it was taken is let go (the panel's keys and buttons; CC 123 on every channel, the host's All Notes Off).
- **Opened while the Move is stopped: stopped** (the owner's "Loading a new project is starting playback when the
  Move's playback is stopped"). A snapshot is taken whenever the copy or the state is written, so one taken while the
  firmware's own loop or sequencer ran (a save, a project left or a copy duplicated while playing, the 30 s autosave
  while played) holds it running, and restored exactly it ran on, the Move stopped: ChoralRoot's loop played its
  chords after "project N open". So after every restore (a project opened, at start too; a State loaded, pasted onto
  the active one, Undo of New; a state file from the page), when the Move's sequencer is stopped, the instance also
  gets a MIDI Stop (FC), the one the Move sends every firmware as its sequencer stops: a firmware that follows the
  Move's Start / Stop (ChoralRoot's loop with MIDI Clock OUT, the default, or IN) stops, nothing else of its memory
  changes (the log: "its saved working memory; the Move stopped: Stop sent"). The Move playing: nothing is sent (the
  core's own Stop and Start go out as they always did). Cured at the restore, not at the snapshot: the snapshot stays
  the firmware as it was (a save does not stop what plays), and the copies already saved while playing (the owner's)
  open stopped too. Not covered: a firmware whose own transport ignores a Stop in (ChoralRoot with MIDI Clock OFF and
  its takeover not shown) runs on as it was saved.
- **Which firmwares** (`src/snapsafe.h`, made by `tools/snapsafe.py`, `make snapsafe`): a snapshot of the memory and
  the globals is the whole state of a module whose tables and data segments never change after start-up; the tool
  reads each .rfm1 (its instructions) and keeps the SHA-256 of those that qualify, with their globals' place in
  wasm2c 1.0.32's instance struct (the host also checks, at each snapshot, that the memory's own record follows them
  there). Every published firmware but the two Fantasy CFWs qualifies: 116 of the catalog's 118; Elasticat and
  Ripple (`rfm1_host_1`: their open files live in the host) keep the flash-only states, as does any module the table
  does not know (a firmware published after this host: `make snapsafe` from the catalog's cache, then a release).
  No `.rfm1m` changes.
- **The files**: `state-NN.bin` stays the flash image (an older host loads it as before); beside it `state-NN.mem`
  (and `move-project-NN.fwK.mem` beside a project's copy, `.clip.fwK.mem` in the project buffer), its `.json` gains
  `"mem"` (the .mem's SHA-256). The .mem names the module's SHA-256 and the SHA-256 of the image it goes with: it is
  restored only into that very module booting that very image (else the flash alone, and the log says why: no .mem,
  another build, another image, damaged). Its pages: all-zero ones a byte, the rest LZ4's block format (our own
  encoder and decoder, `src/memsnap.c`; no third-party code), a checksum at the end. Sizes (the build's `fwrun
  --snap-test` mid-chord): ChoralRoot 0.43 8 MB as ~230-340 KB, Felucca 1.8.2 8 MB as ~250-430 KB, Melodee 1.6.1 16 MB
  as ~200 KB; the 116 published (and ChoralRoot 0.42, still in `dist/move-catalog`): 41 KB .. 1.06 MB, median 290 KB.
- **When the active state and the open project's copy are written** (with the memory): (a) before another state or
  project is loaded over it (and before a Copy or Delete of the active state, New, a project copied or cut); (b) when
  the firmware leaves its slot or the host quits; (c) when the firmware writes its flash, as before (the 2 s
  autosave); (d) every 30 s at most (`MEM_AUTOSAVE_MS`) while the firmware is being played (a player's input since the
  last write: keys, buttons, knobs, MIDI but its clock, the takeover's input) and nothing else wrote it: a firmware
  left alone, or a loop playing by itself, writes nothing. Saving another state is not a load: the active one keeps
  what it holds unless (c) or (d). A project's own save writes its copies when they are behind on the flash, as
  before.
- **No late block**: a snapshot copies the whole memory on the engine's thread (on the simulator 0.5-2 ms for 8 MB;
  on the Move's A72 a few ms, twice that for 16 MB), so the ring is first filled ahead by twice what the last copy took
  (at least 1.5 GB/s's worth), as `preroll` does before a start; the file is made on the storage thread. A restore's
  copy comes after a load's preroll (~80 ms of the other slots' sound). The slot itself is silent for as long as a
  load always was.
- Status: per slot `snaps<s>` (snapshots taken), `snapus<s>` (the last one's cost), `restored<s>` (the running instance
  came from one), `snapable<s>`; `snap_max_us`, `restore_max_us`. `POST /api/test/peek` (--test-api): bytes of a slot's
  memory, or the FNV-1a of its screen.

## Threads

The Move has four Cortex-A72 cores: core 3 is the SPI's (Move Remote's `mr-audio`, SCHED_FIFO 70), cores 0..2 are
ours. Until 2026-11-19 one engine thread rendered all three slots one after another, so their costs added up (the
owner's Move: FIZZ in slot 1 and SLOOP 3 in slot 3, the engine thread at 94 %, ~90 late blocks a second).

- **A render thread per slot** (`rfm1-slot1..3`, pinned to cores 0, 1, 2), **fork-join per block.** The engine's thread
  (`rfm1-engine`) does everything that touches a firmware between two blocks, exactly as before: the sequencer and its
  MIDI, the panel, inputs queued at the block's frame, the active slot's MIDI out, the flash, the screen, loads. Then
  it hands block N to every sounding slot's thread at once, waits for them all, and mixes in slot order (levels, pans,
  sends, the FX returns, the limiter) into the ring. A slot's instance is touched by its render thread only while the
  engine waits, and by the engine only between blocks: each firmware gets the same calls in the same order as on one
  thread, so its sound is the same bytes (tests/threads_e2e.sh replays each slot offline and compares). A block now
  costs the slowest slot plus the mix, not the sum. `--threads 0`: the old way, every slot on the engine's thread.
- **The runtime per thread.** wasm2c 1.0.32 keeps its trap state in process globals: the jump buffer a trap longjmps
  to, the unwind target, and `wasm_rt_call_stack_depth`, which every wasm function increments and decrements.
  `tools/w2c_mt.sh` patches the host's copy of the runtime (every substitution checked): the trap state is
  `__thread`; firmware objects built from now on (`tools/build_fw.sh`) count their depth in a thread-local
  `wasm_rt_call_stack_depth_tls` (initial-exec TLS: one load more per call) and say so (`rfm1m_caps`). The generated
  C is the same, so is the sound (bitident).
- **Objects built before that** (the published Move catalog's, whose depth counter is the process global): on Linux
  the host points each loaded object's GOT entry for that global (its one `R_AARCH64_GLOB_DAT` relocation, checked to
  hold the global's address) at a counter of that object's own, so different old objects run on different threads at
  once; two slots running the same old object take turns (its lock). Checked on all 118 objects of
  dist/move-catalog. Anything unexpected: every call into that object under one process-wide lock (also
  `RFM14_LEGACY_LOCK=1`). The status says `"obj":"mt"`, `"old"` or `"old-locked"` per slot. A `make catalog` rebuild
  turns them into `mt` objects (nothing else changes; the catalog must then be published again).
- **Real time without root.** The engine's, the slots' and core 1's threads ask the module's own `bin/heal` (Move
  Remote's setuid helper, installed by schwung-heal, the one that already gives `mr-audio` SCHED_FIFO) to promote them,
  exactly as Move Remote asks for its audio thread, then lower themselves to their own priority (slots and core 1
  FIFO 60, the engine 58: lowering needs no privilege). No helper: normal priority, as before. `--no-rt-threads`:
  never asked.

### Core 1 (cpu2): dual-core firmwares

On the FM-1, FIZZ and ChoralRoot render part of their voices on the chip's second core: each control block core 0
updates the shared state, hands one job to core 1 ("voices 8..15 into bus B"), renders its own part, waits, and sums
both parts in a fixed order, so the samples are one core's. **The .rfm1 format has no core 1** (RFM1-FORMAT.md: no
threads, every export from one thread): their .rfm1 builds run the job inline (FIZZ's "simulated" back end,
ChoralRoot's stub core), so a host cannot split them by itself. This host offers an optional extension, **cpu2**:

| the module | what |
|---|---|
| imports `rfm1_cpu2_1.run(fn, arg) -> i32` | 1: handed over (core 1 runs the job now); 0: not (run it yourself) |
| imports `rfm1_cpu2_1.wait() -> i32` | 0: the job is done; else it was not (run it yourself) |
| exports `rfm1_c1_stack(top)` | sets its stack pointer and limits to `top` (Emscripten: `emscripten_stack_set_limits` and `_emscripten_stack_restore`) |
| exports `rfm1_c1_call(fn, arg)` | calls `fn(arg)` through its function table |
| manifest `"wants": ["cpu2"]` | (informative) |

- The host makes a **twin** of the instance: the same module instantiated again (its own globals, so its own stack
  pointer; its own table) whose linear memory is the instance's (the same bytes). It gives it a 256 KB stack
  `rfm1_alloc`'d from the module (`rfm1_c1_stack`) and a thread of its own (`rfm1-c1-N`, cores 0..2). `run` hands the
  job over (sequence numbers, a short spin, then a futex), the slot's thread goes on with core 0's part, `wait` joins.
- The rules are the FM-1's: between run and wait, core 0 leaves the job's data alone; one job at a time; the job may
  not ask for core 1 itself (the twin's `run` answers 0). A job's output to stdout is dropped. A trap in a job: `wait`
  answers -2 (the module's fallback renders it) and core 1 stays off for that instance. A memory the twin shares never
  grows (`memory.grow` answers -1; .rfm1 memories are fixed anyway). Hosts without cpu2 (wasm3 on iOS / Android, the
  web) give imports that answer 0: the module runs every job itself, as today.
- **Without an import (FM1VST's shim, its docs/RFM1.md "cpu2"):** RFM1-FORMAT.md refuses a module that imports
  anything but WASI, so a firmware's published .rfm1 cannot import `rfm1_cpu2_1`. FM1VST's `fm1_add_rfm1(... WANTS
  cpu2)` exports `rfm1_c1_stack`, `rfm1_c1_call` and `rfm1_cpu2() -> ptr`: a record `{u16 version 1, u16 size, u32 run,
  u32 wait}` naming two entries of the module's function table that its `rfm1_cpu2_run` / `rfm1_cpu2_wait` call
  through, as built the one-core answers (run 0). `tools/build_fw.sh` builds such a module with `RFM1M_CPU2_REC`:
  after `_initialize` the host puts its own run and wait (the functions an importing module gets) into those two
  entries of the instance (fwglue.c `c1_hook`, `rfm1m_cpu2_t.hook`, `RFM1M_CPU2_ABI` 2); the twin's table stays the
  module's. Every other host runs the same file as before. A record not as described: core 1 off, the module runs
  its jobs itself.
- **What a firmware build needs:** that shim (`WANTS cpu2`) and its core-1 back end calling FM1VST's
  `core-glue/common/rfm1_cpu2.h` (ChoralRoot from 0.42 + ad53bc26: `cr_cpu1.h`; FIZZ 0.13.0: RemoteFM1's
  `tools/rfm1/fizz/fizz_unit.c`, the voices' split of FIZZ's firmware). Nothing else changes; with core 1 off the
  samples are the same (`tests/cpu2fw_e2e.sh`).
- `tools/build_fw.sh` builds such a module with the twin's glue (`rfm1m_cpu2`); `rfm1info --check` allows the two
  imports. `tests/cpu2fix/` is a test firmware written this way (16 voices, half on core 1, a scratch array on the
  stack): 22.9x real time with core 1 off, 43.5x with it (the simulator), the same PCM. `--no-cpu2` (or
  `RFM14_CPU2=0`): never.

### The numbers

`/api/status`, once a second: per slot `render_avg_us` / `render_max_us` (this slot's blocks; the budget is 2902 us
a block), `obj`, and for core 1 `c1`, `c1_jobs`, `c1_inline`, `c1_cpu`, `c1_wait_cpu`, `c1_worst_us`; and
`threads`, `block_avg_us` / `block_max_us` (a whole block: the slots and the mix), `block_cpu`, `par_cpu` (the
fork-join), `tcpu` (each thread's CPU, % of a core: `rfm1-engine`, `rfm1-slotN`, `rfm1-c1-N`, `mr-audio`, ...). The log
says the same every 10 s:

```
engine: blocks 817/4653 us (avg/max of 2902), slot1 fizz 764/4610 us (old object) slot2 choralroot 36/116 us (old object)
  slot3 sloop3 32/232 us (old object); slots on their threads; cpu {"rfm1-engine":2.9,"rfm1-slot1":29.8,...}
```

Per slot also `split` (where its blocks' time went, us a block: `audio_us` rfm1_audio, `tick_us` tick + idle,
`frame_us` frame, `frames` a second and `frame_call_us` each).

**Does FIZZ fit?** A slot fits while its `render_avg_us` stays under ~2600 us (2902 less the mix and the hand-over)
and its spikes are absorbed by the ring (5 blocks, 14.5 ms). On the simulator (an M-series Mac's Docker; the Move's
A72 is ~3.5x slower: the owner's Move ran the set below at 94 % where the simulator took 26 %), FIZZ 0.12.0-beta.19
with a six-note chord rendered a block in ~760 us on average (spikes 4-5 ms): ~2.7 ms on the Move, a whole block.
**The split showed why: 95 % of it was frame()** (plutovg drawing its screen: ~3.7 ms a call, 67 a second), its
voices ~30-100 us. Core 1 (cpu2) cannot help that. Two changes did (tests/threads_bench.sh, FIZZ + ChoralRoot +
SLOOP 3, slots on their threads):

| | FIZZ render avg / max (us a block) | its frame() | all three, a block |
|---|---|---|---|
| 0.12.0-beta.19 (catalog), frames every 15 ms | 765 / 4648 | 3762 us x 67/s | 818 us |
| 0.13.0 dev build (the unit's renderer, cpu2, rd1), frames every 15 ms | 251 / 1258 | 707 us x 67/s | 327 us |
| the same, frames 33 ms on screen, 200 ms behind (default) | 186 / 1312 | 748 us x 30/s | 275 us |

So ~1 ms a block on the Move for all three (from ~2.9 ms), FIZZ's longest block ~4.5 ms (absorbed by the ring).

## Frames

The ABI's clock calls frame() every 15 ms and idle() between (fm1core.h); frame() draws the screen, idle() keeps
the input and the LEDs. The Move shows one firmware at a time and rfm1cs sends its OLED at most every 33 ms, so the
host calls frame() every **33 ms for the slot on screen** and every **200 ms for the others** (the page's small
screens; iOS already holds frames back up to 200 ms, so every firmware takes it). tick() still runs every ms, so the
sound, the sequencers and MIDI keep their device time. `--frame-ms A[,B]` (15,15: the ABI's own). The rate is an
input (`FWI_FRAMES`, logged), so `fwrun --replay` calls frame() as the host did: the slots stay bit-identical to their
replays. What it changes: a firmware that acts on the panel in frame() alone (rather than tick / idle) sees a knob
or button up to 33 ms later on screen, and a background slot's screen and readouts move 5 times a second.

## Readouts (rd1)

A firmware built with FM1VST's `WANTS rd1` exports `rfm1_readout()`: one 48-byte record (seq, control, kind, flags,
ms10, frac, title, value: FM1VST docs/RFM1.md "Readouts (rd1)"). `tools/build_fw.sh` builds such a module with
`RFM1M_RD1` (fwglue.c `rfm1m_rd1`). After each frame the engine reads the active slot's record (`fw_readout`: no call
once its place is known) and, when its seq moved, gives it to rfm1cs (`rfm_move_readout`, 0.5.7+: the knob's popup,
the name then the value; a sound's name; a message). A slot switch clears them (`rfm_move_readouts_clear`) and skips
what the new slot said while behind. The page: `/api/info`'s `"readout"` (seq, control, kind, flags, frac, title,
value). `tests/rd1_e2e.sh`: ChoralRoot's K1 (Move knob 5) turned: "Voicing" "+1" on the page and the popup on the
Move's screen (build/test-out/rd1_k1.pbm); PRESETS: the sound's name; a slot switch: none.

## Takeover (tk1)

A firmware built with FM1VST's `WANTS tk1` brings its own Move interface beside FM-1 mode (FM1VST
docs/RFM1-TAKEOVER.md; rfm1cs-abletonmove 0.7.0, its README "Takeover"): it draws the Move's 128 x 64 screen and the
LEDs of the parts it **claims** (screen, pads, steps, knobs, jog, volume, buttons, Track buttons) and gets their raw
input. It is a slot's layout (rfm1cs 0.8.0): **Track n held + pad 2** (the layout menu; pad 2 is lit only when the
firmware has one) opens it ("<name>, takeover" shown and said), the menu's pad 1 goes back to FM-1; **volume touched
+ Back** always leaves it. While a Track is held its menu takes the pads from the takeover. Unclaimed parts keep their
meaning (the Track buttons the slots). A slot switch leaves it; the slot comes back in its own layout.

The module exports `rfm1_tk`, `rfm1_tk_input` and `rfm1_tk_state`; `tools/build_fw.sh` builds such a module with
`RFM1M_TK1` (fwglue.c `rfm1m_tk1`). After `rfm1_init`, fwrt asks for the record once (`fw_boot`: version 1, size at
least 1468, in memory, a printable name; else no takeover). The engine reads each slot's record after every block it
rendered (its frames and its `rfm1_tk_input` calls) and on its boot, and tells rfm1cs what moved: the claims and name
(`rfm_move_set_takeover`), the screen (`scr_seq`: `rfm_move_tk_screen`), the LEDs (`led_seq`: `rfm_move_tk_leds`),
the words (`say_seq`: `rfm_move_tk_say`, interrupting when `say_flags` bit 0). The core's `TK_INPUT` and `TK_STATE`
actions go to the slot's `rfm1_tk_input` / `rfm1_tk_state` as queued inputs (`FWI_TK_INPUT`, `FWI_TK_STATE`: in time
order with its ticks, logged for `fwrun --replay`). A firmware unloaded or replaced: claims 0 first (the core keeps a
slot's takeover until told). The other slots keep running; MIDI routing is unchanged. `/api/status`: `move_mode`
(4 the takeover), `tk_shown` (the slot, -1 none), each slot's `tk` (claims), `tk_name`, `tk_in`, `tk_states`; with
`--test-api`, `move_pads` (the pads' colours as last sent).

`tests/tk_e2e.sh` runs tkdemo (`tests/tkdemo/tkdemo-1.0.rfm1`: FM1VST's `tools/rfm1/tkdemo`, GPL-3.0-only, kept in git
like `tests/cpu2fix`, built into `build/tkdemo` by `make`).

## Screen reader

RemoteFM1 speaks with Schwung's screen reader (Global Settings > Screen Reader: on, before RemoteFM1 is launched; its
engine, speed, pitch, volume and Speak Delay are the user's). Schwung's own modules call `host_send_screenreader`, which
the shim hands to `schwung-tts`, Schwung's speech program, and mixes into Move's audio. A standalone tool has neither:
Move's software (the shim with it) is stopped while it runs. So `src/announce.c` does the shim's part through the same
contract, Schwung's `src/host/tts_shm.h` (version 1, restated field for field):

- it maps `/schwung-tts` (the shim's segment, left in `/dev/shm`; created 0666 when missing), starts
  `/data/UserData/schwung/schwung-tts` as its child (the sidecar leaves when its parent does, as with the shim);
- the sidecar loads the user's settings (`config/screen_reader_state.txt`, `config/tts.json`) and publishes them; the
  host only restates them with each text, so nothing is changed or saved;
- each utterance goes into the request block (a seqlock) and cuts the one playing; the ring (44.1 kHz stereo) is mixed
  over the firmwares in `engine_audio` (the audio thread; lock-free, as the shim's `tts_client.c`).

**When** (rfm1cs 0.6.12): RemoteFM1's own **Speech** mode, the first row of Global Settings (Sequencer mode: Shift +
step 2): **Off**, **On** (always), or **Jog** (only while the jog wheel is touched: a touch says the screen as it is,
then every word while held; let go, silence and what was playing cut off). Saved with RemoteFM1's settings; never
chosen, its default follows Schwung's screen reader (on: On, so a blind user is never locked out; off: Off). Its change
is said in any mode ("Speech off"). The sidecar runs whenever it is installed, whatever Schwung's setting: each request
says "enabled" (the user's engine, voice, volume, speed and pitch restated as they are). With Schwung's own setting off
the sidecar then saves its `screen_reader_state.txt` on (Schwung's `tts_engine_*.c`), so the host puts that file back
as it was once the sidecar has acted, and again when it stops. Jog's touch also sends a firmware that speaks for itself
the protocol's WHERE (its own screen said). The status: `speech_mode` (0 Off, 1 On, 2 Jog), `speech_open`. Options:
`--screen-reader on|off|auto` (the Move: auto; on: the default On; off: no sidecar at all; the simulator: off),
`--tts-sidecar`, `--tts-shm`, `--tts-config`. `tests/speechmode_e2e.sh`: the three modes and both defaults.

What is said (Schwung's conventions: "label: value", views named as they open, debounced as the shim does with
`tts.json`'s `debounce_ms`, 300 ms by default; a value replaces the value before it, events are joined):

- entering ("RemoteFM1. FM-1 mode, Slot 1, Felucca"), the slot and its layout on a Track tap ("Felucca, Drum layout"), the projects view (the firmware slots,
  Projects, FX setup and its pages), the firmware selected (with "not installed" / "update ready" / "installed" in the
  catalog; its description when the jog turns to it), Favorites / All / Catalog, the filter, States, latch, the slot
  played ("Playing, Slot 2, ChoralRoot");
- a knob touched in FM-1 mode (its name, as the firmware labels it), a readout (rd1: "Voicing: +1"), the volume,
  a note layout's CCs ("CC 16: 66"), octave, channel and layout;
- loading, loaded or failed, downloading, installed or failed, states saved / loaded / deleted, the FM-1 linked or
  lost, and leaving (asked for at the exit gesture; Move Remote stops the audio a moment later, so it is cut short).

**A firmware's own words** (the FM-1 screen reader protocol, [charlesvestal/fm1-reader](https://github.com/charlesvestal/fm1-reader),
`src/fmreader.c`): while speech is on, the firmware played gets the protocol's HELLO every 2 s (BYE when another slot is
played). One that answers (most of the catalog's: AMB-1, FoMni-1, LOOM-8, CTL-1, ..) says what its screen shows; its
words are put in words by its words file (fetched once from the link it gives and kept in `<root>/words/`; without it,
the common rules: `src/fmwords.c`, a port of fm1-reader's `web/words.js`) and spoken after RemoteFM1's own. NOW cuts off
what is being said, KEEP is heard whole. Then, in FM-1 mode, the core's words for the firmware's knobs, readouts and
buttons step back (a knob's name on touch stays). The protocol's SysEx never reaches the page, the MIDI routes or Out.
The status: `fmr_slot`, `fmr_name`, `fmr_link`, `fmr_words` (loaded, fetching, none, failed), `fmr_said`.

Who says what: rfm1cs-abletonmove 0.6.0+ says what the Move's screen shows (`RFM_MOVE_ACT_SAY`, turned on with
`rfm_move_set_speech` once the user's screen reader is on): its views, popups (knobs, readouts, the volume, held-button
labels, host popups such as "FM-1 linked"), menus, the browser, FX setup, the sequencer. The host says only what the
core never draws: entering, loads and downloads, states, a slot switch made from the page (the core shows no popup for
`rfm_move_fw_set_active`), leaving. Built against an rfm1cs without `RFM_MOVE_HAS_SPEECH` (0.5.x), the host guesses the
words from the core's public state instead (`speak_watch` in engine.c). `tests/speech_e2e.sh`: the host with
`tests/fake_tts.c` as the sidecar (the same segment; it logs each text and answers with a tone): entering, the modes,
a Control knob, the volume, a knob touched, the firmware view, a firmware selected and loaded from the Move, the slot
switched from the page and with Back + Track (each said once), the words mixed into the output, the exit gesture, the
settings never changed; with the setting off, nothing.

## A rebuild of the same version

A catalog entry whose version is the installed one but whose .rfm1 is another (the index's `rfm1.sha256` is not the
installed manifest's: the same release built again with new glue, e.g. ChoralRoot 0.42 with cpu2 and rd1) is offered
as an update, as the iOS and Android apps offer it: its installed version reads "<version> old" (catalog.c), the
status counts `cat_updates`. The download goes over the installed files (the same names); a slot that runs the old
build keeps it until the firmware is loaded again, which then runs the new one: objects are opened through a link of
their own (fwrt.c fw_obj_open), since dlopen would hand back the old object for a path it already has loaded. A
version "<v>-rN" also ranks above "<v>" (storage.c st_vercmp). tests/dl_e2e.sh phase R.

## Development builds

`make package DEV_FIRMWARES="a.rfm1 b.rfm1"` compiles those .rfm1 files into `build/fw-dev` and puts them in the
module as `firmwares-dev/`. At start the host installs each over the installed firmware of the same `<id>-<version>`
when its manifest names another .rfm1 (its sha256), else as a bundled one (`--dev-firmwares DIR`, default the module's
`firmwares-dev/`). The id and version stay, so the slots, states and projects that name them keep working; the
signed catalog is untouched (an installed version as new as the catalog's is not an update), and the catalog's next
version replaces it as usual. `FIRMWARES.md` lists them as development builds. Without `DEV_FIRMWARES` there are none.

## Package

```sh
make package                 # dist/remotefm1-module.tar.gz
```

One `remotefm1/` folder: `module.json` (id `remotefm1`, tool, standalone), `standalone` (Move Remote's launcher: the
log, the setuid real-time helper), `remotefm1-host`, `remotefm1-heal`, `firmwares/` (the bundled compiled firmwares
with their manifests, licence texts and skins), `catalog/index.json` (the Move catalog's snapshot: the browser lists
the whole catalog offline; downloads need Wi-Fi), `site/` (the page, ~0.9 MB), `FIRMWARES.md` (each bundled
firmware's licence and exact source), the licences.

With `dist/move-catalog` there (`make catalog`), tools/bundle.py bundles the firmwares `module/bundle.txt` names from
it: Felucca, ChoralRoot and the public (not hosted-only), non-beta firmwares that sound on the check and are under
0.85 MB compressed (39 of 114): about 23 MB. Without it, the module has build/fw's firmwares and no index (~5 MB).
The first launch copies the bundled firmwares into the RemoteFM1 folder (~45 MB), before any sound.

**Engine notes.** A firmware's load: the storage thread opens its object and makes its instance (wasm2c's
instantiate: its memory, megabytes of locked pages); the engine thread fills the ring to ~80 ms of the other slots'
sound, then starts it (its first calls on the engine's thread, between two blocks: see "Threads"). An unloaded instance is freed on the storage thread. A firmware's stdout and stderr: whole lines, at most 4 a
second after a burst of 20, the rest counted ("N lines dropped"); the log file (stderr's) is moved to `<log>.1` past
1 MB (`--log-cap`).

## Layout

| Path | What |
|---|---|
| `src/main.c` | the program: the engine, the page, mDNS, then Move Remote's platform main (`plat_move.c` / `plat_sim.c`) |
| `src/engine.c` | the engine thread: 3 firmware slots on their device clocks, the mix and limiter, the ring the audio thread drains, the rfm1cs session over a socketpair, the actions |
| `src/fwrt.c` | the firmware runtime: a compiled firmware loaded and booted as RFM1-FORMAT.md section 7 says, its clock, its panel, the WASI imports, traps |
| `src/storage.c` | the RemoteFM1 folder, the storage thread (firmwares, states, the Move's settings, library and projects) and the network thread |
| `src/projmem.c` | a project's copies of its firmwares' memory and the states' files: names, the json's hash, the compare that names the active state |
| `src/catalog.c` | the catalog the browser lists (the index merged with the installed firmwares) and its downloads (SHA-256, install) |
| `src/https.c`, `src/catsig.c`, `src/ed25519.c`, `src/catalog_key.h` | the downloads' HTTPS client (the system's OpenSSL 3, dlopen'd); the index's signature, Ed25519 (TweetNaCl's arithmetic), the catalog's public key |
| `tools/sign_index.py` | index.json -> index.json.sig (make catalog, make testcat; tools/bundle.py checks it) |
| `tools/build_move_catalog.py`, `tools/catalog_fw.sh`, `tools/bundle.py`, `module/bundle.txt` | the Move catalog; the module's starter set |
| `src/fx.c` | Schwung FX: the installed modules, their parameters, loading (with the crash probe), the calls |
| `src/fm1link.c`, `src/fm1sim.c` | an FM-1 on USB-A or over Wi-Fi (libremotefm1's session over Move Remote's USB-A hooks and a TCP socket); the simulator's fake FM-1 (tests) |
| `src/fsvc.c` | a Fantasy CFW's file API (rfm1_host_1): the sandbox, the requests, the pump between calls; served on the storage module's file thread |
| `web/move/js/movefs.js`, `movefiles.js` | the page's file browser (the header's Files) and the settings sheet's Files section |
| `web/move/js/movebrowse.js`, `fw-browser.js`, `css/fw-browser.css` | the page's firmware browser (Browse all): RemoteFM1's browser (a copy of its web/site file) over `/api/catalog` |
| `src/files.c` | the file browser's rules (paths, links, what may change), the listing, a folder as a streamed zip |
| `src/wav.c`, `tests/ainchk.c` | a WAV read as 44.1 kHz stereo (`--sim-audio-in`, `fwrun --audio-in`); the input tests' WAV and take checks |
| `src/web.c` | the page on :7730: the site's files, the API, the WebSocket (src/web.h) |
| `web/move/`, `scripts/build_site.sh` | the page: RemoteFM1's web/site with `site.patch` and `js/host.js`, `js/movehost.js` -> `build/site` |
| `src/mdns.c` | `remotefm1.local`, and the RTP-MIDI session's `_apple-midi._udp` |
| `src/midiout.c`, `tests/rtpmidi_peer.c` | Out: RTP-MIDI and USB-A ("MIDI out"); the tests' AppleMIDI initiator |
| `fw/fwglue.c`, `tools/build_fw.sh` | `.rfm1` -> `.rfm1m` (wasm2c, then a shared object with one entry table; cpu2's twin) |
| `tools/w2c_mt.sh` | wasm2c's runtime per thread ("Threads"): the host's copy and the header firmware objects are built against |
| `tests/cpu2fix/` | the core-1 test firmware (C, its `build.sh` with Emscripten, the `.rfm1` kept in git) |
| `tests/tkdemo/` | the takeover's test firmware (FM1VST's tkdemo, `wants: tk1`; the `.rfm1` kept in git) |
| `tools/rfm1info.c`, `tools/fwrun.c` | an `.rfm1`'s manifest and checks; a compiled firmware run offline (rfm1_run's run, or a replay) |
| `third_party/abletonmoveremote` | Move Remote, branch `in-process` (its embedding hooks: the audio source, the input sink, USB-A) |
| `third_party/rfm1cs-abletonmove` | rfm1cs-abletonmove 0.7.0 (9851ff9, branch `takeover`) |
| `third_party/RemoteFM1` | RemoteFM1 at 32d03d6, branch `slow-link` (its `web/site`: the page; libremotefm1 for the FM-1 on USB-A) |
| `docker/Dockerfile` | the build and test image (debian bookworm, the Move's toolchain, wabt 1.0.32) |

On the Move, the RemoteFM1 folder is `/data/UserData/RemoteFM1`:

```
firmwares/   <id>-<version>.rfm1m, .json (the manifest, with the .rfm1's SHA-256), .licenses.txt, and its skin when the
             .rfm1 has one: .rfm1skin (rfm1.skin), .rfm1skin.bg, .rfm1skin.bgl (the images)
             .dl/ a download under way (its .part files)
states/<id>/ flash.bin (the firmware's memory as it was last left, anywhere: what a firmware loaded by hand starts
             from), state-NN.bin (the 32 state slots), each with a .json (id, version, sha256, saved; and "hash": the
             image's SHA-256, written by 0.13.0's host)
move/        settings.bin, library.rmvl, open-project, projects/move-project-NN.rfmmove and beside it
             move-project-NN.fx.json (its FX modules' own state), move-project-NN.fwK.bin / .fwK.json (K 1..3: its own
             copy of firmware slot K's memory, 1 MB, and its id, version, sha256, saved, hash); .clip.fwK.* (hidden:
             the project buffer's copies, while a project is cut or copied)
catalog/     index.json, index.json.sig (the catalog's index and its signature, as last downloaded)
catalog-url  (optional) the catalog's base URL, first line
audio/       a Fantasy CFW's samples, every cart reads them (the page's Audio files puts WAVs here)
code/<id>/   a Fantasy CFW's own scripts and assets (read); data/<id>/ its own files (read and write)
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

When the Move switches firmware itself (its projects view, another track), the page follows (`moved`).

**Slots and Mix & FX** (web/move/js/moveslots.js, movemix.js, moveui.js, css/move.css; the design in docs and the
shared design note): on a desktop the header's three slot chips replace the firmware picker (a live 48 x 48 mini
screen, name, version, level bar; a tap plays that slot, the chevron or an empty chip opens "Load into slot n" /
"Unload"); on a phone the iOS app's faceplate with a slot button top left and its menu. The Mix & FX view has the
Move's FX setup as rfm1cs 0.5.0 has it: one layer (the chain: MIDI FX, the firmwares with their filters, the audio FX,
as the Move's pad columns 1, 3, 5), the selected slot under it (a module's own pages as tabs, 8 knob cells each with
gaps; a firmware's filter, level, pan and sends), and the Mixer. It follows the Move's selection (`mix.page`,
`mix.focus`, each FX slot's `pg`) and sets it. Keys: ⌥1-3, ⌥M, Esc. The page's edits are ops on `/ws` (or `POST
/api/edit`, engine.h ENG_OP_*): FW_ACTIVE, FW_UNLOAD, MIX, ROUTE, FX_LOAD, FXPAR (23: a parameter 0..127, `b` kind<<9 |
slot<<7 | value; 21 FX_PARAM, the 0.4 form, still takes 0..7), PAGE (22: the column or Mixer, `| (row + 1) << 2` the
slot) and FXPG (24: a slot's page); rfm1cs's host setters apply them as the Move's own edits (FXPG and PAGE's row write
`fw.fx_pg` / `fw.fx_focus`, which have no setter), so the Move's screens, the project and the page agree. The page shows what comes back: `{"type":"mix"}` JSON on `/ws` (also `GET /api/mix`), the modules
(`GET /api/fx`), meters (binary 3) and the slots' mini screens (binary 4, at most 4 a second each, only when they
changed). Screenshots: docs/move-page-slots.png, move-page-fxpages.png, move-page-mixfx.png, move-page-phone.png,
move-page-phone-mixfx.png, move-page-phone-fxpages.png. The page
needs no secure context: no AudioWorklet, Web MIDI, WebUSB, microphone, IndexedDB saves or folder access. Besides
the ops above, `/ws` carries MIDI (in: a frame that starts with 32; out: binary 5; the Editor, below).

**The firmware browser** (js/movebrowse.js over js/fw-browser.js, css/fw-browser.css: RemoteFM1's browser, the iOS
app's firmware tiles; `fw-browser.js` and `.css` are copies of RemoteFM1's web/site files, kept the same): a slot's
load menu shows each firmware's thumbnail and "Browse all", which opens the Move's whole catalog as a grid of tiles
for that slot (the slot can be changed in its head; a dialog on a desktop, the whole screen on a phone). The settings
sheet's Firmware page is the same browser (site.patch's sheet.js `moveFirmwarePage` -> `movebrowse.js sheetPage`):
the whole catalog, a tap loads into the slot that plays (or downloads first), "Bigger view" opens the dialog.

- **The data:** `GET /api/catalog` (engine_catalog): every entry of the catalog as the Move's own browser lists it
  (the index's and the installed ones): `id`, `name`, `version` (the index's), `installed` ("" none), `description`,
  `categories`, `virtual`, `indexed` (downloadable), `bytes`, `thumb` (64 x 64, 1 bit, base64), and `favorites`: the
  Move's library's favourites in order (set with the Move's Sample button; the page shows them, it does not change
  them).
- **The thumbnail:** RemoteFM1FW's catalog image (its catalog/catalog.json `image`, from raw.githubusercontent.com,
  as the iOS app shows them) when the device showing the page reaches GitHub, else the Move's own 1-bit thumbnail,
  drawn crisp (pixelated, the theme's text colour on the bezel's), else the name on a colour of its own. Drawn as the
  tiles come into view; each image once a page.
- **The filters** (the owner's, as on the Move): Favourites or All (All by itself when there are none, the choice
  kept), Installed (its updates counted) / Not installed, one category at a time or Fantasy CFW, a search; kept in the
  browser's localStorage.
- **A tap:** an installed one loads into the slot and plays it; one not installed, or with a newer version in the
  index or another build of the same version (UPDATE; the host's `cat_updates`, an installed rebuild reading
  "<version> old"), is downloaded by the Move (`POST /api/download {id}`: engine_download, rfm1cs's FW_DOWNLOAD as a jog
  press in the Move's catalog does it; its progress is the status's `dl_*`, a ring on the tile), then loaded.
  Screenshots: docs/move-page-browser.png, move-page-browser-all.png (dist/move-catalog's firmwares),
  move-page-phone-browser.png, move-page-sheet-browser.png, move-page-phone-sheet-browser.png.

**The Editor** (the header's Editor button, as on remotefm1.com: RemoteFM1's editor-panel.js, behind ON_MOVE):

- **MIDI both ways.** `MoveHostPlayer.midi(bytes)` sends MIDI to the firmware in the Move's active slot as `/ws`
  frames that start with 32 (any size, in pieces). The host puts the bytes back into whole messages (SysEx up to
  64 KB), and the engine gives their USB-MIDI packets to the firmware's `rfm1_midi_in` as many a device ms as it takes
  (rfm1_run's `--sysex` way; logged for the replay). The active firmware's MIDI out (up to 64 packets a block): the
  real-time bytes go to the sequencer as before; SysEx and channel messages go to every page as binary 5, which
  MoveHostPlayer emits as the worklet's `midiOut`. Status: `pmidi_msgs`, `pmidi_bytes`, `pmidi_dropped`, `midi_out_bytes`.
- **Which firmware has which editor** is remotefm1.com's: its `catalog.json` (`editor`, `liveEditor`, `port`, `repo`)
  and `managers/index.json`, read live from remotefm1.com (CORS) by `moveEditors` and merged into the Move's entries.
  No internet in the browser: no Editor button.
- **The editors run in the panel's sandbox** (live-editor.js): the author's live page first, fetched from its site
  with the Web MIDI shim (managers/fm1-shim.js, served by the Move) put in, as on remotefm1.com; else ("Use hosted
  copy", or the live page cannot be read) remotefm1.com's hosted copy, fetched the same way into the same sandbox (its
  own shim's tag dropped: an iframe of another origin could not reach the page). The page is plain http, so the
  sandbox gets a SHA-256 `crypto.subtle.digest` stand-in (Felucca's editor checks files with it). No pop out.
- **What works:** an editor whose page its site lets other sites read (GitHub Pages: most of them) and that talks
  SysEx through Web MIDI: checked by hand with the real pages, Felucca's editor (connected, its sound, 298 messages
  each way) and ChoralRoot's device manager (reads the firmware) on the simulator (`docs/move-page-editor-felucca.png`).
  An editor that only lives on a site that does not allow it, or a firmware with only an editor link and no port,
  shows "works only on its own site, with an FM-1 on USB" (Web MIDI needs https) and its link.

**Files** (js/movefs.js, js/movefiles.js; the host's src/files.c and web.c `/api/fs`): the header's Files button (and
the settings sheet's Files section, in place of Audio files: Add samples, Browse files, Export all projects, Export
states) opens the RemoteFM1 folder: breadcrumbs, each entry's size and date, download, a folder as a .zip, upload
(a file picker or files dropped on the panel), rename, delete (asked first; a folder with things in it asked twice),
a new folder. `docs/move-page-files.png`.

- **The API:** `GET /api/fs/list?path=P` (folders first, then A-Z; `writable`, `why`, `free`, the slots' firmwares,
  the open project), `GET /api/fs/get?path=P` (streamed, an attachment), `GET /api/fs/zip?path=P` (a store-only zip,
  streamed, its length known first; names inside start with the folder's own name, the root's `RemoteFM1/`),
  `POST /api/fs/put?path=DIR&name=N[&replace=1]` (the body; 256 MB at most, 64 MB left free; a temporary file
  renamed when whole; audio/ takes WAV and AIFF only), `POST /api/fs/mkdir?path=P`, `POST /api/fs/rename?path=P&to=Q`,
  `POST /api/fs/delete?path=P[&recursive=1&confirm=NAME]` (a folder with things in it only with both). Refusals:
  400 (the path), 403 (read-only or in use, with the reason), 404, 409, 413, 507.
- **The rules (src/files.h):** paths relative to the root; segments of valid UTF-8, no control characters, no `\`,
  not `.` / `..`, not hidden (the host's own temporary files are), 255 bytes a name and 1024 a path; a new name also
  without `: * ? " < > |` and edge spaces (it must survive a zip on any computer); no symbolic link anywhere on the
  way (links are not listed, zipped or followed).
- **What may change:** audio/, code/, data/, states/ and move/projects/ only; those folders themselves stay.
  **firmwares/ is read-only from the page:** a firmware is native code the host dlopens in a process with a real-time
  thread, so only the signed catalog installs one (Mute in the projects view); an uploaded `.rfm1m` + `.json` would be
  a way around the catalog's signature. catalog/ (the signed index), the root's own files and move/'s settings,
  library and open-project are the Move's. While a firmware runs in a slot its states/<id>/ is left alone, and its
  data/<id>/ and code/<id>/ take new files but no delete, rename or replace; the open project's
  move/projects/move-project-NN.* neither.

**The page's States and projects** (js/movestore.js; the host's src/web.c `/api/store`, engine.c `store_run`): the
owner's "no way of saving or changing states on a firmware, and no way to save or load a project". The header's
**States** and **Projects** caps (beside Files: one click from the faceplate; on a phone the slot menu's States and
Projects, and a slot's load menu has "States of slot n") open one panel with two tabs. Everything acts on the Move's
own States and projects, so the page and the Move always agree; nothing is kept in the browser.

- **States:** a slot's firmware's 32 States as the Move's pads (4 rows of 8, state 1 top left; the slot picked in
  the panel, the playing one first): empty (dashed), stored (the slot's colour), the active one white. A tap picks one;
  its row says which firmware and version saved it, when, and whether with its working memory (its `.json`, read
  through `/api/fs/get`). Actions: Load; Save the sound here (over a stored one: asked first; the active one: Save
  now); Download (a `.zip`: the active one is saved first, as its autosave would); Upload (a state file of this
  firmware into that state; over a stored one asked first; over the active one "replace it and load it"); Delete
  (asked first).
- **Projects:** the 32 projects (project 1 top left) in their colours, the open one white. A tap on the open one saves
  it and says "Saved" on its pad. Actions: Open; Save; Duplicate (then a tap on an empty slot); Download (the open one
  saved first); Upload (a project file into an empty or, asked first, a stored slot; never the open one); Delete
  (asked first; the open one: the Move goes on with an empty project there, as after Delete + its pad). No names in
  this round: numbered as the Move numbers them.
- **Live:** the mix JSON's `store_ver` moves whenever what the panel shows changes on the Move (a pad, an autosave, a
  project opened, a firmware loaded), and the panel reads `GET /api/store` again.
- **The same path as the Move's pads.** A state save / load is the core's `STATE_SAVE` / `STATE_LOAD` action put
  through the host's own deferred handling (`do_deferred`: the active state's memory written first, the working memory
  saved and restored, the log's lines), then the core told what its pad would have set (`fw.st_used`, `fw.st_cur`
  through `rfm_move_fw_state_cur`, New's undo gone), so the Move's screen, its pads and "the active state saves itself"
  stay right. Opening a project is the project grid's: the open one saved (`rfm_move_project_flush`), the core's
  `proj_pending` set, `PROJECT_LOAD` through `on_action` (the store reads it, `rfm1cs_session_project_answer`, its
  `PROJECT_FW`s load the firmwares with the project's copies). Saving the open one is its autosave now (`seq.dirty`
  set, then `rfm_move_project_flush`), its firmware copies first written with their working memory as it is now (the
  autosave alone writes a copy only when the flash moved: "Saved" must be the sound now). Deleting is `PROJECT_DELETE` through `on_action` with the core's directory and
  info updated as Delete + its pad leaves them (the open one: `rfm1cs_session_project_answer` with no blob, the cut of
  the open one). What no pad does goes through the storage thread's own jobs: a state's delete that leaves the Move's
  state buffer alone (`J_STATE_DELETE` with value 1: Delete + pad on the Move is a cut into it, and the page must not
  change what the Move's Copy + pad pastes), a project copied into an empty slot without the Move's project buffer
  (`J_PROJECT_DUP`: its FX state and firmware copies, then its blob through the store, so the store's directory and
  the core's grid follow), and the files the page sends (`J_STATE_IMPORT`, as a paste; `J_PROJECT_IMPORT`). A request
  is answered once its jobs are back (a `J_SYNC` after them) and what it asked for is checked against the Move.
- **What rfm1cs 0.13.0 does not offer** (the submodule is pinned; the host writes the core's public fields as its own
  handlers do, as page_edit does for FX setup): no call asks for a state function or a project function from outside,
  so the Move shows no popup ("State 3 saved") for the page's actions and the project grid's loading screen comes up
  when the blob is read, not at the request. A cleaner job would take `rfm_move_state_act(m, slot, k, op)` (the pad's
  `st_pad` / `st_new` / `st_undo`, with its popup and buffer marks, for SAVE, LOAD, DELETE without the buffer, NEW,
  UNDO) and `rfm_move_project_act(m, slot, op, to)` (`rfmv_proj_open` with `rfmv_pfw_press`, `rfmv_proj_delete`, a
  copy into an empty slot, and a save that says "Saved"). Opening an empty slot (a new project, `rfmv_pfw_new`) has no
  host path at all, so the page offers no "new project": the Move makes them.
- **The API:** `GET /api/store` (`{"ver", "open", "pending", "loading", "used", "dirty", "projects": [{"k", "color",
  "bpm", "clips", "fw"}], "slots": [{"id", "name", "version", "used", "cur", "color"} or null]}`); `POST /api/store
  {"op", "slot", "k", "to", "id", "replace"}` with op `state-save`, `state-load`, `state-delete`, `project-open`,
  `project-save`, `project-dup` (`k` into the empty `to`), `project-delete`, answered when done (200 `{"ok":true,
  "message"}`, 409 `{"ok":false,"error"}` in plain words); `GET /api/store/state?slot=S&k=K` and `GET
  /api/store/project?k=K` (the files); `POST /api/store/put?kind=state&slot=S&k=K[&replace=1][&load=1]` and
  `POST /api/store/put?kind=project&k=K[&replace=1]` (the body: a file).
- **The files:** a store-only `.zip` made by the file browser's own zip writer (`files_zip_plan_sel`: the files of one
  folder that start with a name). A state, `<id>-<version>-state-NN.zip`: `state-KK.bin` (the flash image),
  `state-KK.json` (`id`, `version`, `sha256` of the .rfm1, `saved`, `flash`, `hash` of the image, `mem` of the working
  memory) and `state-KK.mem` (the working memory, when the module's snapshots are whole). A project,
  `move-project-NN.zip`: `move-project-NN.rfmmove` (RMVP, one project), `.fx.json` (its FX modules' state) and each
  firmware slot's copy `.fwK.bin` / `.fwK.json` / `.fwK.mem`, so it restores its firmwares' sound on another Move. A
  lone `.rfmmove` (RemoteFM1's web player's Backup) uploads as a project without copies.
- **An upload:** streamed into a hidden file of the root (256 MB at most, 64 MB left free), unpacked into a hidden
  folder (`.store-up-N/`: store-only entries, each CRC checked, at most 16 files, no name twice; one left behind by a
  stop goes at the next start), checked whole (a state: one `state-NN.bin` with its `.json` naming the slot's firmware,
  its image's and memory's hashes as the `.json` says; a project: one `.rfmmove` with only its own `.fx.json` and
  `.fwK.*` beside it, each copy's hashes), then put in place by the storage thread with renames on the same file
  system: the old state's or project's files go first, the new one's image (a state) or blob (a project) last, so a
  stop half way leaves the old one or none, never a mix. Refused in plain words: another firmware's state ("this is a
  state of Felucca 1.8.2, and slot 2 runs ChoralRoot: a state loads only into its own firmware"), a state over the
  active one without `load=1`, a project over the open one, a state file as a project or the other way round, a
  compressed or damaged `.zip`. The file browser's own rules (files.h) stay as they were: it still never writes a
  running firmware's states/ or the open project's files; only these requests do, through the engine.
- Screenshots (the page test, under build/): `move-page-states.png`, `move-page-projects.png`,
  `move-page-phone-states.png`, `move-page-phone-projects.png`.

**On the Move the page shows:** the faceplate at the site's layouts, the themes and background image, firmware
skins, the firmware's button names and live labels, the screen protector, the computer keyboard and mouse/touch
panel, the header's firmware picker (the Move's firmwares), Source and Site source links, the sheet's Firmware,
Look, Background, View, Engine (one line) and About pages. **Hidden:** the emulated and native browser engines,
opening or dropping an .rfm1, saves in the browser, Use my FM-1 / Connect FM-1, Use my OMX-27, Use my Ableton Move,
the RemoteFM1 folder (the browser's: the Move's own folder is the Files view), audio input, the recorder, volume /
sound output / MIDI input / MIDI learn, Install RemoteFM1 and the app links, the Editor's pop out.

**Upstreaming.** Every change is behind `ON_MOVE`; with `content="web"` the site is RemoteFM1's unchanged. So
RemoteFM1 can take `site.patch`, `host.js` and `movehost.js` as they are; RemoteFM14Move's build then only sets the
flag (and later fetches RemoteFM1's built site instead of patching it).
