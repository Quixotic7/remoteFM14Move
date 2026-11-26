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
  brings the pad back. Release of Copy before a choice: nothing done. (Reworked 2026-11-26: "Copy on the jog" below.)
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

## Pages v2, 2026-11-26

The owner's page design, then their revisions the same day (engines and Warp, the right half, the screens). Built for
rapid iteration (no new tests; the existing ones adapted). Project version 15; kit.json version 2.

**The pages** (the jog as the sequencer's; no list of settings on a page: its picture, a knob's value only while it
is touched or turned, full screen, the knobs' lights saying which have a job):

| Page | Picture | Knobs 1..8 |
|---|---|---|
| Sample Kit | the 16 lanes (4 px each, pad 1 at the bottom; a step a 4 x 4 cell 2 px apart: a hit its lower three rows, an empty step a 2 px dash; the chosen lane an arrow on the left; the playhead's cell inverted); an empty pattern: the page switcher | Right half, Trim silence |
| Pad Sample | the wave over the region above the pad's 16 steps (drawn as a track page's step strip, Move's strip under it); Loop's loop and fade, Granular's position and spray marked under the wave | Start, End, Transpose (Shift: cents; -24..+24), Engine, 5..8 the engine's first four own settings (2026-11-26, the owner's; One Shot, Momentary: dark) |
| Trig | the pad's track over 16 steps, the trigged steps marked | the sequencer's TRIG page for the pad's track (13..16): Micro, Ratchet, Chance, Cond, Func, its amount, Length; 8 dark |
| Pad Sample 2 | (2026-11-26) the amp envelope above the steps | Attack, Decay, Sustain, Release, 5 and 6 the engine's further settings (Granular: Spread), 7 Choke, 8 Reverse |
| Warp | the wave, the steps it lasts (Off: the switcher) | Algorithm, Sync, Speed, Pitch, the algorithm's two |
| Filter | Fever Dream's: the filter as bars (cutoff, resonance, morph; a tick at the knob's cutoff, a dot where the envelope takes it) on the left, its envelope on the right, both live (the pad's newest voice, `rfm_move_kit_live`) | Cutoff, Res, Morph, Env Amount, F Attack, F Decay, F Sustain, F Release |
| Pad Mix | the level as a bar, the pan a mark | Level, Pan, Cutoff, -, Send 1, 2, 3, - |
| Kit Mix | the kit's name, its pads, its level | Level, Pan, Cutoff, Res (the kit bus's low-pass), Send 1, 2, 3, - |

- **The page switcher** (while the jog turns, and a page with nothing to draw): the eight pages' icons in filled
  cells, the shown one inverted, its name under them at one size (1x); no marks.
- **The Filter page's knobs** keep the page (the owner's exception): their name and value in a line at the top, the
  part being changed thick (the envelope's stage, the cutoff's tick).
- **Pitch-like popups** (Transpose, Warp's Pitch, Pitch Env's depth) are bars from the middle.
- **The browser:** three rows, the chosen one fixed in the middle with its icon (Back a left arrow, a folder a right
  arrow, a sound a small wave), the others text only, no inverted row; a turn slides the list a row under the icon
  (100 ms, eased; the next turn starts its own). Five rows asked at a time.

**The engines** (Pad Sample's knob 4; their own settings, 2026-11-26: the first four on Pad Sample's knobs 5..8, the
fifth on Pad Sample 2's knob 5; numbered below in that order):

| Engine | Plays | Own settings |
|---|---|---|
| One Shot | the region once | - |
| Momentary | the region once while held | - |
| Loop | (2026-11-26, the owner's sustain loop) from Start to Loop End, then round Loop End -> Loop Start while held (a step's note: its length); let go, on through Loop End to the region's End, the release its tail; reversed: the mirror; Warp's grains the same | 1 Loop Start, 2 Loop End (inside the region; a percentage of it, "START" / "END" at its ends; their popup the region's wave with the loop's markers, no close-up; a Loop pad's wave always carries them), 3 Loop Fade (0..100 % of half the loop: the loop's end crossfaded into its start, while held) |
| Pitch Env | as One Shot, the pitch moved by an attack / release envelope | 1 Depth (-48..+48 st), 2 Attack, 3 Release |
| LoFi | as One Shot, bits and rate reduced | 1 Bits (1.0..24.0 in 0.1 steps, "12.5 bit"; a real depth, the quantum 2^(1 - bits) of full scale, smooth as it turns; 24.0 transparent; stored in tenths 10..240, locks too; 2026-11-26), 2 Downsample (a sample held 1..32 frames) |
| Granular | grains of the region while held | 1 Position (where it starts), 2 Scan (2026-11-26: -100..+100 %, Off at 0: the position moves on from Position while the voice plays, forwards or back, wrapping round the region; (Scan / 100)^2 passes of the region a second, finer near 0; bipolar popup "+35 %"; locks as the rest), 3 Size (10..500 ms), 4 Density (1..16 grains), 5 Spray, 6 Spread (0..24 st) |

Toggle is gone: a pad saved with it loads as Loop.

**Warp** (all engines but Granular; Off by default): 1 Algorithm **Off, Repitch, Grains, Beats**; 2 Sync (Off, 1..64
steps: the old Stretch, re-fitted when the tempo or the track's rate changes); 3 Speed (0.25..4.00, 0.05 a detent,
Shift 0.01; Sync on: 1/4x, 1/2x, 3/4x, 1x, 1.5x, 2x, 3x, 4x of the synced length); 4 Pitch (semitones, Shift cents;
dark for Repitch); 5 Grain (20..120 ms), 6 Grains' Jitter or Beats' Search (1..30 ms).
- **Repitch:** the rate. With Sync on the fit sets it and Transpose, the right half's pitches and the steps' pitch are
  ignored (the owner's rule); without Sync, Speed times the pitch.
- **Grains:** overlap-add of two Hann grains half a grain apart (their windows sum to one), each read at the pitch,
  the playhead moving at the speed; the first grain starts in its middle so the attack is kept; jitter moves each new
  grain's start.
- **Beats:** the same with WSOLA alignment: each new grain starts within +-Search of the playhead where its frames best
  match (normalised cross-correlation, mono, every 4th frame over 256, offsets every 4th) what the fading grain plays
  next. For drums and loops.
- Both keep the pitch (Transpose, the right half and the notes still apply, with Warp's own Pitch on top); they honour
  the region, Reverse, the loop, Pitch Env, LoFi, the envelopes and the filter. Elasticat's modes were looked at for
  which ideas to have; no code taken.
- The old Stretch value N loads as Warp Repitch, Sync N.

**The filter:** Morph blends the state-variable filter's LP -> BP (unity at its peak) -> HP; Env Amount (-100..+100)
moves the cutoff up to 8 octaves either way by the filter's own ADSR (a voice's, at the coefficients' 32-frame rate).
Open, Env 0, Morph LP: bypassed as before. Morph and Env Amount are lockable (cc 136 + l x 4 + pad % 4, l 7, 8).

**The kit bus:** a stereo low-pass on the voices' mix before the kit's level, pan and sends (the pads' own sends are
not through it); open: bypassed. KIT_SET with index 16 (`RFM_MOVE_KIT_BUS`), param 0 cutoff, 1 resonance; saved with
the kit (header bytes 10, 11) and kit.json `"bus"`. Not in the Mixer's Kit strip (follow-up).

**Trig on a kit track:** the TRIG page for the chosen pad's track, as a firmware track's: no step held, its knobs set
the track's defaults for new steps; steps held, their trigs (the step's release then toggles nothing). A trig belongs
to the step: every pad hit on that step of that track gets it (four pads share a track). The kit's notes come from the
sequencer's clip_note after the ratchets, chance and conditions, so they play as a firmware track's do.

**The right half** (Sample Kit's knob 1): **Pitches** (16 semitones up), **In Key** (16 notes of the key's scale in a
row, upward), **Chromatic** (rows a fourth apart, chromatic in a row), **In Key 4ths** (round A's In Key), **Chop**
(the chosen pad's region in 16 equal chops, the top left the first, reading order; one slot held plays its chop once;
two held loop from the lower's start to the higher's end while both are held; the pads in the pad's row colour, the
chops sounding bright). Off is gone: Off loads as Pitches, the old In Key as In Key 4ths. **A chop's encoding:** the
pad's own note with two new step locks, Chop and Chop End (`RFM_MOVE_KIT_LK_CHOP`, `_CHOPE`: chops 0..15 of the region
at play time, so a region edited later re-chops); one chop plays once, a range loops while the note lasts. A step held
+ a slot enters (or removes) that chop's hit; recording records the hit and its locks (a two-slot range on the first
slot's hit).

**Saving:** 45 settings a pad (`RFM_MOVE_KIT_P_*`, the new ones after Resonance: loop points and fade, Pitch Env,
LoFi, Granular, Warp, Speed, Warp Pitch, its grain, jitter, search, Morph, Env Amount, the filter's ADSR), the kit
bus. Older projects: Classic sound (Warp Off unless Stretch was set, speed 1.00, no filter envelope, Morph LP, the bus
open). kit.json version 2 has the new keys and `"bus"`; a version 1 one maps Toggle, Stretch and the layouts as above.

**CPU** (the scratch renderer on the Mac, 24 voices; the Move's A72 is about 3..5 x slower): One Shot / Loop / Pitch
Env / LoFi 0.2 ms a voice-second, the filter with morph and envelope +0.04, Grains 0.66, Beats 1.2, Granular 0.8
(density 4) .. 1.6 (16). On the A72: about 1 ms a voice-second for the plain engines (24 voices about 2.5 % of a
core), 3 for Grains, 5 for Beats, 3..7 for Granular.

**Not done:** the Mixer's Kit strip for the bus filter; Pad Sample's amp envelope in Fever Dream's drawing (its popup
is round B's AHDSR); live envelope dots on Pad Sample; the page's Kit tab has rows for the new pad settings but none for
the kit bus.

**Follow-up (the owner, the same day):**
- **The page switcher:** the icons at 2x (22 px) in 28 px cells, the shown page's in the screen's centre as a white
  cell with its icon black, the others white on black with no box; a window of about five cells, running off the
  edges (no wrap: black past the first and last). A turn slides the row a cell (110 ms, eased; the next turn starts
  its own). The name under it at 2x, one size for all; "Pad Sample 2" is shown as "Sample 2" (the only name past 10
  characters). Row and name centred together (y 7..34, the name at 41).
- **The Engine popup:** the engines' icons, the chosen one marked, its name; no envelope.
- **Pad Mix:** the wave on the left; a level meter (muted: dithered), the pan a mark on a short line under it, the
  three sends as small bars right of it; no text.
- **Kit Mix:** the 16 pads' meters in two rows of eight (pad 1 bottom left): each its Level setting (the host reports
  no per-pad levels), a cap while it sounds, the chosen one marked; no text.
- **The browser:** Back and folders marked by plain filled triangles (left, right); a sound's small wave stays.
- **The pad's sound tools are gone from the kit** (the owner: "Nix the sample editing tools in the drum kit, then
  just keep those as something you can use inside the sampler"): Shift + the jog's click does nothing in the Kit view;
  Play, Trim, Normalize, Reverse, Freeze and Delete are no longer offered (the page's Freeze button too). The core
  sends no KIT_EDIT (`rfm_move_kit_edit` returns 0); the host's KIT_EDIT / Freeze code stays, unused. The sampler's own
  take tools (Sample: Takes, a take's Tools) are unchanged; Copy / Mix / Join, slicing, Delete held + a pad and the
  project's own copies stay.

## Browser, 2026-11-26

- **The rule (standing):** a sample plays only when its pad is pressed or the sequencer plays it, always through the
  same path: a kit voice with the pad's settings, the kit's strip (level, pan, sends), the kit bus's filter, the FX and
  the volume knob's master. Nothing plays by itself: no landing in the browser, no Start / End / loop point at rest
  (KIT_AUDITION is no longer sent; the host's audition and Files preview are retired), nothing after a recording, a
  kit load, All To Pads or slicing. The slicing screen's slice pads and Tap's pass are kit voices of a hidden pad
  (the defaults: one shot, 0 dB, centred, no sends) on the kit's bus. The sampler app's own take playback (Sample's
  window) is outside the kit; it now follows the master volume too.
- **The root:** My Samples (the Move's `UserLibrary/Samples`; RemoteFM1's takes are in `RemoteFM1/<day>/`) and the
  Move Library (Move's factory sounds, read-only: `/data/CoreLibrary`, its `Samples/` when there; the host's
  `--core-library DIR`; missing: the place is absent). Takes and Kits are no longer in it: a take is a file in My
  Samples, kits stay Sample Kit's (and Kit Mix's) Load / Save (Back from Kits returns to Load / Save). Files still
  opens on the pad's sound: a take's in My Samples, a Move Library sound's there (its project copy traced back).
- **The Move Library's index:** built once a launch on the kit's file thread at its first use (never the audio
  thread; since FLAC, its own thread, "Reading..." until done): every folder below it walked, each sound probed;
  listed: the folders with a sound this host reads somewhere below them and those sounds (PCM 16 / 24 / 32-bit and
  float WAV, PCM AIFF, AIFF-C NONE / sowt, FLAC), Move's categories nested as on disk; a folder's sounds not read as
  one dead row a kind ("3 .ogg (unsupported)"); presets hidden. Kept, its sound is copied into the project's own folder (round C).
- **The marquee:** the chosen row's name wider than its 112 px scrolls: 1 s still, 25 px a second (whole pixels) to its
  end, 1 s, back, again; from its start when the choice changes. Other rows are cut at 9 characters.
- **Steps (the same day):** a step's click toggles at its release only when it was a click (UX-RULES R2: held 300 ms or
  more, or a knob, the volume knob or a pad used while down: nothing). A pitch pad (Chop: a slot) held + a step's
  click puts the chosen pad's hit there at that pitch (chop); again: out. A held step lights the pitches (chops) the
  chosen pad plays on it, bright in the row's colour; a step held + a pitch pad toggles that pitch.
- **Layout (Shift + step 8)** in the Kit view sets the right half's mode (Pitches, In Key, Chromatic, In Key 4ths,
  Chop: five icons, the chosen one marked and named; the jog or knob 1), as Sample Kit's knob 1.
- **Clearing, scoped by the page (the owner's 2026-11-26):** on a pad page Delete + Play clears the chosen pad's
  hits, ghosts and locks over its clip (its track's other pads keep theirs), Delete + a knob that setting's locks of
  the chosen pad; on Sample Kit or Kit Mix Delete + Play clears all four kit tracks' clips and Delete + any knob (either
  order) every pad's locks, kit and trig ("Cleared All" "Locks"). One undo step each, said.
- **Locked or not, on screen (the owner's 2026-11-26):** with a step held, every knob popup (each pad page; firmware
  tracks' CC and TRIG knobs, the FM-1 and ENGINE pages' trigs) shows the step's state: locked, the value solid and a
  7 x 9 padlock in the screen's top-right corner (x 119..125, y 1..9, every popup, the Filter page's line too; the
  value stays where the unlocked one is, a title reaching the corner clipped); not locked,
  the pad's (track's) own value hollow (a 50 % checker), no padlock. A turn locks it, the padlock at once; step +
  knob + Delete clears it, hollow at once. Several steps held: locked if any is, the first locked one's value. No
  step name ("S5") nor "Lock Cleared" on the screen any more; the held step's velocity popup is "Velocity".
- **Step lights (the owner's 2026-11-26: a ghost looked like an empty step):** the chosen pad's steps: empty dim
  white; a hit its row's colour, brighter the harder; a locked hit blinks slowly (1 s round); a ghost trigger (locks,
  no hit) the row's colour as a steady pastel, half way to white and brighter than an empty step (outside the loop
  dimmer); the playhead white. The step keys take any RGB colour (Move's sysex, no hardware animation on that path).
- **Step locks on every pad setting:** a step held lights every knob of Pad Sample, Pad Sample 2, Warp, Filter and Pad
  Mix yellow (Sample Kit, Kit Mix dark; Trig its own). The nine round B locks stay as they were; any other setting
  takes one of nine slots a hit (two sequencer locks: the setting, its value), so a hit locks the nine and up to nine
  more ("Locks full" past them). No project format change. The host applies each lock to that hit only: at note-on
  (engine, Warp, Sync, region, envelope, choke ...) and while it sounds (the filter, its envelope, LoFi, Granular, Warp
  speed and pitch), the pad's later edits leaving a locked value alone.

## Copy on the jog, a held step's length and timing, 2026-11-26

The owner's, after testing: "instead of having to turn and click the jogwheel each time, you should while holding copy
just turn the jogwheel to change the mode, it should default to Copy whenever you release copy and press again ... If
the other pad is empty, then for mix and join it just does a copy to the empty pad slot." "Holding a step + turning
the jogwheel should always change the step length." "Holding a step plus using the left and right arrow keys should
be a shortcut to always adjust the microtiming."

- **Copy held:** the screen shows Copy, Mix, Join (board N's icons), the current one marked and named; the jog's turn
  changes it (no click; the pages stay); every Copy press starts on Copy. The first pad pressed is the source (it
  blinks; its number and an arrow on the screen). Each further pad is a target, done at once in the mode: **Copy** the
  source's sound and settings ("Copied 2>5"); **Mix**, **Join** the host's new file on the target ("Mixed 2+5",
  "Joined 2+5"); an empty target: Mix and Join just copy; an empty source: "Empty". The jog's click does nothing (the
  old confirm step is gone). Copy let go ends it; Back ends it until Copy is let go. Copy + a step stays the
  sequencer's step copy.
- **Repeated targets** while Copy is still held: each gets the same operation from the same source. Move's own Copy
  keeps its clipboard until Copy is pressed again (manual 6.1.4, 11.8, 17.1.3: copy a pad, then press where to paste),
  so a held Copy pasting onto several pads matches it; a new source needs a new press of Copy. One Undo brings back
  every target of the hold (one target: that pad; several: the kit as it was before the first).
- **A step held + the jog:** the chosen pad's hits on the held steps get a new length, as a firmware track's held step
  (2 ticks a detent, about a tenth of a step; Shift a whole step; 1 tick .. the clip's length); the LENGTH popup
  ("Length", the value, the bar strip); the pages never turn while a step is held. **+ Left / Right:** their timing
  (micro), 2 ticks a press, Shift 1, clamped to the step's -12..+11; the Nudge popup ("+16%"). Firmware tracks already
  do both. The other pads' hits on the step are untouched. One undo step a hold for each. A ghost (locks, no hit) or an
  empty step: nothing (a ghost has no timing or length of its own).
- **Polyphony between pads** (the owner: "I sequenced a pattern on one pad and then another pad with triggers on same
  steps and only one pad plays"): not reproduced. On the core (d457792 and 1958869, the installed host's) two pads' hits
  on one step each send a KIT_NOTE (a shared track or not; steps stopped or playing; a held step + pads; Record; a
  locked hit beside a plain one); the host's voices sound together (kit_test: same block, same frame and apart, the
  held engines, locks and a ghost beside); on the simulator two pads on one step give two voices. Only pads sharing a
  non-zero choke group cut each other; a pad again takes its own voice back; past 24 voices the oldest is stolen.
  Kept as checks: move_kit 63, kit_test t_poly.


## Plain hits, playing pads, original places, Export, a held step's velocity, 2026-11-26

The owner's, after trying the build: "When I click to add a new step, it should start with default trig values, like
length 1, chance 100, etc. It seems like when I add a new step currently it contains lock on it for every parameter."
"I had playback going ... sequenced a pattern ... but none of this was playing until I stopped playback and started
again." "After closing app and relaunching, when I click a pad's sample it is no longer pointing to the original
sample ... I would like it to always point at the original sample location. Then maybe in the track settings, shift +
S3, add a feature in which you can export the samples in the kit as a group. We shouldn't be modifying any of the
original samples." "Holding a step and turning the volume knob should always change that steps velocity."

- **A new hit is plain.** The cause: a kit track took the sequencer's TRIG defaults for new steps (the Trig page's
  knobs with no step held set them for the chosen pad's track; an older project may carry them), so every tapped or
  recorded hit got that track's Ratchet, Chance, Cond and Func as locks (the padlock on each), its micro and its
  length. Now a track routed to the kit takes none: a tapped step (or a Record hit, a step held + a pad, a Chop slot)
  is one step long, micro 0, chance 100, no locks; only a held step + a knob locks. The kit's Trig page with no step
  held is dark and dead (a firmware track's TRIG defaults are unchanged; turned on a kit track in the sequencer, they
  do nothing). Locks already saved stay: they look the same as real ones, so loading never drops any (Delete + a knob,
  Delete + Play clear them).
- **Playing pads.** Not reproduced as such: on the core and on the simulator a pad of another track, sequenced while
  playing (a sound loaded meanwhile, its track stopped or never started), sounds on the next pass. The likely cause is
  the one above: a hit carrying a TRIG default Cond such as 1st (or Pre, Fill, a chance) plays only on the first pass
  after Play, or not at all, which a stop and start "fixes". Gone with plain hits; move_kit 64 checks it with such
  defaults on the track.
- **Original places.** A pad remembers where its sound came from (`orig`, project version 18: 16 paths at the kit
  part's end; 17 and older load with none): a kept load's copy (Files' click, All To Pads, To Kit, slices, a take into
  a pad) names the file it copied (the host's KIT_A_LOADED `was` -> `rfm_move_kit_set_orig`, KIT_KEEP's OWNED ->
  `rfm_move_kit_owned`). KIT_FIND carries it after the path (a NUL between): Files opens on the original, highlighted,
  while that file is there, else on the project's copy. The pad still plays the copy (the project keeps working when
  the original moves; copies and pastes of projects carry absolute originals unchanged). Derived audio (Trim,
  Normalize, Reverse, Mix, Join, Freeze) is a new file in the project's folder (never written over anything: each
  edit `new_path`s a free name) and is its own original. A loaded saved kit's pads open on the project's copy.
- **Originals are never written.** Checked: every write of the kit is a new file in the project's folder (`own_copy`,
  edits, Freeze), `Kits/<name>/` (Save) or the export folder; the clean-up deletes only regular files of
  `Kits/Projects/NN/`; the takes are the recorder's new files.
- **Export Samples:** a kit track's settings (Shift + step 3) get a fifth row, Export Samples, the jog's only (knob 5
  stays dark); its click sends KIT_EXPORT (the kit's name, else "Project NN", then each pad's sound's name). The host
  copies each pad's file as it is (the project's copy or an edit's) into `My Samples/<name>/` (" 2", " 3" .. when that
  folder exists: nothing overwritten), as `NN <name>.<ext>` ("01 Kick.wav"), on the file thread, and says "Exported
  12" (or "Not Exported"); nothing to export: "No Sounds".
- **A held step + the volume knob:** the velocity from the touch on: the touch shows "Velocity" (no Master popup over
  it), the turn sets the chosen pad's hits there (1..127), the master untouched; one undo a hold; an empty step or a
  ghost (no hit): nothing (as a firmware track's held step). The cause: the volume knob's touch went to the main
  handler, which put the Master level popup over the kit's velocity popup, so the turn looked like a master change.
  Firmware tracks: the same touch now shows their VELO popup too.
