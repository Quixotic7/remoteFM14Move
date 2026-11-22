#!/usr/bin/env python3
"""RemoteFM14Move: session view, proposal 2 (the owner's review of 2026-11-21, 14 answers): a scene column (7 tracks +
scenes), Live's colours (green: playing), an empty slot selected and not filled, empty clips marked, Shift + a clip
selects, the steps select / stop (step 16: every clip), a knob touched: its track. An Ableton Move Designer document
(format 'ableton-move-designer' v3).

The screens and the LEDs are the mock-up's (mock_session_v2.c, written before the core's code): it writes
sessview2_<name>.pbm and .leds into its working directory.
    python3 make_session_v2.py <the mock's directory> <out.movedesign.json>"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(20261121)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
FWC = ["#c43aff", "#c8ff2a", "#1ae4ff"]                  # the slots' firmwares (ChoralRoot, Felucca, OberWave)


def screen(name):
    """the core's screen (PBM P1) as the designer has it: 1024 bytes, row-major, the MSB left"""
    tok = open(os.path.join(SHOTS, f"sessview2_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first) and the 16 steps"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"sessview2_{name}.leds")).read().strip().split("\n")]
    col = lambda h: None if h == "000000" else "#" + h
    return [[col(h) for h in r] for r in rows[:4]], [col(h) for h in rows[4]]


def m(**k):
    return {"meta": k}


def controls(name, held=None, touched=None):
    pads, steps = leds(name)
    c = {}
    for i in range(3):
        c[f"track-{i + 1}"] = {**m(label=f"FW {i + 1}", function="A firmware's slot. A tap: its layout (a note layout: the "
                                   "sequencer's track view; session view is left)."),
                               "led": {"color": FWC[i] if i == 1 else None}}
    c["track-4"] = {**m(label="SEQ", function="Mixer / Routing; held: the Shift shortcuts; double: the projects"), "led": {"color": "#401010"}}
    c["menu"] = m(label="SESSION", function="A note layout: track view <-> session view", click="Track view <-> session view")
    c["back"] = m(label="BACK", function="Closes the latest layer: the clipboard's mark (a waiting copy, a cut), then session view")
    c["shift"] = {**m(label="SHIFT", function="NEW: Shift + a clip or an empty slot: selected for editing, nothing launched or "
                                            "stopped. Shift + Play: every clip from its start (as in track view; it was: "
                                            "every clip stops)"),
                  "led": {"brightness": 1.0 if held == "shift" else 0.25}}
    c["play"] = m(label="", function="Start / stop the sequencer (the only thing that does)")
    c["delete"] = m(label="DELETE", function="Held + a clip: cut; the press right after a cut: pasted (the steps' rules, 0.9.2)")
    c["copy"] = m(label="COPY", function="Held + a clip: copied (dashed). Held + an empty slot: pasted. Let go after copying, "
                                         "then a press: pasted there once (as steps). NEW: a tap alone does nothing here")
    c["mute"] = m(label="", function="Mute + a clip, or Mute + a step (the 16 tracks): the track muted / unmuted")
    c["undo"] = m(label="", function="Undo (Shift: redo)")
    for b, fn in {"left": "The tracks in view, one left", "right": "The tracks in view, one right (7 of 16 show)",
                  "up": "The scenes in view, one up", "down": "The scenes in view, one down (4 of 16 show)",
                  "capture": "As in track view", "sample": "As in track view", "loop": "Nothing here", "record": "Record: an "
                  "empty slot pressed with it armed records into a new clip there"}.items():
        c[b] = m(label="", function=fn)
    c["volume"] = m(label="MASTER", function="The master volume")
    c["wheel"] = m(label="", function="The knob page, as in track view")
    for k in range(1, 9):
        c[f"knob-{k}"] = m(label="", function="NEW: touched: the track of this column selected, and its popup while touched: "
                           "the track, the firmware it plays, its selected clip and length, its channel" if k < 8
                           else "Over the scenes: nothing")
        c[f"knobled-{k}"] = {"led": {"color": "#ffffff" if touched == k else None}}
    for r in range(4):
        for k in range(8):
            fn = ("NEW: a scene. A press launches the whole row at the next bar (stopped: at once, playing with Play); tracks "
                  "with no clip in the row stop. Green: dim = no clip in the row, medium = clips, pulsing = the scene "
                  "playing, blinking = on its way." if k == 7 else
                  "A clip slot: column = track, row = scene. A clip pressed: launched at the next bar and selected. NEW: an "
                  "empty slot pressed: selected (white), no clip made, the track stops at the bar. GREEN pulsing: playing "
                  "(the selected one flashes white on the beat). Green steady: stopped, plays with Play. Green blinking: "
                  "waiting for the bar. The track's colour, dim: a stopped clip (very dim: an empty clip). White: selected.")
            c[f"pad-{r + 1}-{k + 1}"] = {**m(label="", function=fn), "led": {"color": pads[r][k]}}
    for i in range(16):
        fn = ("NEW: select this column's track (its colour; the selected one full; muted: red)" if i % 2 == 0 and i < 14 else
              "Stop this column's track at the bar: white while it plays" if i < 14 else
              "Nothing" if i == 14 else "NEW: stop every clip at the bar (the transport runs on)")
        c[f"step-{i + 1}"] = {**m(label="", function=fn), "led": {"color": steps[i]}}
        c[f"stepicon-{i + 1}"] = {"led": {"brightness": 0}}
    return c


def layout(x, y, title, desc, name, smeta, held=None, touched=None):
    scr, _ = screen(name)
    ctl = controls(name, held, touched)
    ctl["screen"] = {"meta": smeta}
    return {"id": uid(), "x": x, "y": y, "w": 1048, "h": 616, "type": "layout", "name": title, "description": desc,
            "scope": ALL, "controls": ctl, "screen": scr}


def main():
    DX, DY = 1148, 920
    pos = lambda k: (60 + (k % 4) * DX, 60 + (k // 4) * DY)
    S = lambda fn: {"label": "Session", "function": fn}
    L = [
        ("A. Stopped", "7 tracks and a scene column. Row 1 is what Play starts: its clips green on the pads, an outline with "
         "a play icon on the screen. Track 2's clip 1 is the selected one (white, framed). Track 4's clip 2 is an empty clip.",
         "rest", S("7 track columns; the 8th: the scenes' numbers (the one Play starts underlined). A clip: an outline. NEW: a "
                   "clip Play starts: the play icon in it. NEW: an empty clip: its corners only. Empty slot: a dot."), None, None,
         "Review items 1, 3, 4. Stopped clips are their track's colour, dim; green means 'this plays'. Only Play starts the "
         "transport (item 6: kept)."),
        ("B. Playing", "The same, playing: row 1's clips pulse green (6 steps a beat), filled on the screen; scene 1 pulses "
         "green. The selected clip (track 2) flashes white on each beat.",
         "playing", S("Playing: filled, a black play icon. The selected one stands clear of its frame."), None, None,
         "Item 4 (Live's colours). The white flash keeps the selected clip visible on the pads while it plays."),
        ("C. A clip pressed", "Track 1's clip 2 pressed: selected at once (framed), blinking green until the bar, then it "
         "plays and clip 1 falls back to the track's colour. Scene 1 is no longer what plays: no underline.",
         "queued", S("Waiting for the bar: a doubled outline, blinking."), None, None,
         "Unchanged from 0.9.2, in the new colours."),
        ("D. Shift + a clip: selected only", "Shift + track 3's clip 3: it is the selected clip (white pad, frame; Menu or the "
         "Track button shows its steps) and track 3's clip 1 plays on.",
         "shiftsel", S("The frame on the Shift-pressed clip; nothing else changes."), "shift", None,
         "Item 5. Shift + an empty slot selects it too, without stopping the track. (Scenes moved off Shift + clip to the "
         "scene column.)"),
        ("E. An empty slot pressed", "Track 2's empty row 3 pressed while its clip 1 plays: the slot is selected (white, a "
         "frame round the dot) and NO clip is made; clip 1 blinks in the track's colour and stops at the bar, as Live's stop "
         "button. The clip appears with the first note (steps, pads with Rec, Capture).",
         "empty", S("The selected empty slot: a frame round its dot. The stopping clip blinks."), None, None,
         "Item 2. Empty clips left in a project from before are removed when it loads. With Rec armed an empty slot still "
         "records a new clip (item: kept)."),
        ("F. A scene pressed", "The scene pad of row 2: its clips wait for the bar (green, blinking), tracks with no clip in "
         "row 2 stop (track 3), the scene pad blinks. Stopped: the row is armed at once and plays with Play.",
         "scene", S("The scene's number blinks while it is on its way; underlined once it is what plays."), None, None,
         "Item 1 (column 8 launches scenes). Copy / Delete + a scene pad: nothing yet (item 13 was not answered)."),
        ("G. Step 16: stop every clip", "Step 16: every track's clip stops at the bar (blinking in its colour); the transport "
         "runs on. The even step under a column stops that track; the odd step selects it.",
         "stopall", S("Stopping clips blink between filled and outline."), None, None,
         "Item 7 (Control Live's steps). Mute is Mute + a clip or Mute + a step. Shift + Play now restarts the clips in both "
         "views."),
        ("H. A knob touched: its track", "Knob 3 touched: track 3 is selected and its popup shows while the knob is touched: "
         "the track, the firmware it plays, its selected clip and length, its MIDI channel.",
         "info", {"label": "Track popup", "function": "The whole screen, no border: 'Track 3' and the firmware at 2x (several "
                  "firmwares: their names at 1x; none: 'Not routed'), the selected clip and its length, the channel. A mute "
                  "icon top right when the track is muted."}, None, 3,
         "Your note on item 8 (the grid stays as the screen; a knob touched gives more about the track). The pads keep "
         "showing the grid meanwhile."),
    ]
    items = [{"id": uid(), "x": 0, "y": 0, "w": 4 * DX + 900, "h": 2 * DY + 160, "type": "group",
              "title": "Session view, proposal 2 (after the owner's review, 2026-11-21): scenes, Live's colours, select / stop steps",
              "color": "#2ad17a"}]
    for k, (title, desc, name, smeta, held, touched, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, held, touched))
        items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 200, "type": "note", "text": nt, "color": "#cfe3ff"})
    items.append({"id": uid(), "x": 60 + 4 * DX, "y": 60, "w": 780, "h": 1500, "type": "note", "color": "#ffe58a",
                  "text": "SESSION VIEW, PROPOSAL 2\n"
                          "From your review (14 answers). Unanswered items are unchanged.\n\n"
                          "PADS: columns 1-7 = tracks (Left / Right: one track), column 8 = scenes, rows = scenes "
                          "(Up / Down).\n"
                          "A CLIP: launched at the next bar and selected. Stopped: armed at once, plays with Play.\n"
                          "AN EMPTY SLOT: selected, not filled; the track stops at the bar. Rec armed: records there.\n"
                          "SHIFT + A SLOT: selected only.\n"
                          "A SCENE PAD: the row launched; tracks without a clip in it stop.\n\n"
                          "COLOURS: green pulsing = playing; green steady = plays with Play; green blinking = waiting "
                          "for the bar; the track's colour, dim = a stopped clip; very dim = an empty clip; blinking in "
                          "the track's colour = stopping; white = selected (playing: a white flash on the beat); red = "
                          "recording.\n\n"
                          "STEPS: odd = select the column's track, even = stop it, 16 = stop every clip. Mute + a step: "
                          "the 16 tracks' mutes (as everywhere).\n"
                          "KNOBS 1-7 touched: the track selected, its popup.\n"
                          "COPY: held + a clip copies, held + an empty slot pastes; a tap alone does nothing here.\n"
                          "SHIFT + PLAY: every clip from its start, in both views.\n\n"
                          "SCREEN: the grid stays. New marks: the play icon in a clip Play starts, the corners of an "
                          "empty clip, the scenes' numbers in column 8.\n\n"
                          "TO DECIDE\n"
                          "1. Copy tapped alone still duplicates the clip in track view (the Move's own Note Mode does "
                          "this). Nothing there too?\n"
                          "2. The selected playing clip: a white flash on the beat. Or all white, as the Move?\n"
                          "3. The popup's rows: anything else you want about the track?"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "Session view, proposal 2", "items": items},
              open(OUT, "w"), indent=1)
    try:                                                # a PNG of the six screens for a quick look
        from PIL import Image
        sh = Image.new("RGB", (4 * 522, 2 * 266), (50, 50, 50))
        for i, (_, _, name, *_r) in enumerate(L):
            bits = screen(name)[1]
            im = Image.new("RGB", (128, 64), (0, 0, 0))
            im.putdata([(255, 255, 255) if b else (0, 0, 0) for b in bits])
            sh.paste(im.resize((512, 256), Image.NEAREST), ((i % 4) * 522 + 5, (i // 4) * 266 + 5))
        sh.save(OUT.replace(".movedesign.json", "-screens.png"))
    except ImportError:
        pass


main()
