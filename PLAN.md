# RemoteFM14Move: plan, revision 2 (2026-11-18, after the owner's answers; waiting for their OK)

Goal (owner): FM-1 firmwares run on the Ableton Move itself.
- **Launch:** pick "RemoteFM1" in Schwung's Tools menu and everything works.
- **Controls:** the Move's controls drive the firmware through rfm1cs-abletonmove, in-process.
- **On the Move:** switch firmwares and save or load firmware states from the Move alone.
- **Web manager:** a page on the Move that looks like remotefm1.com.
  - It browses the RFM1 catalog and downloads firmwares onto the Move.
  - It exports firmware states and sequencer projects, and has a file browser for the RemoteFM1 root folder.
- **Two run modes:**
  - **standalone:** Move's software stopped; RemoteFM1 owns the hardware;
  - **overtake:** a Schwung overtake module on top of Move and Schwung.
- Repo: github.com/Quixotic7/remoteFM14Move (empty).

## Owner's decisions so far
- One Schwung tool named "RemoteFM1", separate from Move Remote. Move Remote's code is used inside it as a library.
- Licence MPL-2.0; GPL firmwares are bundled with their source links.
- Firmwares come mostly as `.rfm1` from the catalog.

## Reused pieces
- **Move Remote** (abletonmoveremote 0.1.1):
  - plat_move.c: the SPI link, 128 frames at 44.1 kHz per transfer, and the real-time audio thread on core 3 with
    the setuid helper;
  - mr_core.c: the HTTP and WebSocket server, the LED and screen encoding, the exit gesture;
  - the plat_sim simulator for tests on the Mac;
  - the Docker cross build and the Schwung module packaging.
- **The FM-1 core ABI 6** (FM1VST): 44.1 kHz and 128-frame audio() blocks, exactly the Move's block.
  FM1VST's engine (device clock, core loading) is C++17 without JUCE.
- **.rfm1 runner:** RFMWasmCore.cpp (wasm3), as iOS and Android run it.
- **rfm1cs-abletonmove v0.2.0:** the Move control code and its session.
- **RemoteFM1 web/site:** the player, built against its engine interface (web/engine/API.md); also its screen
  codec (host/src/codec.c).

## Architecture: one engine, two front ends
`librfm14move` holds everything that is not the hardware path.
- **The firmware runner:**
  - `.rfm1` in wasm3; native `.fm1core` .so files where one exists;
  - one fm1core_t interface, rendering ahead into a lock-free ring on a worker thread off core 3.
- **rfm1cs-abletonmove in-process.** Move Remote's code turns the surface into its protocol bytes, and those bytes go
  in memory into `rfm1cs_session_rx`. EV_SEND bytes go back the same way. Neither side changes; the network is just
  not there.
- **The new on-Move features, in rfm1cs-abletonmove** so every host can use them later. They follow
  move-firmware-browser-design.md, "For RemoteFM14Move"; only what that note marks firm gets built.
  - **Design revision 2 (owner, under review).** In the projects view (Shift + step 1):
    - Tracks 1-3 are three firmware slots: up to 3 firmwares loaded, and the picked track is the one the Move
      controls. Track 4 is Sequencer Projects.
    - Mute opens a catalog mode that downloads or updates firmwares on the Move. Delete unloads a track's firmware.
  - **API (proposed):**
    - In: `fw_list` (the whole catalog, with each firmware's installed version), `fw_slots[3]` and `fw_progress`.
    - Out: `ACT_FW_LOAD(slot, id)`, `FW_UNLOAD(slot)`, `FW_ACTIVE(slot)`, `FW_DOWNLOAD(id)` (a download or an
      update) and `FW_LIBRARY` (the blob for the host to store).
  - **The host's side:**
    - up to 3 firmware instances, each with its own worker thread on cores 0-2;
    - the mix done on core 3;
    - the FM-1 panel, screen and LEDs follow the active slot.
  - **All three sound at once, summed (owner's decision).**
    - `FW_LEVEL(slot, 0-127)`: Track held + the volume knob sets that track's level. The volume knob alone stays the
      master.
    - `rfm_move_fw_voices(core, 1..3)`: the host's capacity. A slot past it stays loaded but silent, and its Track LED
      blinks slowly. The shown track always plays.
    - library.json keeps the three levels too.
    - Standalone's number comes from phase 0; overtake says 1. The estimate, to be measured in phase 0:
    - Standalone: three light firmwares are ~60 % of a core; three heavy ones ~110-120 %, spread over 3 cores.
    - Overtake: one wasm firmware; three need native builds.
  - **States:** where they go on the Move is open again. STATE_SAVE, STATE_LOAD and STATE_DELETE stay; the web
    manager names the slots.
- **Storage:** one RemoteFM1 root folder, the same layout as RemoteFM1's shared folder, so the apps and the Move agree.

```
/data/UserData/RemoteFM1/      (or under schwung/; the probe checks space and that it survives Move updates)
  firmwares/   <id>-<version>.rfm1 (+ native .fm1core)
  states/<id>/ slot files (flash images, with name, firmware version and date)
  move/        sequencer projects (.rfmmove), settings, library.json (favourites, colours, tags by firmware id; which firmware is on each of the 3 slots)
  code/ data/ audio/   the Fantasy CFW folder
```

- **The web server** (Move Remote's own server code, extended): the RemoteFM1 site built with `HOST=move`, plus:
  - `/api/fw`: list, load, upload, delete;
  - `/api/states`: list, save, load, export, name a slot;
  - `/api/library`: tag names, the favourites' order, pad colours;
  - `/api/files`: browse, download (a folder as .zip), upload, rename, delete, all inside the root only;
  - `/ws`: the panel in; the screen (codec deltas), LEDs and labels out.
  - The page's new `movehost` engine follows the same engine interface over `/ws`. The sound stays on the Move.
- **The catalog:** the page reads the RFM1 catalog and fetches the `.rfm1` it picks.
  - Route 1: from remotefm1.com. GitHub Pages allows cross-origin reads; the page then uploads the file to the Move.
  - Route 2: the Move downloads it itself, if the probe finds a working HTTPS client there (Schwung's installer
    downloads from GitHub, so probably yes).
  - Never raw .syx. The gating rules are the catalog's.

### Front end A: standalone (Phase 1)
The `standalone` executable owns the SPI exactly as Move Remote does. It starts from the Tools menu and leaves with
Shift + touch volume + click jog; Move then starts again.
- **Audio:** the firmware's blocks go out with a soft limiter; the volume knob is the firmware's MASTER.
- **Cores:** core 3 is the SPI thread, core 2 the firmware, cores 0-1 the web and storage.

### Front end B: overtake (Phase 3)
From Schwung's docs:
- An overtake module is a JavaScript `ui.js` plus an optional `dsp.so`, which the shim loads into Move's own process
  as `overtake_dsp` (plugin API v2).
  - `render_block(inst, int16 *out_lr, frames)` runs in Move's SPI callback on core 3, with about 2.4 ms of slack per
    block. No allocation, file I/O, locks or logging is allowed there.
  - Worker threads are allowed. They must drop to SCHED_OTHER and keep off core 3.
- So `dsp.so` = librfm14move: its worker thread runs the firmware ahead, and `render_block` only copies from the ring.
- `ui.js` is a thin bridge: Move input (`onMidiMessageInternal`) goes into the engine through `host_module_set_param`.
  The screen (1 KB) and LEDs come back through `host_module_get_param` each tick (~60 fps).
- Move's own sound keeps running. So the CPU budget is tighter, and it's open whether our output is mixed with
  Move's or replaces it.
- Schwung Manager shows a module's `web_ui.html` at move.local:7700/remote-ui. Ours would be a link card to our own
  server.
- Open, to check in Schwung's source before phase 3:
  - mixed or replaced audio;
  - LED control from JS;
  - whether a socket can be served from inside Move's process. If not, the overtake web page goes through the
    remote-ui parameter bridge, which is limited.

## The address (owner asked: remotefm1.local or move.local/remotefm1?)
- **`move.local/remotefm1`: not possible.** Port 80 belongs to Ableton's own web server. Our program can't add a
  path to it without replacing it.
- **`remotefm1.local`: yes.** Our program answers mDNS for the name itself, so no system change is needed. It works
  on Macs, iOS and Windows; Android's browser has trouble with .local names in general.
- A URL without a port needs port 80. **Measured:** Ableton's MoveWebService holds :80 even while Move's software is
  stopped, so in both modes it's `http://remotefm1.local:7730/`.
- The page is always also at `move.local:7730`, and the Move's screen shows the address at launch.
- Schwung Manager's Tool tab links to it.

## CPU and audio budget: MEASURED in phase 0 (remotefm14move-probe-2026-11-18.md)
- The Move: 4x A72 at 1.5 GHz; with Move stopped, cores 0-2 ~99 % idle, core 3 ~5 %; 1.7 GB RAM free.
- **wasm3 is too slow on the A72**, about 14x slower than the Mac:
  - Felucca runs at 2.0x real time with a chord; ChoralRoot at 1.2x; GHOULBOX at 1.3x;
  - Melodee and DUELL at 0.9x, so they can't keep up.
- **Firmwares therefore run as native code on the Move.**
  - Recommended: the catalog's .rfm1 files translated with wasm2c and compiled for linux-aarch64. Same file, same
    sound; the catalog builds them.
  - The alternative: FM1VST's cores built natively.
  - **Spike done:** wasm2c Felucca runs at 42x real time and ChoralRoot at 26x, about 2.5-4.5 % of a core. The audio
    is bit-identical to wasm3, and three heavy firmwares fit on one core. fw_voices is 3 in both modes.
  - Each catalog .rfm1 gets a linux-aarch64 wasm2c build in CI, tied to the .rfm1's SHA-256. The Move has no compiler.
  - wasm3 stays as a fallback for light firmwares only, at fw_voices 1.
- **Schwung FX are cheap:** 0.3-7 % of a core per audio FX, about 0 for MIDI FX. All of them load in our process.
- `.fwsc` (fm1-emu): out.

## Build, install, tests
- Docker: Move Remote's bookworm aarch64 cross image plus cmake.
- `make package` builds `remotefm1-module.tar.gz` with `module.json` (tool, standalone; later overtake), the binary,
  the helper, the site, and a starter set of `.rfm1` files.
- Install: Schwung Manager → Install from GitHub `Quixotic7/remoteFM14Move`.
- Submodules at fixed versions:
  - abletonmoveremote: its device code as a library. This is the "small change": a build target that compiles
    Move Remote's hardware and server code into our program. The user never sees it, and nothing changes in how
    Move Remote itself works.
  - rfm1cs-abletonmove;
  - FM1VST (engine, core-api);
  - RemoteFM1 (web/site, the codec).
- Tests on the Mac, against Move Remote's simulator:
  - a firmware boots and sounds;
  - pads play keys; the screen and LEDs come out;
  - firmware switch and states from the Move's buttons;
  - the web APIs, the file browser kept inside the root, the page in headless Chromium;
  - an aarch64 smoke run in Docker/QEMU.

## Phases
0. **Probe** (read-only ssh plus one benchmark binary, with the owner's OK):
   - who holds :80 in each mode;
   - idle CPU and RAM; free space on /data/UserData and whether it survives updates; an HTTPS client;
   - Felucca and ChoralRoot in wasm3 and native on core 2, and 3 instances on cores 0-2 at once;
   - the first audio at a few percent, by ear.
1. **Standalone, one firmware plays.**
   - The tool launches from the Tools menu.
   - Felucca (`.rfm1`) sounds; the Move's controls drive it through rfm1cs.
   - remotefm1.local / move.local:7730 shows the screen and LEDs and takes the panel. Tests; a branch.
2. **On the Move + web manager.**
   - Firmware and state menus in rfm1cs-abletonmove (v0.3.0);
   - the RemoteFM1 root folder; the full remotefm1.com page;
   - catalog download, exports, the file browser; sequencer projects in the root.
3. **Overtake mode.** dsp.so and ui.js, after checking Schwung's source for the three open points.
4. **Release** (owner's OK): GitHub release, Schwung Manager listing. Optionally, the catalog's native builds.

## Schwung FX inside RemoteFM1 (owner's request, 2026-11-18; checked against Schwung's source)
- The model:
  - 3 MIDI FX slots, in order, each with a track filter (one track or All);
  - the 16-track sequencer goes through them to all 3 firmwares, and each firmware has its own track filter;
  - 3 audio FX slots as **send / return** (design v8): each firmware has a level, a pan and a send (0-127) to
    each FX. The host sums each send bus, runs the FX on it in place, and adds the returns. FX should be 100 %
    wet; Schwung has no standard key for that.
  - Core actions: FX_LOAD, FX_PARAM (rfm1cs 0.5.0: the planned parameter's index 0..127; the host keeps the key), MIX, ROUTE. Sequencer MIDI is
    tagged with its track 1-16.
- Hosted by dlopen in our process, both modes.
  - The modules are the user's installed Schwung modules:
    - MIDI FX: /data/UserData/schwung/modules/midi_fx/<id>/dsp.so
    - audio FX: /data/UserData/schwung/modules/audio_fx/<id>/<id>.so
  - We list them from their module.json (ui_hierarchy pages, else chain_params; README "Schwung FX"). Schwung's MIT built-ins (arp, chord, freeverb) may be bundled.
- The audio FX API (`move_audio_fx_init_v2`): int16 stereo, 128 frames at 44100, processed in place.
- The MIDI FX API (`move_midi_fx_init`): process_midi in, plus tick per block, 3-byte messages, no timestamps.
- Our host_api_v1_t answers sample_rate, frames, log, get_bpm, clock status and beat position from our sequencer.
  The rest is NULL, plus the reserved tail of NULLs.
- Where they run:
  - The MIDI FX run in the core thread beside rfm_move_seq_advance.
  - The audio FX run in a mix worker: firmware workers, then the mix worker (FX, sum, levels, limiter), then the
    ring, then core 3. That adds about one block of latency.
- State: get_param("state") and set_param("state") go into the states and projects.
- CPU per module: measured in phase 0 (arp, chord, freeverb). Third-party ones vary, so the UI warns.
- Phase: after phase 2 (the firmware slots), as its own phase 2b, before overtake.
- **Built (2026-11-18, simulator only):** src/fx.c and the engine (README.md, "Schwung FX"), with the page's slot chips
  and Mix & FX view. Differences from the plan: the audio FX run on the engine thread (no separate mix worker: the
  engine already renders ahead of the audio thread), and a module's own state is kept beside the project
  (move-project-NN.fx.json) since rfm1cs's project blob has room only for names and values. Not yet tried on a Move
  with the user's real modules.

## Fantasy CFWs (owner, 2026-11-18: "build in the framework to support fantasy firmwares")
- **Built (simulator only):** README.md, "Fantasy CFWs". Elasticat 0.6.96 and Ripple 0.6.1 build, pass the catalog's
  check (the same PCM and the same files written as wasm3 with a RemoteFM1 folder) and run in the slots with the
  folder's audio/, code/<id>/ and data/<id>/ (src/fsvc.c, the storage module's file thread). The page gets a minimal
  Audio files list (upload, delete); the file browser stays in phase 2.
- **Built since (2026-11-19, simulator only):** audio input (`ain1`) for Elasticat's INPUT and REC: a fixed round trip
  of 1152 frames at lead 5 (README.md, "Fantasy CFWs"); the firmware Editor on the page (SysEx both ways over /ws)
  and the file browser with the exports (README.md, "The page").
- **Open:** the real Move: the input's latency measured (a loopback: line out to line in), the line in / mic
  choice (Move Remote gives what the SPI mailbox holds); a Fantasy CFW badge on the Move's own browser
  (rfm1cs has no field for it); the real Move: the memory (280 MB locked per Elasticat), CPU, SD-card reads.

## Design source
The session "RemoteFM1 Move Design" (local_05fd5546) designs firmware access and management: the on-Move menus and
the web manager. Its note (move-fw-browser-design.md, here) overrides this plan's proposals for shortcuts, layouts,
the slot model and the page structure.

## Still open for the owner
1. From the design note: does "All" list the firmwares on the Move or the whole catalog; where states go and how many slots; whether Shift + Menu opens Track 2.
2. The root folder: /data/UserData/RemoteFM1 (proposal) or inside schwung/.
3. The starter firmwares bundled in the module (proposal: the public, ungated set; the rest from the catalog).
