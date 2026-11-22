#!/usr/bin/env python3
"""RemoteFM14Move: projects that hold their firmwares, and firmware states on the Move (the owner's request of
2026-11-22), as an Ableton Move Designer document (format 'ableton-move-designer' v3). A PROPOSAL, drawn before any
code: the screens and lights are mock_projstates_v1.c's (sessview_ps_<name>.pbm and .leds in its working directory).
The held Track's menu is the build's own (the core's test dump sessview_hold) with pad 7 lit.
    python3 make_projstates_v1.py <the mock's directory> <out.movedesign.json> [<the core's build directory>]"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(20261122)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

if len(sys.argv) > 3:                                   # the held Track's menu: the build's, pad 7 added (slot 2: blue)
    core = sys.argv[3]
    open(os.path.join(SHOTS, "sessview_ps_trk_menu.pbm"), "w").write(open(os.path.join(core, "sessview_hold.pbm")).read())
    rows = open(os.path.join(core, "sessview_hold.leds")).read().strip().split("\n")
    top = rows[0].split()
    top[6] = "2878ff"
    rows[0] = " ".join(top)
    open(os.path.join(SHOTS, "sessview_ps_trk_menu.leds"), "w").write("\n".join(rows) + "\n")

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])


def screen(name):
    tok = open(os.path.join(SHOTS, f"sessview_ps_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons (white), the 4 Track buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"sessview_ps_{name}.leds")).read().strip().split("\n")]
    col = lambda h: None if h == "000000" else "#" + h
    return ([[col(h) for h in r] for r in rows[:4]], [col(h) for h in rows[4]], [col(h) for h in rows[5]],
            [int(h, 16) / 127 for h in rows[6]], [col(h) for h in rows[7]])


def m(**k):
    return {"meta": k}


def controls(name, t):
    pads, steps, rings, icons, tracks = leds(name)
    c = {}
    for i in range(4):
        c[f"track-{i + 1}"] = {**m(label="SEQ" if i == 3 else f"FW {i + 1}", function=t.get("track", "A firmware's slot: orange, blue, yellow")),
                               "led": {"color": tracks[i]}}
    c["menu"] = m(label="", function=t.get("menu", "As in the layer under this one"))
    c["back"] = m(label="BACK", function=t.get("back", "A quick click closes the latest layer"))
    c["shift"] = m(label="SHIFT", function=t.get("shift", "The Shift layer"))
    c["play"] = m(label="", function=t.get("play", "Start / stop the sequencer"))
    c["wheel"] = m(label="", function=t.get("jog", "Nothing here"))
    c["volume"] = m(label="MASTER", function=t.get("volume", "The master volume"))
    for b in ("delete", "copy", "mute", "undo", "left", "right", "up", "down", "capture", "sample", "loop", "record"):
        c[b] = m(label="", function=t.get(b, "Nothing here"))
    for k in range(8):
        c[f"knob-{k + 1}"] = m(label="", function=t.get(f"knob{k + 1}", t.get("knob", "Nothing here (dark)")))
        c[f"knobled-{k + 1}"] = {"led": {"color": rings[k]}}
    for r in range(4):
        for k in range(8):
            fn = t.get(f"pad{r + 1}{k + 1}", t.get("pad", ""))
            c[f"pad-{r + 1}-{k + 1}"] = {**m(label="", function=fn), "led": {"color": pads[r][k]}}
    for i in range(16):
        c[f"step-{i + 1}"] = {**m(label="", function=t.get(f"step{i + 1}", t.get("step", ""))), "led": {"color": steps[i]}}
        c[f"stepicon-{i + 1}"] = {"led": {"brightness": round(icons[i], 2)}}
    return c


def layout(x, y, title, desc, name, smeta, t):
    ctl = controls(name, t)
    ctl["screen"] = {"meta": smeta}
    return {"id": uid(), "x": x, "y": y, "w": 1048, "h": 616, "type": "layout", "name": title, "description": desc,
            "scope": ALL, "controls": ctl, "screen": screen(name)[0]}


PROJ = {"pad": "A project, in its colour (white: the open one). A press opens it: its firmwares load, each put back in "
               "the state the project left it in. Held: what is in it. An empty pad: a new project",
        "knob1": "A held project's colour", "copy": "Held + a project: copied, with its firmwares and their states",
        "delete": "Held + a project: cut", "track": "Lights in its slot's colour when its firmware is ready",
        "play": "While a project loads: it plays once every firmware is ready"}
MENU = {"pad": "The layout menu's scale and root", "pad11": "FM-1 layout", "pad12": "The firmware's takeover",
        "pad13": "Scale", "pad14": "Chroma", "pad15": "Drum", "pad16": "Octave",
        "pad17": "NEW: States. Lit in the slot's colour when a firmware is loaded. A press opens this firmware's "
                 "States; they stay after the Track is let go",
        "pad18": "Latch", "step": "The 16 tracks: route, solo, select", "knob": "CC numbers", "jog": "Root",
        "track": "Track 2 held: its menu"}
STATES = {"pad": "A state of this firmware. Stored (dim): a press loads it. Empty (dark): a press saves there. Copy "
                 "held + a pad: saved over. Delete held + a pad: deleted. The current one is full",
          "step": "Dark and dead", "step14": "New: the firmware restarts as first installed (its icon lit). Undo brings the old state back",
          "knob": "Dark and dead", "jog": "Dead", "back": "Closes States", "undo": "Right after a load or New: what the firmware held is back",
          "copy": "Held + a pad: the firmware's state now, saved over it", "delete": "Held + a pad: that state deleted",
          "loop": "In the firmware browser: opens States too (as today)"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. A project pressed: it loads", "The project's number and its three firmwares, a bar each. Slot 1 held the "
         "same firmware (only its state swapped: ready), slot 2 loads, slot 3 waits.", "proj_loading",
         S("Loading", "A bar a firmware; dotted: waiting"), PROJ,
         "The sound stops while firmwares load. A Track button lights when its firmware is ready."),
        ("B. A firmware that is not on this Move", "Said for a second; the slot stays empty. The project keeps the "
         "firmware's name and state.", "proj_missing", S("Message", "A second"), PROJ, "Nothing downloads by itself."),
        ("C. A project held", "Its number, tempo, clips, and its three firmwares.", "proj_held",
         S("Project", "While held"), PROJ, "The activity bars go, to make room."),
        ("D. A project held: a slot empty", "An empty slot is a dotted line.", "proj_held_empty", S("Project", "While held"), PROJ, ""),
        ("E. Track 2 held: pad 7 is States", "The layout menu as built, with pad 7 of the top row lit in the slot's "
         "colour.", "trk_menu", S("Slot summary", "While held (as built)"), MENU,
         "Today States are reached by a double click on a Track, then Loop."),
        ("F. States", "The firmware's name, its 32 states: framed stored, filled current, a dot empty. Pads in the "
         "slot's colour. Step 14's icon lit: New.", "states", S("States", "ChoralRoot, slot 1"), STATES,
         "States are the firmware's own, shared by every project."),
        ("G. Step 14: a new state", "The firmware restarts as first installed. No stored state is touched; none is "
         "the current one.", "state_new", S("Message", "A second"), STATES, ""),
        ("H. Undo after it", "What the firmware held before is back. The same after a state is loaded.", "state_undo",
         S("Message", "A second"), STATES, "One step: the last load only."),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL (not built): a project holds its firmwares; firmware states on the Move (2026-11-22)", "color": "#ffb000"}]
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: projects and states, proposal 1",
               "items": items}, open(OUT, "w"), indent=1)
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
