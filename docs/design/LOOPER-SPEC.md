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

- **Tap an empty slot:** it records from the next bar line. Stopped: armed, it starts on Play's first frame. Armed,
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
Any pad's tap also chooses its slot. An empty slot chosen while the Loop or Loop Mix page shows opens the Record
page.

## Pages (lp_pages a): the jog, as the kit's

The jog turns five pages (clamped at the ends); while it turns, the switcher: the pages' 2x icons in 28 px cells
sliding, the shown one a white cell, its name under it. A knob touched: its popup (name and value) while touched and
a second after; turned: its value changes; Shift: fine. A page's knobs are lit teal; a dead knob is dark.

| Page | Picture | Knobs 1..8 |
|---|---|---|
| Loops | a lane a row: the scene it plays (2x), its bars as ticks, filled as it plays; waiting: the scene (or "-" a stop) and a dashed box with the beats; stopped: a dotted line; recording: the dot and its wave growing | lanes 1..4 Level |
| Loop | the chosen loop's wave over its recording, the playhead (past it dotted), its bars, the slot's place top right (the grid in small) | 1 Start (steps), 2 Length (a beat, half a bar, whole bars up to those recorded), 3 Pitch (semitones; Shift: cents), 4 Reverse, 5 Warp (Repitch / Off); 6..8 dark (6: Launch mode, stage 2) |
| Record | the record icon, the place, the lane's Source and Length rows | 1 Source, 2 Length (the chosen slot's lane) |
| Loop Mix | the wave, the level as a fader (muted: hollow), the pan, sends 1..3 | 1 Level, 2 Pan, 3..5 Send 1..3, 6 Mute |
| Looper Mix | the lanes' levels (a cap: it sounds; the chosen loop's lane underlined), the Loops strip's fader, pan, sends | 1..4 the lanes' levels, 5 the Loops strip's level, 6 its pan, 7, 8 its sends 1, 2 |

An empty slot's Loop and Loop Mix pages show the Record page; its loop knobs are dark. Every level is 0..127 with 100
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

- The project is **version 19**: the looper's part after the kit's (`RFM_MOVE_LOOP_BYTES`, 3904: the chosen slot, the
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

Overdub (Record held + a playing loop), Grains / Beats stretch (Elasticat-style later), Capture (the last N bars of a
lane's source from the recorder's ring), To Kit / To Takes / a take's To Looper (`RFM_MOVE_SMP_TO_LOOPER`), launch
modes (Trigger, Gate, Toggle: the Loop page's knob 6), the page's web looper; then monitor + click.
