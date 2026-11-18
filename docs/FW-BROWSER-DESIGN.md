# Move firmware browser: design, revision 2 (2026-11-18, waiting for the owner's feedback; no code yet)

- Mockup (private artifact, version 4): https://claude.ai/artifact/DA4bw6cfAapg7c6D2ZmbEL
- Local copy: move-firmware-browser-mockup.html here.
- Sources in move-firmware-browser-src/:
  - gen_data.py builds data.json from remotefm1.com/catalog.json and RemoteFM1FW/catalog/images.
  - splice.py joins template.v1.html's page with script.js into template.html.
  - build.py injects the data.

## The owner's design (firm)

Revision 2 is the owner's own changes after seeing the first mockup.

- **Projects view:** Shift + step 1 opens it.
- **Tracks:** Track 1, 2 and 3 are three firmware slots, so up to 3 firmwares can be loaded at once.
  The track you pick is the firmware the Move controls. Track 4 is Sequencer Projects (today's projects mode).
- **One firmware track holds everything.** There are no separate Favorites and All pages.
  - Step 1 shows Favorites, step 2 shows All.
  - The steps after that filter by category, and they combine with Favorites or All
    (e.g. Favorites that are Chords firmwares).
- **Pads:** a bank of up to 32 firmwares. A pad selects one; a jog press loads it into this track.
- **Jog turn:** shows the version and the description, full screen.
- **Sampling** sets a favourite. **Capture** held + a step tags the firmware. **A knob** sets the pad colour.
- **Mute** opens the catalog mode, to download or update firmwares. It uses the same filter steps.
- **Delete**, on a firmware track, unloads that track's firmware.

## Choices the mockup makes (open questions on the page)

- **Steps:** 1 Favorites, 2 All (one of the two); 3-7 the catalog's five categories; 8-16 nine user tags.
  - Categories match any (OR) among themselves, and AND with Favorites / All.
- **Catalog mode steps:** step 1 Updates, step 2 New (not on the Move); both are toggles, off shows everything.
  Steps 3-16 are the same categories.
  - Pads: an installed firmware in its colour; an update breathes, marked with an up arrow;
    a firmware not on the Move is faint white with a down arrow.
  - A jog press downloads or updates.
- **"All" lists the installed firmwares only.** Downloading happens in the catalog.
- **Track LEDs:** the loaded firmware's colour (full when shown, dim otherwise); an empty slot is faint white.
- **The pad of this track's firmware breathes.** The screen grid draws a border around it,
  and a dotted border around one loaded on another track.
- **Delete unloads at once,** with a message. A held Delete + Track is the safer alternative.
- **Knob 1** sets the colour. Left / Right page the banks.
- **The jog is one position:** 0 is the name view, 1.. are description rows, one row a detent.
- **Rest screen:** 8x4 cells of 16x16 with no borders and two letters of the name each (the owner's comment).
  Popups have no box around the value (the owner's comment).

The big open questions:
- Do the three firmwares all sound at once? If so, each needs a level, and the CPU cost needs measuring.
- Which firmware do the modes outside this view play?
- Should sequencer tracks route to a firmware slot each?

## FX setup (owner, 2026-11-18; Move host only, Schwung modules)

- **Opening it:** the Session button (≡, Move's Menu) in the projects view opens FX setup; Session again or Back leaves.
- **Pages:** the Track buttons pick them.
  - Track 1: MIDI FX.
  - Track 2: Routing. The owner didn't name this one; it is my proposal.
  - Track 3: Audio FX.
  - Track 4: Mixer (levels and sends).
- **MIDI:**
  - The sequencer (16 tracks, each with its own channel) always sends to all 3 firmwares.
  - Its MIDI runs through MIDI FX slots 1, 2, 3 in series. Each slot acts only on the tracks in its filter and
    passes the rest through.
  - Each firmware has a track filter (default: all 16).
- **Audio:** each firmware has a level and a pan, and a send to each of the 3 audio FX, whose returns join the mix.
- **Controls:**
  - Pads 1-3: the page's slots.
  - Steps 1-16: the focused slot's track filter (MIDI FX, Routing).
  - Jog: turn for the module list, press to load (MIDI FX, Audio FX).
  - Knobs: the module's parameters. On the Mixer: 1 level, 2 pan, 3-5 sends.
  - Delete: empties an FX slot.
- **Core API (proposal):**
  - In: `rfm_move_fx_modules(core, kind, list)`: names and parameters (0-127 or a list of words).
  - Out: `FX_LOAD` (kind, slot, module), `FX_PARAM` (kind, slot, index, value), `MIX` (slot, level, pan, sends),
    `ROUTE` (the filters).
  - The core tags each sequencer MIDI event with its track (1-16); the host runs the MIDI FX and applies the filters.
  - A host with no modules hides pages 1 and 3.
- **From RemoteFM14Move's check of Schwung's source (2026-11-18):** both APIs are plain plugins we can host.
  - Audio FX: process_block on 128 frames of stereo int16, in place. MIDI FX: process_midi plus tick for generated notes.
  - Several instances are fine.
  - Licences: the built-ins are MIT and can be bundled; third-party modules are loaded from what the user installed.
- **Decisions that follow from it:**
  - A MIDI FX filter is one track or All: a step sets that track, the same step again sets All.
    An arp's generated notes take the slot's track.
  - Loading a module past the host's CPU budget shows "Too busy".
  - Each FX slot's module state (get_param "state") is saved with the project.
  - The parameters come from each module's chain_params.
  - Send / return is done by the host.
    - Each FX slot has a bus: the sum of each firmware times its send, after its level and pan. The module runs on
      the bus and its return joins the mix.
    - A module with its own dry/wet parameter must sit at 100 % wet. library.json keeps a per-module default for it,
      and that knob's popup says "100 % wet for a send".
  - FX_PARAM uses the index in chain_params order; the host keeps the key strings.
- **Open:**
  - Do live pads go through the MIDI FX (the mockup: straight to the shown firmware)?
  - Is the routing saved with the project or with the setup?
  - The module names in the mockup are placeholders until Schwung's API is checked.

## For RemoteFM14Move (replaces my first sync note)

These are proposals, except the owner's own points above.

- **No separate firmware menu.** All of this is the projects view.
- **Into the core (host to core):**
  - `rfm_move_fw_list(core, entries, n)`: the whole catalog. Each entry has:
    - the id, name and version, and the installed version (or none);
    - the category bits (Synth, Groovebox, Chords, Drums, Other);
    - an ASCII description;
    - a 64x64 1-bit thumbnail or none. The host makes it: squeeze, then an Atkinson dither on luma.
  - `rfm_move_fw_slots(core, ids[3])`.
  - `rfm_move_fw_progress(core, id, 0..100 | -1 failed)`, for loading and for downloading.
  - `rfm_move_fw_library(core, blob, n)`: the favourites (ordered), the colours, the tags and the tag names.
    The core edits it and hands it back.
- **Out of the core (actions):**
  - `FW_LOAD` (slot, id)
  - `FW_UNLOAD` (slot)
  - `FW_ACTIVE` (slot: the one the controls play)
  - `FW_DOWNLOAD` (id: a download or an update)
  - `FW_LIBRARY` (the blob changed)
- **States:** they no longer have Track 3, which is now a firmware slot. Where they go is open
  (a mode inside a firmware track, e.g. Loop or Copy?). Your STATE_* actions still fit.
- **Root folder:** your layout fits. Add move/library.json: the favourites, colours and tags keyed by firmware id,
  plus the firmware on each of the 3 slots.
- **Decided by the owner (2026-11-18): the design is for all three sounding at once, summed,** even if a host
  cannot run three.
  - Track held + the volume knob sets that track's level (0-127), as `FW_LEVEL` (slot, level).
    The volume knob alone stays the master.
  - The host declares `rfm_move_fw_voices(core, 1..3)`: how many firmwares it can play at once.
    A slot past that stays loaded but silent, with its Track LED blinking slowly. The shown track always plays.
  - library.json should also keep the three levels.
- **Engine:** three firmwares at once. That means three wasm cores and a mix.
  - Measured on the owner's Move (2026-11-18, remotefm14move-probe-2026-11-18.md):
    - Compiled with wasm2c, a firmware takes 2.5-4.5 % of one core, with bit-identical audio.
    - So three fit on one core, and fw_voices can be 3 in both standalone and overtake mode.
    - Schwung audio FX take 0.3-7 % each; MIDI FX about 0.
    - wasm3 alone was too slow.
