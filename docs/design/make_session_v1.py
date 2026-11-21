#!/usr/bin/env python3
"""RemoteFM14Move: session view, proposal 1 (rfm1cs-abletonmove 0.9.2, after the owner's report of 2026-11-21): the
pressed clip shown as selected on the screen too (a frame round its cell), clips cut / copied / pasted with the
steps' gestures, the pads' pulse in steps. An Ableton Move Designer document (format 'ableton-move-designer' v3).

The screens and the LEDs are the core's own: its test move_sessview writes sessview_<name>.pbm and .leds into its
build directory (rfm1cs-abletonmove: ctest -R move_sessview).
    python3 make_session_v1.py <the core's build dir> <out.movedesign.json>"""
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
    tok = open(os.path.join(SHOTS, f"sessview_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first) and the 16 steps"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"sessview_{name}.leds")).read().strip().split("\n")]
    col = lambda h: None if h == "000000" else "#" + h
    return [[col(h) for h in r] for r in rows[:4]], [col(h) for h in rows[4]]


def m(**k):
    return {"meta": k}


def controls(name, held=None, pad_fn=None):
    pads, steps = leds(name)
    c = {}
    for i in range(3):
        c[f"track-{i + 1}"] = {**m(label=f"FW {i + 1}", function="A firmware's slot. A tap: its layout (a note layout: the "
                                   "sequencer's track view; session view is left)."),
                               "led": {"color": FWC[i] if i == 1 else None}}
    c["track-4"] = {**m(label="SEQ", function="Mixer / Routing; held: the Shift shortcuts; double: the projects"), "led": {"color": "#401010"}}
    c["menu"] = m(label="SESSION", function="A note layout: track view <-> session view", click="Track view <-> session view")
    c["back"] = m(label="BACK", function="Closes the latest layer: the clipboard's mark (a waiting copy, a cut), then session view")
    c["shift"] = m(label="SHIFT", function="Shift + a clip: its whole row, a scene across the 16 tracks (at the next bar; "
                                           "stopped: at once, playing with Play). Shift + Play: every clip stops")
    c["play"] = m(label="", function="Start / stop the sequencer (the only thing that does)")
    c["delete"] = {**m(label="DELETE", function="Held + a clip: cut (an X in its slot). The press right after a cut: pasted there "
                                             "(a clip there replaced). + an empty slot: the clipboard pasted, again and again. "
                                             "The steps' rules."),
                   "led": {"brightness": 1.0 if held == "delete" else 0.25}}
    c["copy"] = {**m(label="COPY", function="Held + a clip: copied (dashed). + an empty slot: pasted. Let go after copying, then "
                                         "a press on any slot: pasted there once (a clip there replaced). The steps' rules."),
                 "led": {"brightness": 1.0 if held == "copy" else 0.25}}
    c["mute"] = m(label="", function="Mute + a clip: its track muted / unmuted")
    c["undo"] = m(label="", function="Undo (Shift: redo): a cut or a paste taken back")
    for b, fn in {"left": "The tracks in view, one left", "right": "The tracks in view, one right (8 of 16 show)",
                  "up": "The clip slots in view, one up", "down": "The clip slots in view, one down (4 of 16 show)",
                  "capture": "As in track view", "sample": "As in track view", "loop": "Nothing here", "record": "Record: an "
                  "empty slot pressed with it armed records into the new clip"}.items():
        c[b] = m(label="", function=fn)
    c["volume"] = m(label="MASTER", function="The master volume")
    c["wheel"] = m(label="", function="The knob page, as in track view")
    for k in range(1, 9):
        c[f"knob-{k}"] = m(label="", function="A touch selects the track of this column (its selected clip framed)")
        c[f"knobled-{k}"] = {"led": {"color": None}}
    for r in range(4):
        for k in range(8):
            c[f"pad-{r + 1}-{k + 1}"] = {**m(label="", function=pad_fn or "A clip slot: column = track, row = clip. A press "
                                             "launches it (at the next bar; stopped: at once) and selects it; an empty slot: "
                                             "a new clip there. White: the selected clip. Green, blinking: waiting for the bar. "
                                             "Its track's colour: a clip (pulsing in 6 steps a beat: playing)."),
                                         "led": {"color": pads[r][k]}}
    for i in range(16):
        c[f"step-{i + 1}"] = {**m(label="", function=("Stop this column's track (at the bar): lit while it plays" if i % 2 == 0
                                                      else "Mute this column's track (its colour; muted: dim red)")),
                              "led": {"color": steps[i]}}
        c[f"stepicon-{i + 1}"] = {"led": {"brightness": 0}}
    return c


def layout(x, y, title, desc, name, smeta, held=None, pad_fn=None):
    scr, _ = screen(name)
    ctl = controls(name, held, pad_fn)
    ctl["screen"] = {"meta": smeta}
    return {"id": uid(), "x": x, "y": y, "w": 1048, "h": 616, "type": "layout", "name": title, "description": desc,
            "scope": ALL, "controls": ctl, "screen": scr}


def main():
    DX, DY = 1148, 920
    pos = lambda k: (60 + (k % 3) * DX, 60 + (k // 3) * DY)
    L = [
        ("A. Session view at rest", "Track 2 tapped (its note layout: the sequencer), then Menu. The owner's grid of 2026-11-21: "
         "tracks 1-4, clips 1 2 3 / 1 2 / 1 3 / 1 2. The slot's track (2) is selected: its clip's pad white, its cell framed.",
         "rest", {"label": "Session", "function": "8 tracks across, 4 clip slots down. NEW: the selected clip framed (a 16 x 15 "
                  "outline in the gap round its cell). The bar on top: the selected track's column."}, None, None,
         "NEW in 0.9.2: the frame. Before, the screen only had the bar over the selected track's column: nothing said which "
         "clip. The pads are unchanged: white = the selected clip, a track's colour = a clip, dark = empty."),
        ("B. Stopped: a clip pressed", "Track 3's third clip pressed: it is the track's clip at once (it plays with Play) and the "
         "selected one: its pad white, its cell framed, the bar over column 3.",
         "selected", {"label": "Session", "function": "The pressed clip framed; the bar moved to its column."}, None, None,
         "The same for an empty slot: a new clip there, selected and framed."),
        ("C. Playing: a clip pressed waits for the bar", "Track 1's second clip pressed while the sequencer plays: selected at once "
         "(framed; its doubled outline blinks while it waits), its pad blinks green until the next bar.",
         "queued", {"label": "Session", "function": "Framed at once (NEW: before, nothing changed on the screen but the small "
                    "doubled outline). Playing clips: filled, a black play icon."}, None, None,
         "Everything launches at the next bar (the owner's rule). A clip pressed is now visibly the selected one while it waits."),
        ("D. At the bar: it plays", "The clip plays: its pad white, pulsing; its cell filled, standing clear of its frame. The "
         "clip it replaced is its track's colour again.",
         "playing", {"label": "Session", "function": "The selected playing clip: filled, inside its frame."}, None, None,
         "The pulse now fades in 6 steps a beat. It was a new level every tick (96 a beat): every playing clip an RGB write "
         "a tick, about 630 a second to the Move with four clips at 99 BPM, against 13 in track view. Not confirmed on the "
         "hardware whether that flood is what kept the pads from showing presses."),
        ("E. Delete held + a clip: cut", "Clips are cut and pasted as steps are. Delete held: the first clip pressed is cut (an X in "
         "its slot, the pad blinking red); the press right after a cut pastes (a clip there is replaced); after a paste a "
         "clip is cut again and an empty slot gets the clipboard. The clipboard stays: it pastes again and again.",
         "cut", {"label": "Session", "function": "An X in the cut clip's slot."}, "delete",
         "Delete held: cut this clip / paste here (see the rules). Without Delete: launch and select.",
         "The owner's steps sequence, on clips: cut A; A again: pasted back; an empty slot: pasted; B: cut; C right after: "
         "pasted over C; C again: cut. Said: \"Cut track 1 clip 1\", \"Pasted to track 1 clip 3\", \"Track 3 clip 1 replaced\"."),
        ("F. Copy held + a clip: copied", "Copy held: a clip pressed is copied (dashed, the pad blinking white), an empty slot "
         "pressed gets it. Copy let go after copying, then a press on any slot: pasted there once (a clip there replaced).",
         "copied", {"label": "Session", "function": "The copied clip dashed while Copy is held and while a copy waits."}, "copy",
         "Copy held: copy this clip / paste into this empty slot. Without Copy: launch and select.",
         "Back drops a waiting copy's mark (a layer); Back again leaves session view. Copy tapped alone drops it too, or "
         "duplicates the selected clip into its track's next empty slot (as before)."),
    ]
    items = [{"id": uid(), "x": 0, "y": 0, "w": 3 * DX + 900, "h": 2 * DY + 160, "type": "group",
              "title": "Session view, proposal 1 (rfm1cs 0.9.2): the selected clip on the screen, clips cut / copied as steps",
              "color": "#2ad17a"}]
    for k, (title, desc, name, smeta, held, pad_fn, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, held, pad_fn))
        items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 200, "type": "note", "text": nt, "color": "#cfe3ff"})
    items.append({"id": uid(), "x": 60 + 3 * DX, "y": 60, "w": 780, "h": 1300, "type": "note", "color": "#ffe58a",
                  "text": "SESSION VIEW (0.9.2)\n"
                          "The way in: a firmware's Track tapped (a note layout), then Menu. Menu, Back or a Track tap: out.\n\n"
                          "A CLIP PRESSED: launched at the next bar (stopped: at once) and selected at once: pad white "
                          "(green, blinking, while it waits), cell framed on the screen.\n"
                          "AN EMPTY SLOT PRESSED: a new clip there, the same.\n"
                          "SHIFT + A CLIP: its row, a scene across the 16 tracks. Tracks with no clip in the row stop.\n"
                          "THE STEPS: two under each column: stop the track, mute it.\n"
                          "A KNOB TOUCHED: the track of its column selected.\n\n"
                          "CUT / COPY / PASTE, as the steps':\n"
                          "Delete held, a clip: cut. The press right after a cut: pasted (a clip there replaced).\n"
                          "Delete held, an empty slot: the clipboard pasted. It pastes again and again.\n"
                          "Copy held, a clip: copied. Copy held, an empty slot: pasted.\n"
                          "Copy let go after copying, then any slot: pasted there once.\n\n"
                          "UNCHANGED: the launches, the pads' colours, the mapping (column = track, row = clip; the screen and "
                          "the pads show the same 8 x 4). No fault was found in them on the simulator with the Move's own "
                          "build, project and firmware states.\n\n"
                          "TO DECIDE\n"
                          "1. The frame as the selected clip's mark: OK, or another mark?\n"
                          "2. Delete held, two clips in a row: the second is replaced by the first (the steps' rule). OK for "
                          "clips, or should Delete + a clip always cut?"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "Session view, proposal 1", "items": items},
              open(OUT, "w"), indent=1)
    try:                                                # a PNG of the six screens for a quick look
        from PIL import Image
        sh = Image.new("RGB", (3 * 522, 2 * 266), (50, 50, 50))
        for i, (_, _, name, *_r) in enumerate(L):
            bits = screen(name)[1]
            im = Image.new("RGB", (128, 64), (0, 0, 0))
            im.putdata([(255, 255, 255) if b else (0, 0, 0) for b in bits])
            sh.paste(im.resize((512, 256), Image.NEAREST), ((i % 3) * 522 + 5, (i // 3) * 266 + 5))
        sh.save(OUT.replace(".movedesign.json", "-screens.png"))
    except ImportError:
        pass


main()
