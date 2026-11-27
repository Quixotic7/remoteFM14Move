# The looper, stage 1: as built

Source: the design round `LOOPER-DESIGN.md` (cards in `cards.json`, screens in `mock_looper_v1.c`,
`looper-v1-screens.png`, `looper-v1.movedesign.json`). The owner's answers of 2026-11-26: every card approved as
its option a, and on `lp_scene` "A and b and c": all three ways to launch a scene. Shift + a pad, proposed as "choose
silently", now launches the column; choosing a slot without playing it moved to **Loop held + a pad** and **the
arrows** (below). Stage 1 is card `lp_stages` a: the grid, recording, launching, scenes, pages, repitch on tempo
change (and Warp Off), the Loops strip, saving with the project, the sampler's Loops tab, Copy / Delete / Undo.

Code: rfm1cs-abletonmove `src/looper.c` (the face: gestures, lights, screens, the project's part; README "The looper
(stage 1)"; tests `tests/move_looper.c`) and this repo's `src/looper.c` (the audio: recording, voices, the bus, the
files; `tests/looper_test.c`, `tests/looper_e2e.sh`).

## Where it lives

- Track 4's taps go round **Mixer, Routing, Kit, Looper** (a double click: the projects). Track 4 held: pad 1 Mixer, 2
  Routing, 3 Kit, **4 Looper** (teal), 5 the projects. From another Track, Track 4 opens the view it was left on
  (settings byte `t4_kit`: 2 the Looper). Track 4 is teal in the Looper view.
- The Looper view is a layer over the sequencer: it takes the pads, the steps (no Shift), the knobs, the jog, Copy,
  Delete, Undo (when it has one), Loop, Mute, the arrows and Sample (when it has a job for it). Play, Record, Capture,
  Shift + a step and Menu stay the sequencer's.

## The grid (lp_grid a, lp_lane_choke a, lp_colour a)

- The pads' rows are 4 **lanes**, lane 1 the top row (beside Track 1); the columns 8 **scenes**. Slot = lane x 8 +
  scene. A lane plays one loop at a time: launching another in its row replaces it at the launch time.
- Lights: empty dark; a loop dim teal {0, 210, 190}; playing green {0, 255, 60}; lined up (waiting) blinking green;
  about to stop (another lined up, or a stop) blinking teal; armed blinking red; recording red; muted grey; the
  chosen slot white while it is idle (playing, waiting or recording: its state's colour). Copy's source blinks white.

## Recording (lp_rec_gesture a, lp_rec_start a, lp_length a, lp_source a, lp_after_rec a)

- **Tap an empty slot on the Record page:** it records from the next bar line (round 2: on the other pages the tap
  picks the slot; below). Stopped: armed, it starts on Play's first frame. Armed,
  a tap again cancels it ("Cancelled").
- **Tap the red pad, or Sample:** it ends at the next bar line (at least one bar). A lane's **Length** (Open, 1, 2,
  4, 8, 16 bars) ends a fixed one by itself.
- **It plays at once,** from the bar line it ended on, without a fade; its lane's other loop stopped at the bar the
  recording started.
- **A slot held + Sample:** records into it, full or not; Undo brings the loop it replaced back.
- **Another recording running:** a tap on an empty slot ends that one at the next bar and starts the new one on the
  same bar line (one spare buffer each). One armed elsewhere is replaced.
- **The transport stopped during a recording:** its whole bars are kept (none: "Nothing"); it does not play by itself.
- **Source (per lane):** Played (the firmware the Track buttons chose last, resolved when it starts), Track 1..3 (a
  slot dry), Kit (the kit's output alone), Input (the Move's input), Mix (the slots and the kit at their levels,
  before the FX returns and the loops), Loops (the looper's own output: a bounce).
- **The bar's maths** is the recorder's (`src/barcut.h`, shared): a line lands on the first frame at or after it, so a
  loop is whole bars of beats at the tempo it was recorded at. A recording never goes through the recorder's take:
  the sampler records meanwhile.
- **Memory:** three spare buffers of 64 s each, made when the host starts; the loops' memory is capped at 256 MB ("Too
  Long" past it). A recording past its spare keeps its whole bars and does not play by itself.
- **Its file:** the file thread writes `Samples/RemoteFM1/Kits/Projects/NN/Loops/<lane>-<scene> <bpm>bpm <bars>b.wav`
  (" 2", " 3" .. when the name is taken: a file Undo may bring back is never overwritten), 16-bit stereo at the
  engine's rate. The core keeps it as `@/Loops/...` (the project's own folder). The engine plays the spare buffer at
  once; the file thread makes the loop's exact copy and the engine swaps it in.

## Launching and stopping (lp_launch a, lp_quant a)

- **A loop's tap:** it plays at the project's Launch setting (Global settings: Bar by default, Beat, 2 or 4 bars,
  Clip: the end of the lane's loop, Instant). A playing loop's tap: it stops there.
- **A double click (UX-RULES R2's "same thing sooner"):** the tap acts at once; the second tap within 300 ms does it
  now (Instant). Instant is in phase: the loop's position is the song's from the last bar line, so it is on the bar.
- **Its position is the song's** (the beat at each frame), never a free-running counter: it cannot drift.
- **Stopped:** a launch waits for Play; every lane's loop starts again on Play's first frame. Nothing plays by itself.

## Scenes (lp_scene: a, b and c)

- **a.** The left step under each column (steps 1, 3, 5 .. 15) plays that scene in every lane at the launch time; a
  lane with no loop in it stops. Their lights: dim white, green while a lane plays the scene, blinking green while it
  waits. **Step 16 stops all** (dim red, its icon lit: a function). The even steps are dark and dead.
- **b.** **Shift + any pad of the column** plays its scene.
- **c.** **A slide down the column:** a pad pressed while the pad above it in the same column is held (or let go
  under 80 ms ago) and was pressed under `RFM_MOVE_LOOP_SLIDE_MS` (400 ms) ago plays the column's scene. The first
  pad's own tap is overridden: a recording it armed is cancelled; its launch or stop is replaced by the scene's. A
  slide over more pads launches the scene once.
- A double click on a scene's step (or step 16) does it now. Stopped, a scene starts the transport (as session view's
  scene pads).

## Choosing a slot without playing it (the owner's addition)

Shift + a pad launches the column now, so choosing silently is:
- **Loop held + a pad:** that slot chosen, nothing plays or records (Loop is the looper's in this view: its tap does
  nothing, the sequencer's loop layer does not open).
- **Left / Right:** the chosen slot a scene left or right; **Up / Down:** a lane up or down.
Any pad's tap also chooses its slot. (Round 2: an empty slot chosen on the Loop or Loop Mix page no longer opens the
Record page; those pages show it empty.)

## Pages (lp_pages a): the jog, as the kit's

The jog turns five pages (clamped at the ends); while it turns, the switcher: the pages' 2x icons in 28 px cells
sliding, the shown one a white cell, its name under it. A knob touched: its popup (name and value) while touched and
a second after; turned: its value changes; Shift: fine. A page's knobs are lit teal; a dead knob is dark.

| Page | Picture | Knobs 1..8 |
|---|---|---|
| Loops | a lane a row: the scene it plays (2x), its bars as ticks, filled as it plays; waiting: the scene (or "-" a stop) and a dashed box with the beats; stopped: a dotted line; recording: the dot and its wave growing | lanes 1..4 Level |
| Loop | the chosen loop's wave over its recording, the markers (outside them dithered), the playhead (past it dotted), its bars, the slot's place top right (the grid in small); empty: a dotted line, "Empty" | (round 2) 1 Start, 2 End (the markers), 3 Steps, 4 Pitch (semitones; Shift: cents), 5 Reverse, 6 Warp (Off, Repitch, Grains, Beats); 7, 8 dark |
| Record | (round 2) the lane's Source, Length and Count-in rows, under them "Mon Off  0 dB" (the sampler's Monitor and In Level), "STP" while the step looper is on | 1 Source, 2 Length (the chosen slot's lane), 3 Count-in, 4 In Level, 5 Monitor, 6 Step Looper |
| Loop Mix | the wave, the level as a fader (muted: hollow), the pan, sends 1..3 | 1 Level, 2 Pan, 3..5 Send 1..3, 6 Mute |
| Looper Mix | the lanes' levels (a cap: it sounds; the chosen loop's lane underlined), the Loops strip's fader, pan, sends | 1..4 the lanes' levels, 5 the Loops strip's level, 6 its pan, 7, 8 its sends 1, 2 |

An empty slot's Loop and Loop Mix pages show it empty (round 2); its loop knobs are dark. Every level is 0..127 with 100
= 0 dB (a 0.5 dB detent, the shared level popup `rfmv_level_popup`): a loop's ("Loop 2-3 Level"), a lane's ("Lane 2
Level": every lane and the strip as siblings), the strip's ("Loops Level").

**Popups:** Pitch (the value, a bar from the middle), Length (the bars recorded, those played filled: board E), Start
(a bar's 16 steps, the start on: board F), Source ("Played 2", "Track 1" .. : Track 1..3's button bright: board H),
Pan and the sends (a bar).

**While a recording is armed or runs** the view shows it (boards I, J, K): the dot, the source's sign (its track's
number, K, the input's icon, M, L); armed: four squares, the beats gone filled, the place; recording: the bar (Open)
or "bar/bars" (fixed), the beat marks (Open), the bar's wave growing, the source's level, the bars as boxes.

## The volume knob

- **A slot held + the volume knob:** that loop's level (its popup while touched or turning).
- **Track 4 held + the volume knob** in the Looper view: the Loops strip's level (elsewhere: the kit's, as before).

## Copy, Delete, Undo, Mute (lp_undo a)

- **Copy held + a loop:** the source (blinking; the screen: its place and an arrow, "Copy"); each further slot is a
  target, done at once (the same audio and settings, no new file: `LOOP_COPY`), "Copied 1-1>2-3". Copy let go ends it.
- **Delete held + a loop:** emptied ("Deleted 2-3"); a recording or armed slot: cancelled. **Delete held + a scene's
  step:** the column emptied ("Deleted Scene 3").
- **Undo:** one step: the last recording (the slot as it was), a delete, a copy's target, a scene cleared ("Undo",
  what). Undo is lit while there is one; with none, Undo is the sequencer's. Settings turns are not undone.
- **Mute held + a loop:** muted / unmuted (grey).

## Mixing (lp_bus a): one path

Every loop's voice: its level, pan and mute, its own sends 1..3 (after its level and pan), then its lane's level,
then the **Loops strip** (level, pan, sends 1..3, the volume knob's master: the firmwares' and the kit's audio taper),
into the mix before the FX returns, beside the kit's bus (`looper_render_bus`). The strip is the Mixer's fifth (knob
5; its screen's fifth strip "Loop"; pad column 5 stays the Mixer's choice pads) and `MIX` slot 4. The looper's own
block is the recorder's Looper stream (the sampler's Source Looper, now offered) and the Loops source. Nothing plays
any other way: no preview, no audition. Each voice fades in 3 ms on a launch (a recorded loop's own start: none) and
out 10 ms on a stop; a loop's seam (its cycle's ends) is faded 2 ms each side.

## Tempo (lp_tempo a)

- **Warp Repitch** (the default): the loop's place is the song's beat, so a tempo change repitches it; Pitch adds on
  top: the loop plays its part faster or slower within each cycle, still on the bar (pitched up, the rest of the cycle
  is silent; pitched down, it is cut at the cycle's end).
- **Warp Off:** it plays at its own rate (Pitch on top) from its launch, round its part, not on the song's position.

## The sampler's Loops tab (lp_tab a)

Offered by the host (`rfm_move_smp_offer` tabs bit 3; `RFM_MOVE_SMP_TABS_BUILT` 0xF with the looper). It takes the
screen, the jog, Back and knobs 1..4 only, so the pads stay the firmware's (boards O, P): the tabs line, the grid in
small (a loop an outline, playing solid with a mark, waiting or armed dashed, recording solid with a black dot, empty a
dot; the jog's slot underlined), one inverted row: an empty slot the lane's source and length; armed "Armed";
recording "Bar 3"; a loop its bars with Play or Stop. The jog: a slot a detent (up past the first: the tabs); its
click: records into an empty slot, ends a recording, plays a loop, stops a playing one (at the bar); Sample: records
into the jog's slot (full or not) or ends the recording; knobs 1..4: the lanes' levels (the level popup); 5..8 dark
and dead.

## Saving (lp_files a)

- The project is **version 19** (round 2: **20**, below): the looper's part after the kit's (`RFM_MOVE_LOOP_BYTES`, 3904: the chosen slot, the
  page, the strip; each lane's source, length, level; each slot's path, bars, recorded tempo and 11 settings).
  Versions 18 and older load with an empty looper. `projmem.c` reads the firmware ids before both parts.
- A project load sends every slot's `LOOP_LOAD` (its path, bars, tempo) and `LOOP_SET`s, every lane's and the strip;
  the host reads the WAVs on the file thread (a slot shows its place until then; a missing file: "Missing").
- The project's folder (`Kits/Projects/NN`) is deleted, copied and moved with its `Loops/` sub-folder (storage.c; a
  project's Copy + paste, cut, delete and the page's duplicate). On a save of the open project the file thread deletes
  the `Loops/` files no slot or Undo refers to (`rfm_move_loop_refs`), never a file under 30 s old.

## The host's part (`src/looper.c`)

Every call on the engine's thread but the file work. The engine's thread never allocates (tests/looper_test.c counts
every allocation by thread): recordings go into the spares, loaded and copied sounds come from the file thread, a
sound let go by every slot is freed there. The engine's thread takes the looper's mutex only to post a request or take
an answer, as the kit does.

| Core action | The host |
|---|---|
| `LOOP_REC` (90) `R_START` (value bars, vel source, slot the Played slot), `R_END`, `R_CANCEL` | `looper_rec`, `_rec_end`, `_rec_cancel` |
| `LOOP_LAUNCH` (91) down 1 slot / down 0 lane, value the quantization | `looper_launch`, `looper_stop` |
| `LOOP_SET` (92) a slot's `P_*`, or index 32 + lane: `L_*` | `looper_set`, `looper_lane` |
| `LOOP_LOAD` (93) path (`@/Loops/..`), value bars, take bpm x 10; none: emptied | `looper_load` (shared from memory when a slot or the Undo keep has it) |
| `LOOP_COPY` (94) index target, take source | `looper_copy` (the same sound) |
| `MIX` slot 4 | `looper_mix` |

Answers: `rfm_move_loop_recorded` (the file written: its path, frames, bars, tempo, peaks; or Too Long / Nothing /
Not Saved), `rfm_move_loop_loaded`; `rfm_move_loop_status` about 30 a second (each lane's scene, what waits, its
place, its level; the recordings; the beat; the bar's peaks; the memory).

**The sample rate** is the engine's, 44.1 kHz (`FW_FS`; the kit's spec mentions 48 kHz voices, but the engine runs at
44.1 kHz and the looper records and plays at that rate, so a loop stays sample-exact against the bar).

## Tests

- rfm1cs `tests/move_looper.c` (ctest `move_looper`): the capability, Track 4's round and pad 4, the grid's lights,
  recording (tap, armed, cancel, the red pad, Sample, a held slot + Sample, back-to-back), launching (Bar, the double
  click's Instant, a stop, the Launch setting), scenes three ways and step 16 (the transport started), choosing
  silently, the pages and their knobs, the Loops tab, Copy / Delete / Undo / Mute, the project (19, 18, a bad part),
  the Mixer's fifth strip.
- `tests/looper_test.sh` (natively: plain, ASan + UBSan, TSan): a recording block-exact at the bar and its WAV frame
  for frame, the loop at once from the bar it ended on, a launch on the bar line, Instant in phase, a stop at the bar,
  the tempo doubled (twice as fast, still on the bar), the mix path (strip, lane, loop level, master, mute, pan, a
  loop's and the strip's sends), an emptied slot brought back from memory, Warp Off, Open and back-to-back
  recordings, a cancel, the transport stopped during one and armed until Play, the memory's cap, the clean-up, no
  allocation on the engine's thread.
- `tests/looper_e2e.sh` (the simulator): the view and the round, a recording of Felucca at the bar, its WAV, the loop
  heard through the mix, step 16 and step 1 at the bar and now, the Loops strip on Track 4 + volume, the Loops tab
  offered, the project duplicated with its `Loops/`, a second start loading the loop. `make test-looper`; `make test`
  runs both.

## Stage 2 (not built)

(Round 2 built overdub, as the step looper's key 2, and Grains / Beats.) Capture (the last N bars of a
lane's source from the recorder's ring), To Kit / To Takes / a take's To Looper (`RFM_MOVE_SMP_TO_LOOPER`), launch
modes (Trigger, Gate, Toggle: the Loop page's knob 6), the page's web looper; then monitor + click.

## Round 2 (the owner's first try of stage 1, 2026-11-26)

The owner's words are quoted; where the call was ours it says so. Code: rfm1cs `src/looper.c` (and `kit.c`'s Files,
`seq.c`'s count-in, `seq_ui.c`'s Play and Shift + step 13, `screen.c`'s overlay), this repo's `src/looper.c`,
`src/stretch.h`, `src/kit.c`, `src/engine.c`. Tests: rfm1cs `tests/move_looper.c` (`t_round2`, `t_steploop`, the
markers and Warp in `t_pages`, version 20 in `t_project`); `tests/looper_test.c` (`t_round2`); `looper_e2e` records
from the Record page.

### 1. Loading a take or a file ("load on takes from the sampler or load in a sample from files")

- **The jog's click** (any page) opens **the kit's own Files** for the chosen slot: the same state, rows, screen and
  host code (rfm1cs `kit.c` `rfmv_kit_lbr_*`, `files_nav` shared with the kit's click), its root **Takes** (when the
  host has the take library), **My Samples**, **the Move Library**. No landing: nothing loads until a sound's row is
  clicked. While it is open a pad picks the slot it goes to (nothing records or plays); Back goes a folder up, the
  places, then closes it.
- A sound's click: `LOOP_FILE` (95: index the slot, value the place, take the row's id). The host reads it on the
  looper's file thread through `looper_cfg_t.read` (the engine's: `kit_row_read`, any format the kit reads, resampled
  to 44.1 kHz), writes its copy into the project's `Loops/` as a 16-bit WAV ("Name.wav", " 2" .. when taken; the
  original never changed), and answers as a recording (`rfm_move_loop_recorded`): **its bars its length at the tempo
  now, rounded to whole bars** (1..64), Steps that many bars, the markers the whole sound. The slot's sound plays on
  until then; Undo brings back what the slot had ("Loaded 2-3"; refused: "Not Loaded").

### 2. The page decides a tap ("when in the record tab, then that's when it does the current click a pad and it immediately starts recording")

- **Record page:** a tap on an empty slot records (as stage 1).
- **The other pages** (Loops, Loop, Loop Mix, Looper Mix): it picks the slot; loops' taps launch and stop as before.
- Gone: "an empty slot chosen on Loop / Loop Mix opens Record". Loop held + a pad and the arrows stay silent picks.
- The sampler's Loops tab keeps its click (records into an empty slot): it has no pages.

### 3. One input setting ("its input selection is separate from the sampler's line in")

- The looper's Input source is the input **with the sampler's In Level** (engine `ain_gained`, the same buffer the
  sampler records); there is no looper input setting. The Record page's knob 4 is the sampler's **In Level** and knob
  5 its **Monitor** (`rfm_move_smp_set`: the same value in both places, the settings saved, `SMP_SET` to the host).

### 4. Count-in ("the countin should delay the playback when you press play")

- **Count-in** (the Record page's knob 3: Off, 1 Bar, 2 Bars; our call: **global** to the looper, saved in the
  project's looper part, byte 8). A looper recording armed while stopped, Play: the sequencer's own count-in
  (`rfmv_seq_count_in_bars`, the same mechanism as Rec armed + Play; the sampler's Bars arm waits for the bar on the
  host's recorder, which is not a count-in, so it is not shared), 1 or 2 bars of ticks; the metronome's clicks if it
  is on (each bar's first accented), the screen's count popup either way, the step LEDs red, one a beat to go (the
  Looper view). Then the transport starts and the recording begins on bar 1: **the sequencer's playback waits the
  count-in too**, so everything starts together. (`seq.c`: `cin_go` marks the count-in's first tick, so a 2-bar one
  runs at the tempo; the clock code is otherwise untouched.) `rfm_move_loop_count_in` says the bars.

### 6. Monitor ("an option to monitor Line In")

- The sampler's **Monitor** setting, now offered: **Off** (the default: feedback), **On**, **Auto** (while an input
  recording is armed or runs: the looper's, an overdub of the input, or the sampler's with Source Input). Reached from
  the looper's Record page (knob 5) and the sampler's Record tab.
- The input at its In Level goes **through the Loops strip** (our call, so it has a level, pan, sends and the volume
  knob: `looper_monitor`; the strip's level is its level), into the mix before the FX returns, never into the
  looper's own block (the Loops source, the sampler's Looper stream). Latency: a block plus the input ring's lead.

### 7. Warp engines ("we should add different warp stretch engines")

- A loop's **Warp**: **Off** (its own rate round its region), **Repitch** (the region resampled to fill its steps: a
  tempo change repitches), **Grains**, **Beats** (the kit's time-stretch: the region fills its steps, the pitch kept,
  Pitch on top). The knob's order is Off, Repitch, Grains, Beats (the values: `RFM_MOVE_LOOP_W_*`, new ones after).
- The kit's code is **shared, not copied**: `src/stretch.h` (header-only, no allocation) holds the two-grain engine's
  window, frame reading, restart (Grains' jitter, Beats' WSOLA alignment) and start, factored out of `kit.c` (which
  uses it unchanged: `kit_test` passes as before). The looper's grains: 60 ms, Grains' jitter 20 % of half a grain,
  Beats' search 10 ms; its playhead is the song's place in the region, so it stays on the bar.
- **Cost:** four lanes in Beats, about 13 us a 128-frame block on the Mac (0.4 % of the block; `looper_test` prints
  it). No "Texture" / "Tones" engine: nothing cheap and verified to fit the Move's CPU yet (our call: not added).

### 8. Start, End and Steps ("here's the start and end of this piece of audio, I would like it to play over this many steps")

- The Loop page: **Start** and **End** markers (`RFM_MOVE_LOOP_P_MSTART` / `_MEND`, 0..`RFM_MOVE_LOOP_MK_ONE` = 2^24 of
  the sound: sample-accurate to 16 M frames; `LOOP_SET` carries them in `take`). A detent moves a column of the
  close-up (the sound / 8 / 64); **Shift: one frame** (the close-up 256 times nearer). The close-up as the kit's trim:
  the value (seconds), the frames round the marker from the host's peaks (`LOOP_PEAKS` 96 -> `rfm_move_loop_peaks`,
  on the file thread), the marker a line with a notch, outside dithered.
- **Steps** (Length, 1..1024: a step a detent to 16, a beat to 64, then a bar; Shift: a step): the region between
  the markers fills exactly that many steps at the tempo now with the loop's Warp, locked to the bar. This replaces
  "Length = bars of those recorded"; Start (steps) is unused. Version 19's Start / Length load as the markers of that
  part, played over Length's steps (the same sound).
- The same for loaded files.

### The project, version 20

The looper part: header byte 8 the Count-in, byte 9 the step looper on; each slot 129 bytes (version 19's 121, then
the markers u32); an empty slot all zeros (checked). `RFM_MOVE_LOOP_BYTES` 4160 (`_V19` 3904); versions 19 and 18
load. This repo's `projmem.c` knows 20.

### 9. The step looper ("a looper mode takeover feature you could enable that takes over the step keys")

- **On / off:** **Shift + step 13** (step 13 had no shortcut; its icon lit while on), in the **FM-1 layout Track 4
  held + step 13** (there Shift is GLO; Track 4 held + a step is the Shift layer in every layout), or the Record
  page's knob 6. A full-screen card for 1.5 s: "Step Looper On", four boxes (lanes 1..4, keys 1 Rec, 2 Dub, 3 Rpl, 4
  Nxt); off: "Steps: sequencer". Saved with the project (byte 9).
- **Where:** every mode and layout (the FM-1 layout, note layouts, Sequencer, the Kit view, session view) but the
  Looper view (its steps are the scenes) and open menus. It takes the steps without Shift or a Track held; Shift +
  a step (Track 4 + a step) stays the shortcuts. **Step sequencing is off while it is on.** The pads stay the
  firmware's.
- **The FM-1 layout** uses the steps for the FM-1's 16 white keys (its other controls: the pads the FM-1's keys and
  buttons, Shift GLO, the knobs SELECT / PRESETS / ALGO / K1..K4, the named buttons the FM-1's). While the step
  looper is on the step keys wait (our call: the least lost: the pads' FM-1 keys still play every key); nothing else
  moves. Lane levels: a group key held + the volume knob, as in every layout.
- **The groups:** steps 1-4, 5-8, 9-12, 13-16 are lanes 1..4, each in its **Okabe-Ito** colour: blue {0, 114, 178},
  orange {230, 159, 0}, bluish green {0, 158, 115}, reddish purple {204, 121, 167} (neighbours far apart for
  colour-blind players; the steps are RGB LEDs, `LED_RGB`, so these go out as they are, not through a palette index).
- **The keys** (a group's):
  - **1 Rec, the pedal.** Empty or stopped: records at the bar into the lane's slot (its first empty one, or the one
    it recorded or played last: a stopped lane's tap records over it, **Undo brings it back**: "reuse that slot" is
    one tap); stopped, Play with the Count-in. Armed: cancelled. Recording: it ends at the bar and loops at once.
    Playing: an overdub ends; else nothing.
  - **2 Dub: overdub** on / off (`LOOP_DUB` 97). The lane's Source layered onto the playing loop from its next
    cycle: the first pass replaces a preallocated spare's silence, later passes sum (saturated), each frame once.
    Heard from the next pass. Off: the file thread writes the loop + its layer as a new WAV; the loop's sound swapped
    in seamlessly; its settings kept; **one Undo** (the loop before it, from memory). One overdub at a time (another
    commits it); not while reversed.
  - **3 Replace:** records over the playing loop from the next bar (Undo: the loop it replaced).
  - **4 Next:** the lane's next empty slot is the pedal's (the old loop stops when the new recording starts);
    **a double tap: Stop** (the lane stops at the launch time; the pedal's slot back to what it was). Our call: Stop
    on the double tap, not Shift + key 4 (Shift + a step stays the shortcuts).
  - **A group key held + the volume knob:** the lane's level (the level popup).
- **The lights** (at the step LEDs' own rate; only changes go out): playing: a key a quarter of the loop, the one now
  in the lane's colour, the others dim; overdub on: that key blinks fast (white); waiting to start or stop: blinking;
  recording: the bars done red, the bar now blinking red; armed: the group blinking, key 1 red; a loop stopped: dim;
  empty: very dim; a held key: white. Shift or a Track held: the shortcuts' lights.
- **The screen:** a change's overlay (rows 0..10, 1.2 s): "Lane 2 REC", "Lane 1 Dub on", "Lane 3 > 3-2", "Lane 4
  Stop"; while a group key is held its keys' jobs ("L2 Rec Dub Replace Next"); with the volume knob the lane's level.
  No text on the LEDs.
- Loops record from each lane's Source, default **Played** (in the FM-1 layout: the firmware played).
