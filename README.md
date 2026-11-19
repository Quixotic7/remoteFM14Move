# RemoteFM14Move

FM-1 firmwares (`.rfm1`) running on the Ableton Move itself, played from the Move's own pads, knobs and buttons.
A [Schwung](https://github.com/charlesvestal/schwung) standalone tool, **RemoteFM1**. Phase 1 of [PLAN.md](PLAN.md).

- **The firmwares** run as native code: each `.rfm1` is translated by wasm2c and compiled for the Move
  (`<id>-<version>.rfm1m`), with the same sound as the `.rfm1` in wasm3, bit for bit (tested).
- **The controls** are [rfm1cs-abletonmove](https://github.com/Quixotic7/rfm1cs-abletonmove) 0.5.5, in-process: FM-1
  mode on the pads and knobs, MIDI, Control and the sequencer, and the projects view (Shift + step 1, or Track 1 held +
  step 1) with three firmware slots, their levels and their states.
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
  firmwares/ read-only), Export states and Export all projects downloaded. Screenshots: `docs/move-page.png`,
  `move-page-editor.png`, `move-page-files.png` and the slots' and Mix & FX's;
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

**Does FIZZ fit?** A slot fits while its `render_avg_us` stays under ~2600 us (2902 less the mix and the hand-over)
and its spikes are absorbed by the ring (5 blocks, 14.5 ms). On the simulator (an M-series Mac's Docker, ~5x faster
per core than the Move's A72), FIZZ 0.12.0-beta.19 with a six-note chord renders a block in ~700-950 us on average
(spikes 4-6 ms): roughly 3.5-4.5 ms on the Move, more than a block. The threads take every other slot, the mix and
the FX off FIZZ's core; FIZZ itself needs its core 1 (cpu2, a FIZZ build change) to fit a heavy part.

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
| `src/catalog.c` | the catalog the browser lists (the index merged with the installed firmwares) and its downloads (SHA-256, install) |
| `src/https.c`, `src/catsig.c`, `src/ed25519.c`, `src/catalog_key.h` | the downloads' HTTPS client (the system's OpenSSL 3, dlopen'd); the index's signature, Ed25519 (TweetNaCl's arithmetic), the catalog's public key |
| `tools/sign_index.py` | index.json -> index.json.sig (make catalog, make testcat; tools/bundle.py checks it) |
| `tools/build_move_catalog.py`, `tools/catalog_fw.sh`, `tools/bundle.py`, `module/bundle.txt` | the Move catalog; the module's starter set |
| `src/fx.c` | Schwung FX: the installed modules, their parameters, loading (with the crash probe), the calls |
| `src/fm1link.c`, `src/fm1sim.c` | an FM-1 on USB-A or over Wi-Fi (libremotefm1's session over Move Remote's USB-A hooks and a TCP socket); the simulator's fake FM-1 (tests) |
| `src/fsvc.c` | a Fantasy CFW's file API (rfm1_host_1): the sandbox, the requests, the pump between calls; served on the storage module's file thread |
| `web/move/js/movefs.js`, `movefiles.js` | the page's file browser (the header's Files) and the settings sheet's Files section |
| `src/files.c` | the file browser's rules (paths, links, what may change), the listing, a folder as a streamed zip |
| `src/wav.c`, `tests/ainchk.c` | a WAV read as 44.1 kHz stereo (`--sim-audio-in`, `fwrun --audio-in`); the input tests' WAV and take checks |
| `src/web.c` | the page on :7730: the site's files, the API, the WebSocket (src/web.h) |
| `web/move/`, `scripts/build_site.sh` | the page: RemoteFM1's web/site with `site.patch` and `js/host.js`, `js/movehost.js` -> `build/site` |
| `src/mdns.c` | `remotefm1.local` |
| `fw/fwglue.c`, `tools/build_fw.sh` | `.rfm1` -> `.rfm1m` (wasm2c, then a shared object with one entry table; cpu2's twin) |
| `tools/w2c_mt.sh` | wasm2c's runtime per thread ("Threads"): the host's copy and the header firmware objects are built against |
| `tests/cpu2fix/` | the core-1 test firmware (C, its `build.sh` with Emscripten, the `.rfm1` kept in git) |
| `tools/rfm1info.c`, `tools/fwrun.c` | an `.rfm1`'s manifest and checks; a compiled firmware run offline (rfm1_run's run, or a replay) |
| `third_party/abletonmoveremote` | Move Remote, branch `in-process` (its embedding hooks: the audio source, the input sink, USB-A) |
| `third_party/rfm1cs-abletonmove` | rfm1cs-abletonmove, tag `v0.5.5` (f6144bc) |
| `third_party/RemoteFM1` | RemoteFM1 at 32d03d6, branch `slow-link` (its `web/site`: the page; libremotefm1 for the FM-1 on USB-A) |
| `docker/Dockerfile` | the build and test image (debian bookworm, the Move's toolchain, wabt 1.0.32) |

On the Move, the RemoteFM1 folder is `/data/UserData/RemoteFM1`:

```
firmwares/   <id>-<version>.rfm1m, .json (the manifest, with the .rfm1's SHA-256), .licenses.txt, and its skin when the
             .rfm1 has one: .rfm1skin (rfm1.skin), .rfm1skin.bg, .rfm1skin.bgl (the images)
             .dl/ a download under way (its .part files)
states/<id>/ flash.bin (the flash as it was left), state-NN.bin (the 32 state slots), each with a .json
move/        settings.bin, library.rmvl, open-project, projects/move-project-NN.rfmmove
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
