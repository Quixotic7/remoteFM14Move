# The looper: design round 1 (proposal, nothing built)

2026-11-26. Settled before this round (the owner's words in quotes): a session-view-like grid, 8 x 4; repitch on
tempo change; overdub in a second stage; saved with the project; one Loops strip; "Make loops available as a tab, but
also make it so the loops tab does not need the pads"; Track 4's round Mixer, Routing, Kit, Looper (Track 4 held +
pad 4 = Looper); resample the looper; Elasticat-style stretch later. Everything else below is a proposal, each choice
a card in `cards.json` (ids `lp_*`). Screens: `mocks.md`, `png/`, `looper-v1.movedesign.json`.

The looper must feel like the kit: the jog turns pages, a page is a picture, a knob says its name on touch and its
value on turn, levels in dB (0.5 dB a detent), every sound through one mix path, nothing plays by itself.

## 1. What a loop is

A loop is a clip of stereo audio in one slot of the 8 x 4 grid. It knows its length in **bars** and the **tempo it
was recorded at**. It is always tempo-synced: its playhead is derived from the song position (the sequencer's beat),
not free-running, so it can never drift. The audio is held in RAM (16-bit stereo, like the kit's sounds) and kept as a
WAV in the project's own folder.

## 2. The grid (card lp_grid)

- **Rows are lanes, columns are scenes** (Move's own Session Mode: "each row of pads represents a track's clip slots
  and each column represents a scene"). Lane 1 is the top row, beside Track 1, as on Move. 4 lanes x 8 scenes = 32
  slots. The kit's convention, "a pad row a track", is the same idea.
- A lane plays **one loop at a time** (launching another in the row replaces it at the bar): this is the "choke per
  row" (card lp_lane_choke).
- Alternative on the card: RemoteFM1's own session view (columns 1-7 tracks, column 8 scenes, 28 slots).
- **Lights**, as session view: empty dark; a loop dim teal (the looper's colour, card lp_colour); playing green;
  lined up blinking green; about to stop blinking teal; recording red; armed blinking red; the chosen slot white.

## 3. Recording (cards lp_rec_gesture, lp_source, lp_length, lp_rec_start)

- **Tap an empty slot: it records**, starting at the next bar (launch quantization). Stopped transport: it is armed
  and starts with Play (the sampler's Bars behaviour the owner liked: "we arm the recorded, then as soon as you hit
  play it starts recording"). Recording a slot stops its lane's playing loop, as in Live.
- **Length per lane:** Open (default), 1, 2, 4, 8, 16 bars. Open: tap the red pad (or press Sample) to end it at the
  next bar; Fixed: it ends by itself. Either way it **plays at once** from the bar it ended on, seamlessly.
- **Source per lane:** Played (the firmware the Track buttons last chose, default), Track 1, 2, 3 (a slot dry, as the
  recorder's slot streams), Kit (the kit bus alone), Input, Mix (slots + kit at their levels, before the FX returns,
  no loops), Loops (the looper's own output: resampling lanes into one).
- A slot held + Sample records into it even when full (replace), the kit's gesture.
- Recording uses no `rec_t` take, so the sampler can record meanwhile; it copies the source block into a preallocated
  buffer on the engine thread and writes the WAV on the file thread when it ends.

## 4. Launching and stopping (cards lp_launch, lp_scene, lp_stop)

- **A loop's pad:** tap = plays at the next bar; double click = now (UX-RULES R2's "same thing sooner" exception: the
  tap acts at once, so no 300 ms delay). A playing loop's pad: tap = stops at the bar, double = now.
- **Scenes:** under each column, its left step key plays that column in every lane at the next bar (a lane with no loop
  there stops). Step 16 stops all at the bar (its icon lit: a function). Shift + a pad: choose only (silent).
- **Quantization:** the global Launch setting session view already uses (card lp_quant).
- A scene pressed while stopped starts the transport, as session view's scene pads do.

## 5. Where it lives (cards lp_view, lp_pages, lp_tab)

- **Track 4's round:** Mixer, Routing, Kit, **Looper**; Track 4 held + pad 4. The pads are the grid; the step keys the
  scenes; Copy / Delete / Undo as session view (Copy + loop, then a slot; Delete + loop).
- **Pages (the jog, as the kit's; switcher with 2x icons):**
  1. **Loops**: a lane a row: the scene it plays, its bars filling as it plays; queued dashed; recording the wave.
     Knobs 1-4: lanes 1-4 level.
  2. **Loop**: the chosen loop's wave over its bars, the playhead. 1 Start (steps), 2 Length (bars of those recorded),
     3 Pitch (repitch, semitones; Shift cents), 4 Reverse, 5 Warp (Repitch / Off), 6 (stage 2) Launch mode.
  3. **Record**: the chosen slot's lane: 1 Source, 2 Length. (An empty slot chosen opens here.)
  4. **Loop Mix**: as the kit's Pad Mix: 1 Level, 2 Pan, 3-5 Sends 1-3, 6 Mute.
  5. **Looper Mix**: 1-4 lane levels, 5 Loops strip level, 6 pan, 7-8 sends (the strip is also the Mixer's column 5).
- A slot held + the volume knob: that loop's level, as a held kit pad.
- **The Loops tab** in the sampler app (Sample; the core already reserves `RFM_MOVE_SMP_TAB_LOOPS`): takes the screen,
  jog, Back and knobs 1-4 only, so **the pads stay the firmware's**: you play a firmware and loop it with the jog. The
  grid drawn small, the jog's slot underlined, one row under it. Turn: the slot; click: record / play / stop it at the
  bar; Sample: record into the jog's slot or end the recording; knobs 1-4 the lanes' levels.

## 6. Tempo (card lp_tempo)

Stage 1: **Repitch** (settled): rate = tempo now / tempo recorded, so the pitch moves with the tempo; Pitch adds on
top. Warp Off: plays at the recorded rate, not synced (for one-shots). Stage 2: Grains and Beats from the kit's Warp
(keeps the pitch); Elasticat-style later.

## 7. Mixing (card lp_bus)

The looper is a bus beside the kit's: its loops' voices (level, pan, own sends) summed, then the **Loops strip**
(level, pan, sends, the volume knob's master, as `kit_master`), into `g_mix.acc` before the FX returns, its own block
as the recorder's `REC_ST_LOOPER` stream. Nothing plays any other way: no preview, no audition.

## 8. Saving (card lp_files)

- The project gains a looper part (a new project version, as the kit's 18): the lanes' settings, the 32 slots' bars,
  recorded tempo, settings and the file's `@/` path.
- Files: `Samples/RemoteFM1/Kits/Projects/NN/Loops/<lane>-<scene> <bpm>bpm <bars>b.wav`, written when the recording
  ends (not at save), so they are safe before the save and Move's own browser sees them. The project folder moves
  with the project as the kit's does; the clean-up deletes loops no slot or undo refers to.
- Loading a project reads the loops on the file thread (a slot shows loading until ready).

## 9. Resampling (card lp_resample)

Already possible: the sampler's Source Looper (`RFM_MOVE_SMP_SRC_LOOPER`) and Master take the looper. New: a lane's
Source **Loops** records the other lanes into one loop (bounce, freeing lanes). Stage 2: a loop **To Kit** (the next
empty pad) and **To Takes**; a take **To Looper** (`RFM_MOVE_SMP_TO_LOOPER`, reserved).

## 10. Undo (card lp_undo)

One step, as the kit: the last recording (the slot empty again), delete (the loop back), copy (the target as it was),
or a scene/lane clear. Settings turns are not undone. Overdub (stage 2) keeps the loop before the layer as its undo.

## 11. Stages (card lp_stages)

- **Stage 1:** the grid, record (Open / fixed, sources), launch / stop / scenes quantized, the five pages (Start,
  Length, Pitch, Reverse, Warp Repitch/Off, mix, sends, mute), repitch on tempo, the Loops strip, saving and the
  project's copies, the Loops tab, Copy / Delete / Undo.
- **Stage 2:** overdub (Record held + a playing loop: layers from the loop's next start until tapped again),
  Grains / Beats stretch, Capture (the last N bars of a lane's source into an empty slot, from the recorder's
  always-listening ring), To Kit / To Takes / To Looper, launch modes (Trigger, Gate, Toggle), the page's web looper.
- Then (the settled order) monitor + click.

## 12. What it reuses

| Piece | Reuses |
|---|---|
| Bar-exact start / end in a block, beat at the block's first frame | `src/recorder.c`'s bar-cut math (split out as a helper; the looper keeps its own buffer, no `rec_t` take) |
| The sources' blocks | the engine's existing recorder taps (`rec_feed`: master, slots dry, input, kit's `g_kit_blk`) plus one new tap, the mix before returns |
| Playback voice, interpolation, sends | `src/kit.c` voice code (linear interpolation, own sends, `kit_master`); stage 2 its Warp Grains / Beats |
| The bus and strip | `kit_render_bus`'s pattern: a `looper_render_bus` beside it in `engine.c`, `REC_ST_LOOPER` as its own block |
| Files, thread, clean-up, project folder | the kit's file thread, `wav.c`, `kit_proj_dir`, round C's clean-up and storage moves |
| WAV peaks for the wave | `takes.c` / the kit's KIT_PEAKS |
| UI | core `kit.c` page switcher, popups, `ahdsr`-style pictures; `sampler.c` tabs and rows; session view's lights, Copy / Delete, Launch setting, `rfm_move_seq_pos` |

## 13. Hard spots found in the code

- **"Mix without loops" is not the master.** The FX returns are shared (`g_bus` sends from slots, kit and loops), so
  the master minus the looper cannot be computed. The proposal's Mix source is taken before the returns (dry of FX);
  a loop recorded from it gets FX again through its own sends. Recording the master while loops play includes them.
- **One recorder.** `rec_t` runs one take at a time; looper recordings must not go through it, or the sampler and the
  looper block each other. Its bar-cut logic needs extracting into something both can call.
- **Memory.** 32 loops x 16 bars at 90 bpm is about 240 MB in RAM (16-bit stereo, 44.1 kHz); the owner's Move has
  1.4 GB free. Proposal: a 256 MB looper cap ("Too Long" past it), recording into a preallocated spare buffer so the
  engine thread never allocates.
- **Row order.** Move's Session Mode puts lane 1 on the top row (beside Track 1), while the kit puts pad 1 bottom left.
  RemoteFM1's session view uses columns for tracks. Three conventions: card lp_grid decides.
- **Steps.** Session view uses odd / even steps under each column for tracks; with columns as scenes here, the odd
  step plays the scene and the even ones are dark (step 16 stop all), a small departure.
- **Project folder recursion.** The kit's storage moves and clean-up were written for files right in `Projects/NN/`;
  a `Loops/` subfolder must be checked to move with cut / copy / paste (clean-up already skips subfolders).
- **Sample rate.** The recorder defaults to 44.1 kHz and the kit notes 48 kHz voices; the looper should record and
  play at the engine's rate to stay sample-exact against the bar.
