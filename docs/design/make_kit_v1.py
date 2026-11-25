#!/usr/bin/env python3
"""RemoteFM14Move: the sampler's stage 3, the kit (Track 4's Kit view, 16 sampled pads), as an Ableton Move Designer
document (format 'ableton-move-designer' v3). A PROPOSAL, round 1, drawn before any code: the screens and lights are
mock_kit_v1.c's (kit1_<name>.pbm and .leds in its working directory).
    python3 make_kit_v1.py <the mock's directory> <out.movedesign.json> [<boards.json for the review page>]"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(202611253)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
BTN = ["sample", "record", "play", "loop", "mute", "delete", "copy", "undo"]


def screen(name):
    tok = open(os.path.join(SHOTS, f"kit1_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons, the 8 buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"kit1_{name}.leds")).read().strip().split("\n")]
    col = lambda h: None if h == "000000" else "#" + h
    return {"pads": [[col(h) for h in r] for r in rows[:4]], "steps": [col(h) for h in rows[4]],
            "knobs": [col(h) for h in rows[5]], "icons": [int(h, 16) / 127 for h in rows[6]],
            "tracks": [col(h) for h in rows[7]], "btn": dict(zip(BTN, [col(h) for h in rows[8]]))}


def m(**k):
    return {"meta": k}


def controls(name, t):
    L = leds(name)
    c = {}
    for i in range(4):
        c[f"track-{i + 1}"] = {**m(label="KIT" if i == 3 else f"FW {i + 1}", function=t.get(f"track{i + 1}", t.get("track", "A firmware's slot: orange, blue, yellow"))),
                               "led": {"color": L["tracks"][i]}}
    c["menu"] = m(label="", function=t.get("menu", "As in the layer under this one"))
    c["back"] = m(label="BACK", function=t.get("back", "A quick click closes the latest layer"))
    c["shift"] = m(label="SHIFT", function=t.get("shift", "The Shift layer"))
    c["wheel"] = m(label="", function=t.get("jog", "As in the layer under this one"))
    c["volume"] = m(label="MASTER", function=t.get("volume", "The master volume"))
    for b in ("left", "right", "up", "down", "capture"):
        c[b] = m(label="", function=t.get(b, "As in the layer under this one"))
    for b in BTN:
        c[b] = {**m(label="", function=t.get(b, "As in the layer under this one")), "led": {"color": L["btn"][b]}}
    for k in range(8):
        c[f"knob-{k + 1}"] = m(label="", function=t.get(f"knob{k + 1}", t.get("knob", "As in the layer under this one")))
        c[f"knobled-{k + 1}"] = {"led": {"color": L["knobs"][k]}}
    for r in range(4):
        for k in range(8):
            fn = t.get(f"pad{r + 1}{k + 1}", t.get("pad", ""))
            c[f"pad-{r + 1}-{k + 1}"] = {**m(label="", function=fn), "led": {"color": L["pads"][r][k]}}
    for i in range(16):
        c[f"step-{i + 1}"] = {**m(label="", function=t.get(f"step{i + 1}", t.get("step", ""))), "led": {"color": L["steps"][i]}}
        c[f"stepicon-{i + 1}"] = {"led": {"brightness": round(L["icons"][i], 2)}}
    return c


def layout(x, y, title, desc, name, smeta, t):
    ctl = controls(name, t)
    ctl["screen"] = {"meta": smeta}
    return {"id": uid(), "x": x, "y": y, "w": 1048, "h": 616, "type": "layout", "name": title, "description": desc,
            "scope": ALL, "controls": ctl, "screen": screen(name)[0]}


KIT = {"pad": "Left 4 x 4: the kit's 16 pads (pad 1 bottom left), a press plays and chooses one (white); right 4 x 4: the chosen pad's pitches",
       "step": "The chosen pad's steps only (on its track, 13..16)", "knob": "The chosen pad's eight: pitch, stretch, start, play, attack, release, level, send 1; a touch: its name and value",
       "jog": "Turn: the next row; past the first: the tabs (Pad, Kit, Files). Click: the row's popup", "track4": "Kit (a click: the next view; held + pad 3: Kit)",
       "sample": "A pad held + Sample: records into that pad", "copy": "Held + a pad, then another: copy, mix or join", "delete": "Held + a pad: its steps, then its sample",
       "back": "A popup: done. On the tabs: nothing more (Back never leaves the view)"}
POP = {**KIT, "jog": "Turn: the value. Click or Back: done"}
FILES = {**KIT, "jog": "Turn: the next place, folder or sound (each sound plays as the jog lands). Click: open, or load onto the chosen pad",
         "pad": "Left 4 x 4: choose the pad a sound loads onto (blinking white)", "shift": "Held + the click on a folder: its actions"}
REC = {**KIT, "sample": "Red: recording into the held pad; let go of the pad (or the length, or the threshold) ends it", "pad": "The held pad: red"}
SMP = {"jog": "As in the sampler (built)", "sample": "Records (the sampler's Record tab)", "pad": "Left 4 x 4: the kit; the next empty pad lit: where the take lands"}
ROUTE = {"step": "Steps 13..16: the kit's tracks (its colours); a hold routes a track to the firmware or back to the kit", "track1": "Held"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. The Kit view", "Pad tab at rest: the chosen pad's sound, the steps it lasts, one row.", "rest", S("Kit, Pad tab", "Tabs, the pad's number, its wave, the step grid, one row"), KIT, "Pad 1 is bottom left; a pad row a kit track (13..16), a colour each."),
        ("B. Three rows", "While the jog turns: three rows, the chosen in the middle.", "three", S("Kit, Pad tab", "Three rows"), KIT, ""),
        ("C. A value", "Knob 1 touched or the row clicked: its name and value, the steps it lasts under it.", "pitch", S("Popup", "Pitch"), POP, ""),
        ("D. Stretch", "Fitted to 16 steps (repitched); the steps it lasts lit.", "stretch", S("Popup", "Stretch"), POP, ""),
        ("E. How it plays", "One shot, while held, loop; the envelope under it.", "mode", S("Popup", "Play mode"), POP, ""),
        ("F. The envelope", "Attack turned: its part drawn thick.", "attack", S("Popup", "Attack"), POP, ""),
        ("G. Choke group", "The pads of the group lit; the others dim.", "choke", S("Popup", "Choke"), POP, ""),
        ("H. A send", "Send 1 of the three audio FX.", "send", S("Popup", "Send 1"), POP, ""),
        ("I. Files: places", "Takes, Move's Samples, saved kits.", "places", S("Kit, Files tab", "Places"), FILES, ""),
        ("J. Files: a folder", "A sound a row, its wave's picture; each plays as the jog lands.", "files", S("Kit, Files tab", "Sounds"), FILES, ""),
        ("K. A whole folder", "Shift + click on a folder: All To Pads.", "folder", S("Folder actions", ""), FILES, ""),
        ("L. Pad held + Sample", "Armed, waiting for the threshold.", "armed", S("Recording into pad 7", "Armed"), REC, ""),
        ("M. Recording", "The wave grows; the pad's release ends it.", "recording", S("Recording into pad 7", ""), REC, ""),
        ("N. Copy, mix, join", "Copy held, pad 2 then pad 5: the three choices.", "merge", S("Copy held", ""), KIT, ""),
        ("O. Joined", "Pad 5 holds 2 and 5 end to end, a new file.", "joined", S("Kit, Pad tab", ""), KIT, ""),
        ("P. Slice a take", "A take's To Kit: 16 slices by its hits onto pads 1..16.", "slice", S("Slice", ""), KIT, ""),
        ("Q. Kit tab", "Load, Save, the right half's layout.", "kittab", S("Kit, Kit tab", ""), KIT, ""),
        ("R. Saved", "A kit is a folder of its sounds.", "saved", S("Message", ""), KIT, ""),
        ("S. In key", "The right half: the scale's notes, rows a fourth apart.", "inkey", S("Popup", "In key"), POP, ""),
        ("T. Routing", "A firmware Track held: steps 13..16 are the kit's tracks.", "route", S("Track 1 held", ""), ROUTE, ""),
        ("U. Sampler: Kit", "Source Kit; To: the next empty pad.", "sampler", S("Sampler, Record", ""), SMP, ""),
        ("V. A pad's tools", "The jog's click on the pad's sound row: play, trim, normalize, reverse, delete.", "padact", S("Pad tools", ""), KIT, ""),
        ("W. Further rows", "Envelope, choke, level (then pan, sends, reverse).", "rows2", S("Kit, Pad tab", ""), KIT, ""),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL, kit round 1 (not built): Track 4's Kit view, 16 sampled pads (2026-11-25)", "color": "#ff3ca0"}]
    boards = []
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
        boards.append({"id": name, "title": title, "desc": desc, "note": nt, "screen": screen(name)[0], "leds": leds(name)})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: the kit, proposal 1",
               "items": items}, open(OUT, "w"), indent=1)
    if len(sys.argv) > 3:
        json.dump(boards, open(sys.argv[3], "w"))
    try:
        from PIL import Image
        sh = Image.new("RGB", (COLS * 522, rows * 266), (50, 50, 50))
        for i, (_, _, name, *_r) in enumerate(L):
            bits = screen(name)[1]
            im = Image.new("RGB", (128, 64), (0, 0, 0))
            im.putdata([(255, 255, 255) if b else (0, 0, 0) for b in bits])
            sh.paste(im.resize((512, 256), Image.NEAREST), ((i % COLS) * 522 + 5, (i // COLS) * 266 + 5))
        sh.save(OUT.replace(".movedesign.json", "-screens.png"))
    except ImportError:
        pass


main()
