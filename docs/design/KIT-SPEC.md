# The kit (the sampler's stage 3): the spec to build

Source: the design page's kit round 1 (https://claude.ai/artifact/2MMPRRBkxNaSXumJTTZk4P, cards `kit_*`, version 10)
and the screens in `mock_kit_v1.c` / `kit-v1.movedesign.json` (board letters A..W below). On 2026-11-25 the owner
said: "Please build it with your best guesses for the answers, I'm off to bed." So every decision below is the card's
"my pick"; the owner's later answers on the page override it. What the owner had already settled (their words in
quotes) is marked **settled**.

The owner's standing UI rules apply everywhere (rfm1cs-abletonmove `design/UX-RULES.md`): feedback at once; gated
click / hold / double click (a hold acts while held, never at the release); a layer that draws a menu takes the jog,
Back closes layers and never changes the mode or the view; a knob in use is lit, says its name on touch and its value
on turn (a full-screen popup); the Move's screen is very small: icons, minimal text, a setting's name only in a popup,
popups borderless and full screen, no boxes or inverted icons (a chosen row is a white row, as the sampler); lit
controls have a job, dark ones are dead; only Play starts the transport; knobs in an editor never move the jog's
choice. Draw in the sampler's look (src/sampler.c: 11 x 11 icons, `top()` tabs, `rowi()` rows, one row at rest and
three while the jog turns for RFM_MOVE_POPUP_MS, `v2_titleBox` popups). The mock's icons (I_PAD, I_PITCH, I_STRETCH,
I_ENV, I_CHOKE, I_SEND, I_LVL, I_PAN, I_COPY, I_MIX, I_JOIN, I_SLICE, I_SAVE, I_KEYS, I_KIT) are the ones to use.

## Stages (card kit_stages, pick a)

- **3a, playing:** the Kit view, 16 pads, sounds from Takes and Samples, a pad held + Sample, pitch, reverse, start
  and end, play modes with the envelope, choke, level and pan, the right half, tracks 13 to 16, kept in the project.
- **3b, shaping and sharing:** stretch and the step grid, FX sends and the Mixer's Kit strip, a whole folder, saving
  and loading kits, the sampler's Kit source and destination.
- **3c, making sounds:** mix and join, slicing a take, a pad's own edits, and the kit on the page's Sampler window.

## Where it lives (settled + kit_view a)

- **Settled:** Track 4's clicks go round its views Mixer, Routing, Kit, Looper (the click gated 300 ms, a double click
  = the projects); Track 4 held: pad 1 Mixer, 2 Routing, 3 Kit, 4 Looper, 5 Projects. Coming back from another
  Track, Track 4 is on the view it was left on. The Looper is not built yet: until stage 4 the round is Mixer,
  Routing, Kit (pad 4 dark and dead).
- **Pads:** the left 4 x 4 (columns 1..4) is the kit, **pad 1 bottom left**, then left to right, rows upward (MPC /
  Melodee order). A press plays the pad (velocity as the Velocity setting gives) and makes it the chosen pad; Shift +
  a pad only chooses it (silent). The right 4 x 4 (columns 5..8) is the chosen pad's pitches (below).
- **Screen (boards A, B, C):** tabs above the first row as icons: **Pad** (I_PAD), **Kit** (I_KIT), **Files** (I_DIR);
  the chosen pad's number at 2x top right. Pad tab at rest: the pad's wave (start..end), under it the 16-step grid
  with the length it lasts as a bar, then one row (the jog's); while the jog turns: three rows, the chosen in the
  middle; the jog past the first row: the tabs (click, turn, click as the sampler). A row's click: its popup
  (`v2_titleBox` name + value; Pitch / Stretch / Start / End also draw the step grid under it). Back: a popup closes;
  on the tabs or a list's top nothing more (Back never leaves the view).
- **Steps (settled):** the chosen pad's steps only, on its track (below). Pressing a step toggles the chosen pad's
  note there (the step's note = the pad's note, see tracks), as the sequencer's steps do.
- **Delete held + a pad:** its steps on its track go first; a pad with none is emptied (its sound off the pad; the
  file stays). Undo brings either back. **Mute held + a pad:** mutes the pad (grey), saved with the kit.

## The knobs (kit_knobs a)

Always the chosen pad's, lit pink (KIT {255, 60, 160}): 1 Pitch, 2 Start, 3 Stretch (3b; until then dark and dead),
4 Play mode, 5 Attack, 6 Release, 7 Level, 8 Send 1 (3b; dark until then). Touch: the popup with its name and value;
turn: changes it; Shift + turn: fine. The jog's rows hold every setting.

## The Pad tab's rows (in this order)

Sound (the file's name; click: the pad's tools, 3c; in 3a: Play and Delete only), Pitch (-24..+24 semitones, Shift
fine: cents), Stretch (Off, 1..64 steps; 3b), Start, End (the region, as a fraction shown in seconds), Play (One
shot / Momentary / Loop: icons I_PLAY, I_PAD, I_LOOP; board E), Attack, Hold, Release (ms; board F draws the AHR with
the part being turned thick), Choke (Off, 1..4), Reverse (Off / On: played backwards, the file untouched), Level (dB),
Pan, Send 1, 2, 3 (3b).

## How a pad plays (kit_play a, kit_choke a)

- **One shot** plays start..end to the end (the pad's release ignored); **Momentary** plays while the pad (or the
  step's note) is held; **Loop** goes round start..end while held. **Attack, hold, release on all three**: on a one
  shot the hold lasts to the end and the release is its tail.
- **Choke:** Off or groups 1..4; a pad in a group stops the others sounding in that group as it plays (a short fade,
  no click). The Choke popup lights the group's pads (board G), the others dim.
- A pad played again takes its own voice back (a short fade). Up to 16 voices plus the right half's pitch voices
  (cap 24 in all, the oldest stolen).

## The right half (settled + kit_right a)

A row in the **Kit** tab chooses **Pitches** (the default), **In Key** or **Off**, kept with the kit. Pitches: the
chosen pad at 16 semitones up from its own pitch, pitch pad 1 bottom left (bright: the pad's own and its octave, in
its row's colour; others dim white). In Key: the project's key and scale (the selected track's if it has its own),
rows a fourth apart, four scale notes a row, the pad's own note the root (roots bright). Off: dark and dead. A note
played there sounds the chosen pad repitched; recorded or step-entered, the step keeps that pitch (the kit track's
note carries it, below).

## Tracks 13..16 (settled + kit_tracks a)

- **Settled:** "Default routing to the last 4 tracks"; a chosen pad shows only its steps.
- **A pad row a track:** pads 1-4 play on track 13, 5-8 on 14, 9-12 on 15, 13-16 on 16. Choosing a pad selects its
  track (its clip, length and rate are that track's: hats at 1/32 on one row, a long loop on another). Notes: pad k
  of a row is MIDI note 36 + (pad - 1) (pads 1..16 = 36..51, the Move's drum notes) on the track's channel. A
  right-half pitch must be written so the step plays that pad at that pitch after save / load; the builder chooses the
  encoding (e.g. a per-note pitch field if the sequencer has one, or a note range of its own per pad) and documents it.
- **Routing:** the kit is a fourth destination beside the three firmwares. A new project routes tracks 13..16 to the
  kit; an old project (no kit) keeps its routing and gets an empty kit. A firmware Track held shows steps 13..16 in
  the kit's row colours (board T); a hold there routes that track to the firmware, and again back to the kit.
- **The pads' lights (kit_lights a):** each row in its track's colour (13 pink {255,60,160}, 14 orange {255,140,40},
  15 light blue {120,220,255}, 16 violet {170,120,255}); a pad with a sound dim, bright while it sounds, the chosen
  one white, empty dark, muted grey, recording red.

## Choosing a sound (kit_load a)

The **Files** tab: three places, **Takes** (I_LIST: every take, newest first, as the sampler's list), **Samples**
(I_DIR: Move's library `/data/UserData/UserLibrary/Samples`, folders and WAV / AIFF files), **Kits** (I_KIT; 3b).
A sound a row with its wave's small picture (board J); **each sound plays as the jog lands on it** (preview, on the
kit's voice path, never recorded); a click loads it onto the chosen pad; a pad pressed while browsing chooses the
target (blinking white). From the sampler, a take's actions gain **To Kit** (I_KIT, between Trim and Delete as the
mock's row): it closes the sampler on the Kit view with the pads blinking; a pad press puts the take there.

## Recording into a pad (settled + kit_rec a)

**Settled:** with the Kit view showing, hold a pad, press Sample: it records into that pad until the pad is let go
(or for the set length, or from the threshold); the kit's screen shows the wave growing (boards L armed, M
recording), the pad red, Sample red. **It uses the sampler's own Record settings** (source, mode, threshold, length)
as left. The take is written as any take (Takes list, Samples/RemoteFM1/<day>/) and put on the pad.

## Stage 3b

- **Stretch (settled + kit_stretch a):** Stretch = N steps: the pad's start..end is repitched so it lasts exactly N
  steps at the project's tempo (and re-fits when the tempo changes); Pitch is then ignored (shown "Stretch"). While
  Pitch, Stretch, Start or End change, the popup draws the 16-step grid with the length as a bar **and the step
  buttons light the steps it lasts** (board D), the first brightest.
- **FX sends (kit_fx a):** Send 1, 2, 3 on every pad to the three audio FX of FX setup; the kit is a **fourth strip in
  the Mixer** (level, pan, sends for the whole kit) and in Routing's slot pads where those list the slots.
- **A whole folder (kit_folder a):** Shift + the jog's click on a folder in Files: **All To Pads**: its first 16
  sounds in name order onto pads 1..16, the kit's sounds replaced; one Undo brings them back (board K).
- **Kits (kit_save a):** Kit tab rows: Load (a list of saved kits), Save, the right half's layout. A saved kit is a
  folder `Samples/RemoteFM1/Kits/<name>/` with copies of its sounds and `kit.json` (every pad's settings, the right
  half's layout); names given Kit 1, Kit 2 ... (renamed on the page later). "Saved / Kit 3" message (board R). A
  project keeps its own kit (settings + the sounds' paths) whether or not it was saved.
- **The sampler's Kit (kit_sampler a):** Source **Kit** = the kit's output alone (resampled); destination **To Kit** =
  the take lands **on the next empty pad**, which is lit white while the sampler is open (board U).

## Stage 3c

- **Mix and join (settled + kit_merge a):** Copy held, a pad, then another: the screen offers **Copy** (I_COPY),
  **Mix** (I_MIX: both summed, normalized not to clip), **Join** (I_JOIN: the first then the second, end to end); the
  jog chooses, the click does it (board N); the result is a new file on the second pad (board O: "Join 2+5"); Undo
  brings the pad back. Release of Copy before a choice: nothing done.
- **Slice (kit_slice a):** a take's actions: To Kit -> Slice: the wave with its cuts (board P), a row choosing
  **Hits** (onsets; the default), **Equal** (16 / 8 / 4) or **Steps** (a slice a step at the tempo); knobs 1 and 2
  move the chosen cut, the pads light and play the slices; the click puts them onto pads 1..16 from the chosen pad
  on, the take staying one file, each pad a region of it.
- **A pad's own edits (kit_tools a):** the Sound row's click brings play, trim, normalize, reverse, delete (board V);
  an edit makes the pad its own copy (a new file in `Samples/RemoteFM1/Kits/.edits/` or the kit's folder); the take it
  came from stays as it was.
- **The page:** the Sampler window gains a Kit tab: the 16 pads (play, choose), their settings, load / save kits.

## Host limits

16-bit sounds in memory, 64 MB a kit (a sound past it refused: "Too Long"); reading and every write on the file
thread the takes' tools use, never the audio thread; 48 kHz stereo voices, linear interpolation, an AHR each, a Kit
bus into the sends and the master; the project gains the kit (a new project version; an older host reads it as no
kit).

## Round A, 2026-11-25

The owner tried the kit and asked for a rethink of loading sounds and a few additions (the canvas
https://claude.ai/artifact/Xzv8SYTaqAcB8bPhdaproH). Built as follows; round B (tap chop, a pad filter, step locks)
comes next.

- **Pad-first loading** (the owner's words: "I'd like to press a pad and then the sample on that pad should be
  selected. If I have a pad selected and then select a new sample it should immediately be on that pad so I can use
  the pad itself to preview."): Files opens on the chosen pad's sound (its place, folder and row; an empty pad: where
  the last load came from). The jog at rest 120 ms on a sound puts it on the chosen pad, which plays it once (no
  separate preview for sounds; a fast spin loads only the sound it rests on; file reads on the worker). A folder or a
  kit loads nothing. A pad pressed in Files plays and becomes the target (blinking white); the list jumps to its
  sound. The click keeps the sound and goes back to the Pad tab; Back as before (up a folder, then the tabs).
  **Undo is one step for the whole browse**: what the pad held before Files was opened for it; a pad pressed
  meanwhile starts a new browse for that pad. The pad's settings stay; a new sound gets its whole region.
- **Close-up Start and End:** Start (knob 2, its row) and End (its row's popup) show the value large over the frames
  round the point, 8 times nearer than the whole sound (32 with Shift, which also makes the turn fine); the point a
  line with a notch on top; what does not play dithered. The host gives the peaks (KIT_PEAKS, off the audio
  thread). At rest 150 ms it is heard (KIT_AUDITION): a moved start from it for a second, a moved end the region's
  last second; each further turn cancels the pending one.
- **Hits' sensitivity:** in the slicing screen with Hits, knob 3 (it was free) sets the sensitivity 1..100 (50 as
  before); the cuts are asked again 100 ms after a turn. The host's onset detection takes the sensitivity and a
  minimum spacing between hits (60 ms).
- **Trim silence:** a take recorded into a pad (a pad held + Sample) starts 5 ms before its first sound (-50 dBFS);
  the file stays whole. The Kit tab's fourth row, Trim silence On / Off (default On), kept with the kit (project
  version 13) and in kit.json ("trim").
- **Toggle play mode:** the fourth Play mode: a tap starts the pad, the next stops it (a short release); it loops while
  on; a step's note toggles it the same way. Its icon I_TOGGLE (play, then stop); the Play popup draws the four.
- **Recording gesture:** unchanged (a pad held + Sample), as the owner confirmed.

## Round B, 2026-11-26

From the owner's approved canvas (Demo.dc.html; the brief wins where they differ). Built for rapid iteration: no new
tests yet (the owner's "move fast"), the full suites when the design is locked.

- **AHDSR:** Decay (envelope steps) and Sustain (0..100 %, 100 at first) after Hold. Held modes sit at Sustain until
  let go; a one shot runs A-H-D, sustains to the region's end with Release as its tail. A one shot with Hold > 0,
  Decay 0 and Sustain 100 % keeps round A's sound (released after the hold). The popup draws the five stages, the one
  turned thick.
- **Filter:** a TPT state-variable low-pass a voice; Filter 0..100 (20 Hz .. 20 kHz, 100 open: bypassed, the frames
  as before) and Resonance 0..100 (Q 0.5 .. 16), rows after Release. The cutoff glides (10 ms) and the coefficients are
  made every 32 frames and ramped between.
- **Knob pages** (the page chosen at the touch): 1 Pitch, 2 Start, 3 Stretch, 4 Play, 5 Filter, 6 Resonance, 7 Level,
  8 Send 1 (pink); Shift held at the touch (blue): 1 Attack, 2 Hold, 3 Decay, 4 Sustain, 5 Release, 6 Choke, 7 Pan,
  8 Fine (cents: Shift + knob 1 before). Shift pressed after Start is touched: the 32x close-up and a fine turn, as
  round A.
- **A step held:** the volume knob sets the chosen pad's hits' velocity there ("Step 5 Velo"); the step keys show
  velocity by brightness (100 as before). The knobs that lock light yellow (Pitch, Start, Filter, Resonance, Level;
  Shift: Decay, Pan), the rest dark: a turn gives that hit its own value ("Step 5" / the setting / the value). Locks
  are sequencer locks (cc 136 + lock x 4 + pad % 4, the value 0..254: move.h RFM_MOVE_KIT_LOCK0), saved with the
  project, sent with the note (KIT_NOTE's klk_mask / klk) and applied to that voice only. A locked step blinks slowly
  (1 s round) while its pad is chosen. Delete + a step clears its notes and locks.
- **Tap:** Slice's sixth method. The first step pressed plays the take from its start (the preview path), a cut at 0;
  each further press a cut where the sound heard is: the core's clock less 17 ms (768 frames at 44.1 kHz: the host's
  5-block lead plus Move's SPI block; the host's lead_us when it sets one), 16 slices at most. The steps light white a
  cut each, the slice under the playhead in the take's colour; the screen shows the playhead. A press after the take
  ended: a new pass. Delete + a step: the cuts cleared. Knobs 1 and 2 nudge, the click puts the slices on the pads.
- **The page's Kit tab:** Decay, Sustain, Filter, Resonance, Trim silence (setting 31), Toggle.
- **Saving:** project version 14 (20 settings a pad; 13, 12, 11 read with Decay 0, Sustain 100, the filter open);
  kit.json gains "decay", "sustain", "cutoff", "reso" (missing: the same defaults).

## Round C, 2026-11-26

The owner's ask: "you load in samples or record samples to each pad, then these samples become their own thing, so
you can then crop, transform, resample, freeze effects, stretch, whatever and it does not modify the original samples
you loaded in. Similar to Koala. Then when you save a Kit, a Kit is stored as a folder with a collection of its
samples." Built for rapid iteration (no new tests; the existing ones kept passing).

- **The project's own folder:** `Samples/RemoteFM1/Kits/Projects/NN/` (NN the project's slot, 01..32). Move's own
  browser sees it (as it sees Kits/); Files' Kits never lists it (no kit.json). The core keeps a path there as
  `@/<name>` (`RFM_MOVE_KIT_OWN`: the open project's folder, whatever its slot), so a rename never touches it and a
  project cut and pasted or duplicated keeps its sounds: the host's storage moves the folder with the project as it
  moves the firmwares' copies (delete: the folder deleted; cut: moved into `Projects/.clip`; copy: copied there;
  paste: copied from there; the page's duplicate: copied; an import: an empty folder). Saved bytes unchanged:
  project version 14 still.
- **Kept sounds are copied in** on the file thread: Files' click, Files left any other way (another tab, the view
  closed, another pad's browse) after the jog put a sound on the pad (`KIT_KEEP`), All To Pads, To Kit and a take's
  slices (`KIT_LOAD` param 1), a kit loaded, a take recorded into a pad, the sampler's next empty pad. While browsing
  the jog still loads the original (nothing written). A copy is named as its source (" 2", " 3" .. when another file
  has the name); a file already in the folder, the same source copied before, or a file there with the same bytes is
  not copied again. A take recorded into a pad is copied: the take library keeps its own entry.
- **Edits write into the folder:** Mix, Join, Trim, Normalize, Reverse, Freeze; `Kits/Edits/` only when there is no
  project folder (older projects pointing there still load). An older project's pads keep their paths until a keep or
  an edit; nothing is migrated on load.
- **Clean-up:** when the open project is saved (the autosave, the pad's save, leaving it), the folder's files that
  neither a pad, nor the Undo (a pad's, the whole kit's), nor an answer on its way refers to are deleted on the file
  thread: regular files right in that folder only, a minute old at least (a file just made is never at risk), never
  anything outside `Kits/Projects/`.
- **Freeze** (the Sound row's tools: Play, Trim, Normalize, Reverse, **Freeze** (a snowflake), Delete; the page's
  Kit tab: a Freeze button): the pad rendered offline on the file thread by a voice of the kit's own code into
  `<name> frz.wav` (16-bit stereo, 48 kHz): its region, pitch and fine, stretch (the repitch rendered), reverse, the
  AHDSR, the filter, level and pan, at full velocity. Length: **One shot** the whole note to its end, the release its
  tail; **Momentary, Loop, Toggle** held for the region once (at the pad's rate), then released (the release's
  tail). At most 60 s and the kit's memory (else "Too Long"); past full scale it clips. Then the pad has the new file
  with pitch 0, stretch Off, reverse Off, the region whole, the envelope default (A, H, D, R 0, Sustain 100 %), the
  filter open, resonance 0, level 0 dB, pan centre; play mode, choke, mute and the sends stay. "Freezing" at the
  click, the pad blinks white while it renders, "Frozen / Pad N" when done; one Undo brings the unfrozen pad back.
  The send FX are not baked: they run in the engine's chain.
- **Kits:** Save copies the sounds the kit uses (from the project's folder) into `Kits/<name>/` with `kit.json`, as
  before. Loading a kit copies its sounds into the project's folder, so editing never changes the saved kit.
- **Follow-up:** **Freeze with FX** (the sends rendered through the audio FX); a sound's original place for Files
  after a restart (within a run Files opens on the source of a copy it made; after one, on the project's folder).

## Pages, 2026-11-26

The owner's ask: "I want the Jogwheel behaviour to function like it does on the sequencer pages. You turn jogwheel and
it changes a page, while turning you see the page markers on the bottom and see the name of the page briefly, This
goes away and you are on the page." Then their own page design (the pages, their order and names, the browser).
Built for rapid iteration (no new tests; the existing ones adapted). Nothing new saved: project version 14 still.

- **The jog turns the pages** as the sequencer's knob pages turn: a detent a page, stopping at the first and the last;
  while it turns the page's name big and the seven page marks at the bottom (v2_modeName, as the ENGINE page's
  arrival), RFM_MOVE_POPUP_MS (a second) after the last detent, then the page. The page is the Kit view's (in RAM),
  not a pad's. No tabs, no rows, no Shift knob page.
- **A page:** its picture at the top (x 0..101; the chosen pad's number 2x top right), then 8 cells in two rows of
  four as the FX pages draw them (a short name and the value at 1x; a knob with none: a dash, its light dark).
  Sample Kit is all picture.

| Page | Picture | Knobs 1..8 |
|---|---|---|
| Sample Kit | 16 lanes of 4 px (pad 1 at the bottom): each pad's hits over the 16 steps shown, a pad with no sound empty, the chosen lane underlined, each lane's playhead while playing | Right half (Pitches / In Key / Off), Trim silence, the rest dark |
| Pad Sample | the wave over the region, no name (the click's browser opens on the sound) | Pitch, Fine, Start, End, Play mode, Reverse, Choke, - |
| Pad Sample 2 | the AHDSR | Attack, Hold, Decay, Sustain, Release, - |
| Stretch | the wave, the steps it lasts | Stretch, - |
| Filter | the filter's curve | Filter, Resonance, - |
| Pad Mix | the level as a bar, the pan a mark (muted: hollow) | Level, Pan, Send 1, 2, 3, Mute, - |
| Kit Mix | the kit's name, its 16 pads | the kit's Level, Pan, Send 1, 2, 3 (the Mixer's Kit strip), - |

- **Knobs:** the popups as before (Start / End the close-up, Shift 32x and fine; the envelope's stage thick; the
  filter's curve; the right half's layout and key; the kit's strip a bar). Shift is fine adjust.
- **The click (a pad's page): the browser**, as Move's: three rows, an icon on each row's left. The root: Takes,
  Samples, Kits (folders: a right arrow). In a folder **Back** is the first row (a left arrow): its click goes a
  folder up (at a place's top: the root); a folder's click enters it; a sound (a small wave) put on the pad as the
  jog rests on it (round A's 120 ms); its click keeps it and leaves; the Back button goes a folder up, then the root,
  then leaves (the sound kept: round C's copy-in). A pad pressed: the target, the list on its sound (round A).
- **The click on Sample Kit or Kit Mix:** Kits, Load (the browser's Kits) and Save.
- **Shift + the click:** the pad's tools (Play, Trim, Normalize, Reverse, Freeze, Delete).
- **Unchanged:** a step held + a knob (the lockable settings on the page shown, lit yellow, the rest dark), a step
  held + the volume knob, the pads, the steps, Copy / Delete / Mute with pads, the right half, Slice, recording into
  a pad, the routing fix.
