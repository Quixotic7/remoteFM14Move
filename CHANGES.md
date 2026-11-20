# Changes

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
