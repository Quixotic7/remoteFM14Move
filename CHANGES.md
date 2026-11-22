# Changes

## Not released (branch session-view-fix)

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
