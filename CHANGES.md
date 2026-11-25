# Changes

## Unreleased

rfm1cs-abletonmove's `sampler` branch (2a78cfe, local: 0.15.0's sampler app and `rfm_move_seq_pos`; closed with a
take recording, one Sample press brings the app back and ends the take).

- (rfm1cs 5d76ada) **Down in the FM-1 layout does nothing** (the owner's 2026-11-25: the Track buttons switch
  firmwares); the help's two mentions of it are gone.
- **The sampler after the owner's try on the Move (2026-11-24).** A stems take is a folder of its own,
  `<MMDD>-<HHMMSS> Stems <bpm>bpm <bars>b` (or `<seconds>s`): `Master.wav`, `Track 1 <firmware>.wav` ..
  `Track 3 ...`, `Input.wav` (`Looper.wav`, `Kit.wav` later); `... Stems.part` while it records, named when whole,
  ` (2)` never over a take. The page's file browser follows `audio/rec`, the takes' link (`files_follow_link`): listed
  as a folder (`"link": true`), download, zip, upload, rename and delete inside it, each path inside its target; the
  link itself stays; every other link still hidden and refused, `.` / `..` refused. Bars, stopped: Sample arms, Play
  starts the take at the song's first frame (worked as built; now tried end to end). `rec_take_t.first`; with
  `--test-api` `smp_reds`, `rec_play_f`, `rec_first_f`, `rec_beat0`. Help "Sampler". Tests: `recorder_test.c` (the
  stems folder, names, ` (2)`, the `.part` folder readable, a failing folder), unit (the followed link),
  `files_e2e.sh` (the takes through `audio/rec`), `rec_e2e.sh` (one Sample press after Back, the stems folder, Arm then
  Play with Length 1 and Open).

- **The sampler** (README "The sampler"): `src/recorder.c` in the host, driven by the core's sampler app
  (`RFM_MOVE_CAP_SAMPLER`): Master, Slot 1..3 (dry) and Input (with its level) each block, the sequencer's position
  for the bar lines, the status ~30 times a second, one event a take, the firmware names as labels. Takes to the
  Move's sample library (`Samples/RemoteFM1/<date>/`), `ROOT/audio/rec` a link to them; without that library
  `ROOT/audio/rec` itself; `--takes-dir`. The carts' file service follows that link (`fsvc_set_audio_link`). Help:
  "Sampler". `/api/status` `rec_*`. Tests: `tests/rec_e2e.sh`; unit: the link; `ainchk info`, `gaps`.

## 0.4.0 (2026-11-24)

rfm1cs-abletonmove 0.14.0 (tag v0.14.0, 1004f14).

- **Engine parameters (ep1)**, stages 1 and 2 of the ENGINE knob page (the owner's of 2026-11-24: "add a page after
  the FM-1 knobs for Engine parameters ... you reach that page, then click the jogwheel, now you can scroll and select
  from different pages for the synth engine ... Each page then supports up to 8 parameters on the knobs"). A firmware
  that exports `rfm1_ep` / `rfm1_ep_set` / `rfm1_ep_watch` (FM1VST `WANTS ep1`; ChoralRoot 0.45 from FM1VST's
  `rfm1-ep1`) has its record read after each block and its description, layers, values and watched cells given to
  rfm1cs 0.14.0 (its ENGINE page); the core's `EP` / `EP_WATCH` actions become queued, logged inputs (`FWI_EP` 13,
  `FWI_EP_WATCH` 14). `/api/status` per slot `ep1`. README "Engine parameters (ep1)"; `tests/ep1_e2e.sh`;
  `tests/wsprobe.c --op` (a panel op at a time: OPT held while PRESETS turns). Firmwares without ep1: their objects
  byte for byte as before. rfm1cs-abletonmove at its ENGINE page (local `session-view-fix`, bf898c5).

- **The page's projects after a start** (seen on the owner's Move: every stored project but the open one listed with
  0 BPM, no clips, no firmwares). The core fills a stored project's brief only from a save or a paste in that session
  (`R_PROJECT_STORED`); nothing asked for the others, so the page, and the Move's own grid, showed them empty. The
  host now reads each stored project's file at start on the storage thread and gives the core its brief
  (`rfm_move_project_info`). A file this host cannot read says so in `/api/store` (`"read":"damaged"` or `"newer"`)
  and on the panel, is kept as stored so nothing is made over it, and is logged. `tests/store_e2e.sh` restarts the
  host and checks the first listing.

- **States: pad 1 is the project's own sound, pads 2..32 a library** (the owner's of 2026-11-24: "a project should
  keep it's own unique firmware state which then shows up as slot 1, so slot 1 always means project state, a unique
  state per project", and "You should be able to reset a state by holding the state and pressing delete"; the host's
  part of rfm1cs 0.14.0's contract, README "Projects that hold their firmwares; States"). Index 0 is the open
  project's copy of the firmware (no state file 0); state files 1..31 are a library shared by all projects that
  playing never writes: no state is active, the autosave goes into the project's copy and `flash.bin` only. A library
  load goes into the project (the project's copy written at once) with one undo per slot, as do New and a paste onto
  pad 1; Copy + pad 1 copies the sound now; a library pad held + Delete (`STATE_NEW` with its index) stores the
  firmware as first installed there, the same code as the page's Reset to default. An old `state-00` (the owner has
  one for ChoralRoot) is moved once, by renames with a journal, to the first free library index; with none free it
  is left in place, never shown or written, and said in the log. The page's States panel: pad 1 white, PROJECT,
  "Project N's sound": Save now, Download (the project's copy as a state file), Upload (replaces it and loads, asked
  first), Reset to default (New, asked first; the Move's Undo brings it back); library pads: Load into the project,
  Save here / over (asked first), Download, Upload, Delete, Reset to default; `/api/store`'s `cur` is always 0, with
  `proj` and `undo` per slot. State files downloaded before (state-00.* inside) still upload. Tests:
  `tests/states_e2e.sh` (the owner's point with two projects, a library state byte for byte after loads, playing and a
  restart), `tests/projfw_e2e.sh` (the Move's gestures, the `state-00` move and the no-free-index case),
  `tests/store_e2e.sh`, `tests/loadstop_e2e.sh`, `tests/page_e2e.mjs`.
- **rfm1cs-abletonmove 0.14.0 at cb708b8** (the three core branches of 2026-11-24 joined: the owner's notes on the
  controls and the arp, States, the rates and the triplet layout; the core's CHANGES has each). For the host: a **new
  random project** (step 13 on the Projects page: "create a new project with 3 random firmwares (random from the ones
  installed) and then a random scale and root note ... in a empty slot") comes as `PROJECT_FW` with no `PROJECT_LOAD`
  and no copies: a project with no copy now loads the firmware as first installed (was its `flash.bin`, another
  project's sound), the project switched to is written to `move/open-project` as its firmwares come (it opens at the
  next start), and a project deleted while open gets no copies written back when the one before returns (its Undo);
  `tests/projnew_e2e.sh`. **A new project from the page** (the owner's "Works but no way to start new project in empty
  slot"): New project on an empty slot of the Projects panel, `/api/store` op `project-new` (`rfm_move_project_new`).
  The project blob is version 10 (the rates): the host's reading of its firmware part is unchanged (version 9 and
  up) and uploads go through the core's own check. `module/help.json`: the Sequencer (Back + Play, the knob lock, Shift
  + an FM-1 knob, Rate and Feel, FW Play), Arp (per track, the clip's notes, Delete + click, random lanes, octaves
  down, no Chance), Back shortcuts, States (the new model, a pad held + Delete), Projects (step 13) and Firmwares (step
  13 Random) pages. Nothing else in the host needed a change (the library file is passed through as before: the core
  moves a tag stored on step 13 itself; the page's browser has no Move tags).

- **The page's States: Reset to default** (the owner's "Works but need an option to reset the selected state to
  default"). A state's row has Reset to default: the state becomes the firmware as first installed, as the Move's step
  14 New gives it (a fresh, erased flash, no working memory; its row says "as first installed"). Over a stored state
  it is asked first; the active state's firmware restarts fresh and the state stays active; another stored state only
  has its file replaced; an empty one stores it at once. `POST /api/store` op `state-reset`, answered when done, 409
  with a plain reason when refused (an empty slot, another firmware). `tests/store_e2e.sh` reads the voicing back
  (0 after a reset), `tests/page_e2e.mjs` clicks it in the panel.
- **A project or a state opened while the Move is stopped is stopped** (the owner's "Loading a new project is starting
  playback when the Move's playback is stopped"). A firmware's working memory saved while its own loop or sequencer
  ran (a save, a duplicate or a project left while playing, the autosave while played) came back running: the Move's
  log showed ChoralRoot's "chord: G9" / "chord: Fsus9" after "project 22 open", with no note in. After every restore,
  the Move stopped, the instance now gets the Stop (FC) the Move sends every firmware as its sequencer stops, so its
  loop stops and its memory is otherwise as saved; the Move playing, nothing is sent. Projects and states already
  saved while playing open stopped too. README "The working memory"; `tests/loadstop_e2e.sh`.
- **rfm1cs-abletonmove 0.14.0** (local d013197; the owner's notes of 2026-11-23 on the Move's controls and the
  Projects page; the core's CHANGES has each one). In the track view each track keeps its knob page; on the FM-1 knob
  page the jog's click is the takeover (Knobs + Keys) and there the jog turns the FM-1 layout's target (SELECT, PRESET,
  ALGO: Shift + click the next; in Knobs, Shift + click puts the jog on a knob or back on its pages), Play the
  firmware's only while taken over; Delete and Copy held are the sequencer's there too, so Delete + Play clears the clip
  to one bar and nothing reaches the firmware; Shift + Track 1-3 opens that firmware's track settings; the Shift menus
  mark the selected row in place; a Shift + knob turn with no readout shows the turn; the active state and the open
  project are white, breathing; the Projects page: a pad's press is its info, its click opens, the open one's click
  saves ("Saved"), Undo brings back a deleted project, a delete is a short red flash then an empty slot; **the arp has
  no CHANCE** (always 100 %, knob 4 dark; a stored 0 no longer silences it). The host's part: `PROJECT_SAVE` with
  `value` 1 (the open project's pad clicked) writes the firmwares' copies as they are at that moment, synced, with
  their working memory, as the page's Save does (the autosave still writes a copy only when it is behind);
  `tests/store_e2e.sh` clicks the pad on the simulator. Undo of a delete is the core's `PROJECT_PASTE` of the cut
  project (nothing new here); the arp's notes reach a firmware through the track's routing as a clip's (nothing new).
  help.json: the Sequencer, States and Projects pages.
- **The page's States and projects** (the owner's "the local webpage still has no way of saving or changing states on
  a firmware, and there is no way to save or load a project"; README "The page's States and projects"). The header's
  States and Projects caps (the phone's slot menu; "States of slot n" in a slot's load menu) open one panel: a slot's
  32 States and the 32 projects as the Move's pads, empty / stored / the active state or open project in white. States:
  load, save the sound into one (over a stored one asked first), download, upload (another firmware's state refused in
  plain words; over the active one only replaced and loaded), delete (asked first); the other slots one tap away.
  Projects: open, save (a tap on the open one says "Saved"), duplicate into an empty slot, download with its firmware
  copies, upload, delete (asked first). Live with the Move (the mix JSON's `store_ver`). Each action goes the way the
  Move's pads go (the core's `STATE_*` / `PROJECT_*` actions through the host's own handling, the core told what its
  pad would set), so the Move's pads, its active state's autosave and the working memory stay right; what no pad does
  (a delete that leaves the Move's buffers alone, a copy into an empty slot, a file from the page) is the storage
  thread's own jobs. New: `GET /api/store`, `POST /api/store`, `GET /api/store/state`, `/api/store/project`, `POST
  /api/store/put` (a store-only .zip: a state's image, working memory and .json; a project's .rfmmove, FX state and
  firmware copies; checked whole, unpacked into a hidden folder, put in place by renames), `files_zip_plan_sel`,
  `files_unzip`, js/movestore.js, `tests/store_e2e.sh` (the API, ChoralRoot's voicing read back: a state and a project
  downloaded, deleted, uploaded, loaded, the sound back) and the panel's checks in `tests/page_e2e.mjs`
  (`tests/fx_e2e.sh` leaves `store_ver` out of its mix comparison). rfm1cs 0.13.0 has no entry point for a state or
  project function from outside: the Move shows no popup for the page's, and the page makes no new project (README:
  the calls a cleaner job would take).
- **States and projects hold the firmware as it was** (the owner's "States are not saving or loading. Always loads the
  same."; README "The working memory"). A state was a copy of the firmware's flash image only, and most firmwares
  write their flash on their own schedule (ChoralRoot once it has been quiet a while), so a state saved after a change
  held the old sound: on the owner's Move ChoralRoot's states 00, 01 and 03 and Melodee's two were byte for byte its
  `flash.bin`, and every load re-applied what the flash last held. Now a state, a project's copy of a slot and New's
  undo also keep the firmware's working memory (its whole memory and globals, the host's view of the instance), and a
  load restores it into a new instance of the same module instead of booting it from its flash: exactly as it was
  (offline, every one of the 116 published firmwares that qualify goes on bit for bit as the original would, core 1
  or not). `state-NN.bin` stays the flash image (an older host loads it as before) with `state-NN.mem` beside it
  (`move-project-NN.fwK.mem` for a project's copy; the `.json` gains `"mem"`); a .mem is used only by the very module
  that wrote it booting the very image beside it, else the flash alone (the log says why). Which firmwares:
  `src/snapsafe.h`, generated by `tools/snapsafe.py` (`make snapsafe`) from the .rfm1 files (their SHA-256; no
  `.rfm1m` rebuilt): all published ones but the Fantasy CFWs (Elasticat, Ripple: flash only, as before). The active
  state and the open project's copy are written with the memory before another state or project loads over them,
  when the firmware leaves or the host quits, when the firmware writes its flash (as before), and at most every 30 s
  while it is being played; saving another state no longer writes the active one unless it is behind on the flash.
  A snapshot fills the ring ahead first (no late block on the simulator; see README for the Move), the file is made
  on the storage thread (ChoralRoot 8 MB: ~230-340 KB). After a restore: the takeover is told it is not shown, held
  keys and MIDI notes are let go (CC 123). New: `src/memsnap.c`, `fw_snap_*` in `src/fwrt.c`, `fwrun --snap-test`,
  `POST /api/test/peek`, status fields (`snaps<s>`, `restored<s>`, `snap_max_us`, ..), `tests/states_e2e.sh` (the
  firmware played, not poked: ChoralRoot's voicing through its takeover, Felucca's PRESETS; 25 checks) and unit checks
  of the file format; the page's Files keep the open project's `.fwK.mem` too.
- **ChoralRoot's "+12 voicing on every state load or New"**: not reproduced on the simulator with ChoralRoot 0.43
  (FM-1 mode and its takeover, core 1 on): a fresh start and New give 0 and the host sends nothing to K1 at a boot. A
  load booted the firmware from the state's flash, and ChoralRoot re-applies its session from the flash at boot
  (`css_apply`: `cr_voicing_step(x->voicing - cr.voicing)`), a session it writes only on SAVE held: a flash whose
  session holds +12 brings +12 back at every load of it (a state, a project, the firmware loaded). States saved by
  this host now restore the voicing they were saved with; New is a fresh memory (0 here).
- `do_deferred` does not run again inside one of its own actions (a snapshot renders ahead: its render_block came
  back into the same action, endlessly).

## 0.3.0 (2026-11-23)

rfm1cs-abletonmove v0.13.0 (the tag; the entries below name the local commits it was built from on the way). The
Move catalog is unchanged (118 firmwares, the index of 2026-11-21).

- **The owner's requests while testing 0.12.0 on the Move** (rfm1cs-abletonmove 0.13.0, local commit f91f2ea; the
  board, drawn from the build: docs/design/ux-0.13.movedesign.json, 30 layouts, made by docs/design/make_ux_013.py).
  Shortcuts on a held Back (+ Left / Up home, + Loop the whole clip, + Play to bar 1, + Delete closes everything); a
  held Track's step selects at the press, a Track tap flashes the selected track, a held Track + the volume knob
  shows the slot's level in every layout; the FM-1 knob page in the note layouts (TRIG, FM-1, CC banks; the jog's
  click: Knobs / Knobs + Keys); session view: a deleted clip's X goes when Delete is let go, a double click on the
  playing clip stops it; the Loop page: the length's popup and the step lights show the last bar's steps, the pads
  set its length, a step after the loop's end leaves the length alone, a bar held + the jog changes its notes'
  length, a double click always makes that bar the loop; the Shift menus: the jog selects a parameter, its click
  edits it; a new project's routing (tracks 1-4 / 5-8 / 9-12 to firmwares 1 / 2 / 3, channels 1-4 a group), also for
  an empty project made current at start. `tests/sess_e2e.sh` (21 checks: the X), `tests/fx_e2e.sh` (its premise,
  every firmware hearing every track, set explicitly). `tests/page_e2e.mjs`: its four ChoralRoot checks follow the
  two versions (0.43 installed by the build, the site's catalog of the day listed: 0.44 since 2026-11-22, so an
  UPDATE mark), not a fixed pair.
- **Found by the on-device run of 2026-11-22** (docs/DEVICE-QA-2026-11-22.md; tools/device_qa): leaving session view
  with Menu named the view "equence"; the name is whole now (rfm1cs-abletonmove v0.13.0).
- **A project holds its firmwares; firmware states on the Move** (rfm1cs-abletonmove 0.13.0, local commit 8b28fed,
  `RFM_MOVE_CAP_PROJFW`; the owner's "Project should save firmwares"). Each project keeps its own copy of each slot's
  firmware memory, `move/projects/move-project-NN.fwK.bin` (+ `.json` with the image's hash), written whenever the
  memory is and only when behind it; opening a project writes the outgoing one's copies (synced), then loads its
  firmwares with its copies (the same firmware: only the memory swapped; a missing one: the slot empty, the project
  keeps its name and its copy; no copy: `flash.bin`, else a fresh memory) behind the core's loading screen, and names
  the stored state the memory equals as the active one. A new project has three empty slots; Copy + a project then an
  empty pad duplicates it with its copies (the project buffer carries them; a delete drops them). The active state
  saves itself; Copy / Delete on a state pad use a state buffer, a paste onto the active state loads it; step 14 New
  (a fresh memory, no state active) and Undo. A version 1..8 project leaves the firmwares alone and becomes version 9
  with copies at its next save; at start a version 9 project loads its own firmwares instead of the library's. The
  host now builds the core's `projfw.c` (the Makefile). Status fields for the tests; `POST /api/test/poke`
  (--test-api); `tests/projfw_e2e.sh` (32 checks) and unit checks of `src/projmem.c`; the page's Files keep the open
  project's copies. README "Projects that hold their firmwares; States"; help.json's States page and Projects lines.
- **The owner's answers on the review page** (rfm1cs-abletonmove 0.12.0, local commit bfa3b0b; the board, drawn from
  the build: docs/design/ux-0.12.movedesign.json, 30 layouts, made by docs/design/make_ux_012.py). Session view: from
  anywhere (Shift + Menu, a held Track + Menu, Back + Menu) and Back never leaves it; a colour a slot (orange, blue,
  yellow; the columns take their slot's, solo purple); columns that do not reach the firmware played dimmed and
  dotted; the stop icon filled; Delete deletes into the buffer and never pastes, Copy held copies / pastes /
  replaces, the same for scene rows, with an icon while held; Launch quantization (Global settings) and each clip's
  own Launch and After (project blob version 8); a select step held: the clip's settings on knobs 1, 2 and the level
  on the volume knob; Shift + arrows page. The arp: Shift + step 11 gated (a click opens, a hold toggles), On / Off
  across the screen and the pictures dimmed when off, the lanes' wave, a held lane step's pad shortcuts, step 1 + a
  step the length. Buttons and menus: held controls take over an open menu, only Back closes a menu, Loop and Mute
  latch on a quick click, every Shift menu names its knobs, a menu owns its knobs over the Mixer / Routing / the FM-1
  layout, Track 4 held a whole layer, Delete + Play clears the clip (Shift + step 13 gone), octave and held-step
  transpose popups, Fill shown, "None" for an empty undo, Back's light only with a layer to close, the browser's
  filter icons, knob popups over a takeover's screen. `tests/sess_e2e.sh` (20 checks: Delete never pastes, Copy held
  pastes the deleted clip) and help.json (Session View, Arp, Sequencer, Track Buttons, Layout Menu) follow.
- **The owner's UX rules on the arp, the held Track buttons and session view** (rfm1cs-abletonmove 0.11.0, local
  commit e398818; the rules: its design/UX-RULES.md; the board, drawn from the build as the items were greenlit:
  docs/design/ux-0.11.movedesign.json, made by docs/design/make_ux_011.py from the core's test dumps). The arp menu:
  a header names the touched knob and its value, the jog's click turns the arp off and on, the lanes' steps are the
  step keys (the MOD lane drawn as signs), Menu flips the view under the menu and Back closes it. A held Track 1..3:
  click / hold / double click gated (a hold acts while held), not routed is dark, the selected track blinks, a solo
  stays, the slot at a glance on the screen. Session view: its own layer (a Track tap stays in it, the knobs lit by
  the routing; a held Track: the pads route a column), a scene pad starts playback, the handover flashes (stop icon /
  play icon), a double click is now, Shift + a scene stops it, the stop steps' icons lit. `tests/sess_e2e.sh` (19
  checks) and help.json (Session View, Layout Menu, a new Arp page) follow.
- **Session view, after the owner's review** (rfm1cs-abletonmove 0.10.0, local commit bd49975; the board
  docs/design/session-view-v2.movedesign.json, made before the code by docs/design/mock_session_v2.c and
  make_session_v2.py): the pads' 8th column launches scenes (7 tracks in view); Live's colours (green: playing; the
  track's colour, dim: stopped); an empty slot pressed is selected and not filled, and clips with no notes are empty
  slots again when a project loads; Shift + a slot selects it; the steps under a column select / stop its track,
  step 16 stops every clip; a knob touched shows its track's popup; Copy tapped alone does nothing there; Shift +
  Play restarts the clips. `tests/sess_e2e.sh` and help.json's Session View page follow.
- **Session view** (rfm1cs-abletonmove 0.9.2, local commit 790142c; the owner's report of 2026-11-21): the pressed
  clip is framed on the Move's screen (the board docs/design/session-view-v1.movedesign.json); clips are cut, copied
  and pasted with the steps' gestures (Delete held: cut, the next press pastes; Copy held: copy, an empty slot
  pastes); a playing clip's pad fades in 6 steps a beat, not a level a tick (measured on the simulator with the
  owner's project: 39 LED writes a second in a playing session view, 634 before). `tests/sess_e2e.sh` (new, in make
  test); help.json: a Session View page.

## 0.2.0

- **ChoralRoot 0.43 bundled**, with its own Move interface (the takeover, CHORDS: pad 2 of its Track's layout menu).
- **rfm1cs-abletonmove 0.9.1**: proposals 2 and 3 and the takeover (Back is the firmware's, its clock leads the
  sequencer, the volume popup over it).

- **One main layout** (rfm1cs-abletonmove 0.9.0, the owner's board docs/design/track-buttons-v3): a slot's note
  layout is the sequencer on its selected track; Track n held is one menu (layouts on the pads, the 16 tracks on the
  steps: select, per-firmware solo, not routed); Back only closes layers; a firmware pad's double tap loads it; Play
  sends Start, clock and Stop to every loaded firmware (engine.c: MIDI dest ALL); Shift shortcuts in the Mixer and
  Routing; Track 4 held shows its Shift menu at once. help.json and the e2e tests follow; midiout_e2e checks the clock.
- **The Track buttons, proposal 2** (rfm1cs-abletonmove 0.8.0, the owner's board docs/design/track-buttons-v2):
  Track 1..3 are the firmware slots (tap: play it in its own layout; held: its layout menu: FM-1, its takeover,
  Scale, Chroma, Drum, Octave, latch, scale, channel, root, octave, CC numbers; double tap: its firmware browser);
  Track 4 the sequencer side (tap: the Mixer / Routing; held: the Shift shortcuts; double tap: the projects); Back
  flips to the Sequencer and back. Control mode is gone (every note layout has the CCs on its knobs). The FM-1
  layout's knobs: SELECT, PRESET, ALGO, -, K1..K4; the jog's click cycles SELECT / PRESET / ALGO. The takeover is
  pad 2 of its slot's layout menu. help.json, README and every e2e test moved to the new gestures.
- **Takeover (tk1).** A firmware made for the Move can bring its own Move interface (FM1VST docs/RFM1-TAKEOVER.md,
  rfm1cs-abletonmove 0.7.0): Track 1 in FM-1 mode opens it, the firmware draws the screen and the LEDs of the parts
  it claims and gets their input; volume touched + Back always leaves it. Firmwares without it are unchanged.
  (`rfm1m_tk1` in `fwglue.c`, `fw_tk*` in fwrt, the record read after every block; `tests/tk_e2e.sh` with tkdemo.)

## 0.1.2

- **Screen reader.** RemoteFM1 speaks what the Move shows: the mode, the firmware view and its slots, the firmware
  selected, loads and downloads, the slot played, buttons, steps ("Step 1") and the sequencer, knob names and values
  (a firmware's own readouts, Control mode's CCs, the volume), Global Settings, latch, the FM-1 linked, and leaving.
  It uses Schwung's own speech program and the user's voice settings.
- **Speech mode** in RemoteFM1's Global Settings (Shift + step 2 in Sequencer mode): **Off**, **On**, or **Jog** (only
  while the jog wheel is touched; a touch says the screen). Off unless Schwung's own screen reader is on.
- **Firmwares that speak for themselves.** Firmwares with the FM-1 screen reader protocol
  ([fm1-reader](https://github.com/charlesvestal/fm1-reader): AMB-1, FoMni-1, Golf, Tekstuur, LOOM-8 and more) say
  their own words through the Move, with their words file.
- **MIDI routing between firmwares.** On FX setup's Routing page, hold a firmware's pad and tap another's to send its
  MIDI there, or the pad below them (**Out**) to send it out: to USB-A (when the FM-1 isn't linked there) and as
  network MIDI ("RemoteFM1 Move", RTP-MIDI, seen in a Mac's Audio MIDI Setup over USB-C or Wi-Fi). Saved with the
  project; no loops.
- **Favorites in your order:** in the Favorites list, knob 2 moves the selected firmware up or down.
- **Global Settings redrawn:** the battery with its icon and percentage on top, the settings below.
- **Delete** is a modifier: hold it and tap steps to cut, then paste by the last action; Track 4 + Delete clears.
- Fixes: audio uploads from the page's file browser, folders dropped on it, knob labels.

## 0.1.1

- **On-device help.** RemoteFM1 now ships Schwung's `help.json`: Global Settings > System > Help > Modules > RemoteFM1 explains the modes, the firmware view (load, download, update, switch slots, states), FX & Mix, MIDI and Control, the sequencer, an FM-1 as the panel, and how to leave.

## 0.1.0 (first release)

**RemoteFM1 on the Ableton Move.** FM-1 firmwares (`.rfm1`) run on the Move itself, as native code, and you play them
from the Move's own pads, knobs and buttons. It is a [Schwung](https://schwung.dev) tool: choose **RemoteFM1** in
Schwung's Tools menu and the Move becomes an FM-1 host; leave with **Shift + touch the volume knob + click the jog
wheel**, and Move's own software comes back.

Needs Schwung 1.6.0 or later. Install it from Schwung Manager: **Modules > Install Custom Module > From GitHub URL**,
`Quixotic7/remoteFM14Move`.

- **Three firmware slots, each on its own thread.** Load up to three firmwares at once, switch between them, set
  their levels and keep their states, from the projects view (Shift + step 1). Each slot renders on its own thread
  and core, so a block costs the slowest slot, not the sum; firmwares built for the FM-1's second core (ChoralRoot
  0.42, FIZZ 0.13.0) run that part on a thread of its own too.
- **The catalog and the firmware browser.** 118 firmwares, every one built for the Move and checked against the
  `.rfm1`'s own sound, bit for bit. 39 come in the package; the rest download over Wi-Fi from the browser on the Move
  (Mute in the projects view) or on the page, by category or search, with thumbnails, descriptions and updates.
  The catalog is signed, and every file is checked before it is installed.
- **The sequencer and the Move's controls.** FM-1 mode on the pads and knobs, MIDI, Control and a sequencer with
  projects, from [rfm1cs-abletonmove](https://github.com/Quixotic7/rfm1cs-abletonmove) 0.5.9.
- **Labels and readouts.** Each firmware's knob and button names and live labels, and its readouts (a knob's name
  then its value, a sound's name, a message) on the Move's screen and on the page. FM-1 pop-ups use the whole
  screen (a Global Settings switch puts them back on the left half).
- **Schwung MIDI FX and audio FX.** The MIDI FX and audio FX you installed in Schwung run inside RemoteFM1: three MIDI
  FX in series on the sequencer, three audio FX on send and return buses. Set them up on the Move (Session in the
  projects view) or in the page's Mix & FX view.
- **The page.** `http://remotefm1.local:7730/` (or `http://move.local:7730/`) is RemoteFM1's own player for the Move:
  the faceplate with the firmware's screen and LEDs live, themes and skins, the firmware's Editor, the slots, Mix & FX
  and the firmware browser. It works on a computer, a tablet or a phone.
- **Your FM-1 as a panel.** Plug an FM-1 running the RemoteFM1 firmware (0.3 or later) into the Move's USB-A port and
  its keys, knobs and screen play the active slot. An FM-1 with RemoteFM1 Wi-Fi does the same over the network.
- **Wi-Fi setup from the page.** With the FM-1 on USB-A, the page's "Set up the FM-1's Wi-Fi" scans, joins and
  remembers networks for it.
- **The file browser.** The page's Files view opens the RemoteFM1 folder on the Move: download a file or a folder
  as a .zip, upload, rename and delete; export all projects or all states.
- **Fantasy CFWs.** Host-only firmwares such as Elasticat and Ripple run too, with their own files, the shared audio
  folder and the Move's line in or mic as their audio input.

Every firmware keeps its own licence; the package's `FIRMWARES.md` lists each bundled one with its exact source.
