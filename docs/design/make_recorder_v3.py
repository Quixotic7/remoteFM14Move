#!/usr/bin/env python3
"""RemoteFM14Move: the recorder, the recordings and the looper (the owner's request of 2026-11-24), as an Ableton Move
Designer document (format 'ableton-move-designer' v3). A PROPOSAL, round 3 (the sampler's screens after the owner's references: Digitakt, Tonverk, Torso S-4), drawn before any code: the screens and lights are
mock_recorder_v3.c's (rec3_<name>.pbm and .leds in its working directory).
    python3 make_recorder_v3.py <the mock's directory> <out.movedesign.json> [<boards.json for the review page>]"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(202611243)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
BTN = ["sample", "record", "play", "loop", "mute", "delete", "copy", "undo"]


def screen(name):
    tok = open(os.path.join(SHOTS, f"rec3_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons, the 8 buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"rec3_{name}.leds")).read().strip().split("\n")]
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
        c[f"track-{i + 1}"] = {**m(label="MIX" if i == 3 else f"FW {i + 1}", function=t.get(f"track{i + 1}", t.get("track", "A firmware's slot: orange, blue, yellow"))),
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


UNDER = {"pad": "The layer under the app: the firmware's note layout, played as before", "step": "The clip's steps, as before",
         "knob": "The layer under the app (the app takes no knob)"}
APP = {**UNDER, "sample": "Opens the sampler. In it: record / stop. White: the app is open. Red: recording",
       "jog": "Turn: the next cell. Click: edit it (the whole cell lit); turn changes it; click or Back: done",
       "back": "Editing: done. Else: closes the app",
       "track": "Held + Sample: the app opens with that firmware as the source (nothing records yet)"}
TRIM = {**APP, "knob1": "Start", "knob2": "End", "knob": "Dark and dead while trimming",
        "jog": "Turn: Start, End, Play, Save. Click: edit a marker (the view zooms in on it), play, or save",
        "delete": "Held + the jog's click: the take discarded (Undo: back)", "back": "Editing: done. Else: the take kept as it is"}
LIST = {**APP, "jog": "Turn: the next row. Click: plays the take / opens the folder", "left": "The folder above; at the top: the recorder",
        "right": "The chosen take's menu", "delete": "Held + the jog's click: the take deleted (Undo: back)"}
MENU_ = {**APP, "jog": "Turn: the next line. Click: does it", "back": "The list", "left": "The list"}
KIT = {"pad": "The kit's 16 pads, bottom-left 4 x 4. A press plays its sample and selects it (white). Dark: empty",
       "step": "The kit's track: its steps", "knob1": "Level", "knob2": "Pan", "knob3": "Tune", "knob4": "Mode: one shot, held, loop",
       "knob5": "Start", "knob6": "Length", "knob7": "Decay", "knob8": "FX send",
       "jog": "Click: pick the chosen pad's sample from the list", "track4": "The kit", "sample": "Opens the sampler", "back": "As everywhere"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. The recorder", "The level along the top with the threshold's mark; eight settings, the value over its name; the source's sound along the bottom. The jog is on MODE.",
         "rec", S("Recorder", "Meter, 4 x 2 settings, live wave"), APP, "After the Digitakt's recorder page."),
        ("B. A setting edited", "The jog's click lights the whole cell; the jog changes it.", "rec_edit", S("Recorder", "THRES edited"), APP, ""),
        ("C. Recording", "The take grows across the screen; bar lines; the strip counts the bars.", "rec_run", S("Recording", "Source, bars done / wanted, level"), APP, ""),
        ("D. Trim", "What is kept is the white block. Start, End, Play, Save.", "trim", S("Trim", "The take, its kept part, four cells"), TRIM,
         "After the Tonverk's and Digitakt's trim. Editing a marker zooms in on it."),
        ("E. The takes", "A list, the newest first: time, source, length.", "list", S("Takes", "Today's folder"), LIST, ""),
        ("F. The folders", "A folder a day, then Move's library and audio/.", "list_root", S("Samples", "The folders"), LIST, ""),
        ("G. A take's menu", "Right arrow on a take.", "menu", S("A take", "Its wave and facts; what to do with it"), MENU_, ""),
        ("H. A kit pad", "Its sample with start and end; eight settings, a knob each.", "kit", S("Kit pad", "Sample, 4 x 2 settings"), KIT, "After the Digitakt's source page and the S-4."),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL, round 3 (not built): the sampler's screens redrawn (2026-11-24)", "color": "#ffb000"}]
    boards = []
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
        boards.append({"id": name, "title": title, "desc": desc, "note": nt, "screen": screen(name)[0], "leds": leds(name)})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: the sampler's screens, proposal 3",
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
