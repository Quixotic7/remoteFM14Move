#!/usr/bin/env python3
"""RemoteFM14Move: the looper (Track 4's fourth view, the sampler's Loops tab), round 1, as an Ableton Move Designer
document (format 'ableton-move-designer' v3), a PNG per screen (128 x 64 at 4x) and a contact sheet. A PROPOSAL, drawn
before any code: the screens and lights are mock_looper_v1.c's (loop1_<name>.pbm and .leds in its working directory).
    python3 make_looper_v1.py <the mock's directory> <out.movedesign.json> <png dir> [<boards.json for the page>]"""
import base64, json, os, random, sys

SHOTS, OUT, PNG = sys.argv[1], sys.argv[2], sys.argv[3]
random.seed(202611261)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
BTN = ["sample", "record", "play", "loop", "mute", "delete", "copy", "undo"]


def screen(name):
    tok = open(os.path.join(SHOTS, f"loop1_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"loop1_{name}.leds")).read().strip().split("\n")]
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
        c[f"track-{i + 1}"] = {**m(label="LOOPS" if i == 3 else f"FW {i + 1}",
                                   function=t.get(f"track{i + 1}", t.get("track", "A tap: play that firmware (leaves the Looper)"))),
                               "led": {"color": L["tracks"][i]}}
    c["menu"] = m(label="", function=t.get("menu", "As in the layer under this one"))
    c["back"] = m(label="BACK", function=t.get("back", "A popup: done. A page: nothing more (Back never leaves the view)"))
    c["shift"] = m(label="SHIFT", function=t.get("shift", "Held + a pad: choose that slot silently; held + a knob: fine"))
    c["wheel"] = m(label="", function=t.get("jog", "As in the layer under this one"))
    c["volume"] = m(label="MASTER", function=t.get("volume", "The master volume; a slot held: that loop's level"))
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


GRID = {"pad": "The grid: a row a lane (lane 1 the top row, beside Track 1), a column a scene. Empty: tap = record at the next bar. "
               "A loop: tap = plays at the next bar (double = now); playing: tap = stops at the bar (double = now). Shift + pad: choose only",
        "step": "Under each column: the left step plays that scene (all four lanes) at the next bar; step 16: stop all (lit icon)",
        "knob": "The page's knobs (lit teal); a touch: its name and value", "jog": "Turn: the pages (Loops, Loop, Record, Loop Mix, Looper Mix). Click: (stage 2) the loop's actions",
        "track4": "Looper (a click: the next view, Mixer; double: projects)", "sample": "A slot held + Sample: record into it (replacing)",
        "copy": "Held + a loop, then a slot: copied (as session view)", "delete": "Held + a loop: deleted (Undo: back)", "undo": "The last record, delete, copy or clear undone",
        "play": "The transport; stopped: armed and queued slots start with it", "record": "(stage 2) Held + a playing loop: overdub",
        "capture": "(stage 2) The last bars of the chosen lane's source into the chosen empty slot"}
POP = {**GRID, "jog": "As the kit: a knob's popup goes after a moment"}
REC = {**GRID, "pad": "The red pad: recording (blinking: armed for the next bar). Tap it: ends at the next bar and plays (Open)",
       "sample": "Red while recording; a press ends it at the next bar"}
TAB = {"pad": "Not used: the firmware's (the tab needs no pads)", "jog": "Turn: the slot (lane by lane); past the first: the tabs. Click: record / play / stop that slot (at the bar)",
       "knob": "Knobs 1-4: lanes 1-4 level; 5-8 dark", "sample": "Records into the jog's slot (an empty one), or ends the recording", "back": "The tabs, then out",
       "track": "As in the sampler app (Track 1-3 held + Sample: that firmware the source)"}
HELD = {"pad": "Pads 1-5: Mixer, Routing, Kit, Looper (pad 4, teal), Projects", "track4": "Held"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. Loops page", "The Looper at rest: a lane a row, the scene it plays, its bars filling as it plays.", "loops", S("Looper, Loops", "Lanes 1-4: scene, bars, playhead; lane 4 stopped"), {**GRID, "knob": "Knobs 1-4: lanes 1-4 level"}, "Pads: lane 1 top row; green playing, teal a loop, white the chosen slot."),
        ("B. Page switcher", "The jog turning: the Looper's five pages, as the kit's.", "switch", S("Pages", "Loops, Loop, Record, Loop Mix, Looper Mix"), GRID, ""),
        ("C. Loop page", "The chosen loop (lane 2, scene 2): its wave over its bars, the playhead; its place top right.", "loop", S("Looper, Loop", "1 Start, 2 Length, 3 Pitch, 4 Reverse, 5 Warp, 6 Launch"), {**GRID, "knob1": "Start (steps)", "knob2": "Length (bars)", "knob3": "Pitch (repitch)", "knob4": "Reverse", "knob5": "Warp: Repitch / Off", "knob6": "(stage 2) Launch: Trigger / Gate / Toggle"}, ""),
        ("D. Pitch", "Knob 3: the loop repitched (still in time).", "pitch", S("Popup", "Pitch"), POP, ""),
        ("E. Length", "Knob 2: the bars it plays of those recorded.", "length", S("Popup", "Length"), POP, ""),
        ("F. Start", "Knob 1: the start moved by steps; still locked to the bar.", "start", S("Popup", "Start"), POP, ""),
        ("G. Record page", "An empty slot chosen: its lane's Source and Length.", "recpage", S("Looper, Record", "1 Source, 2 Length"), {**GRID, "knob1": "Source", "knob2": "Length: Open, 1, 2, 4, 8, 16 bars"}, ""),
        ("H. Source", "Knob 1 on Record: Track 2 (its button bright).", "source", S("Popup", "Source"), POP, "Sources: Played, Track 1-3, Kit, Input, Mix (no loops), Loops (resample)."),
        ("I. Armed", "Empty slot tapped while playing: waits for the bar; the beats to go.", "armed", S("Recording", "Armed"), REC, ""),
        ("J. Recording, Open", "Bar 3 being recorded; tap the pad (or Sample) to end at the next bar.", "recording", S("Recording", "Open"), REC, ""),
        ("K. Recording, 2 bars", "Fixed length: ends by itself and plays at once.", "fixed", S("Recording", "Fixed"), REC, ""),
        ("L. Launch pending", "Scene 3 launched (step 5): lanes wait for the bar, dashed; the beats gone.", "pending", S("Looper, Loops", "Queued"), GRID, "Blinking green: lined up. Blinking teal: about to stop."),
        ("M. Loop Mix", "As the kit's Pad Mix: level, pan, sends 1-3, mute.", "loopmix", S("Looper, Loop Mix", "1 Level, 2 Pan, 3-5 Sends, 6 Mute"), GRID, ""),
        ("N. Looper Mix", "Lanes 1-4 levels, then the Loops strip (Mixer column 5).", "loopermix", S("Looper, Looper Mix", "1-4 lane levels, 5 Loops level, 6 pan, 7-8 sends 1-2"), GRID, ""),
        ("O. Loops tab", "In the sampler app: works with no pads (they stay the firmware's).", "tab", S("Sampler, Loops", "Grid, one row"), TAB, ""),
        ("P. Loops tab, recording", "A slot recording (solid, a dot); one queued (dashed).", "tabrec", S("Sampler, Loops", "Recording"), TAB, ""),
        ("Q. Track 4 held", "Pad 4: Looper (the settled shortcut).", "held4", S("Track 4 held", ""), HELD, ""),
        ("R. Undo", "One undo: the last record, delete or copy.", "undo", S("Message", ""), GRID, ""),
        ("S. Copy", "Copy held + loop 1-1: then a slot to paste.", "copy", S("Copy held", ""), GRID, ""),
        ("T. To Kit (stage 2)", "The jog's click on a loop: Play, To Kit, To Takes.", "tokit", S("Loop actions", ""), GRID, ""),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL, looper round 1 (not built): Track 4's Looper, 4 lanes x 8 scenes; the Loops tab (2026-11-26)",
              "color": "#00d2be"}]
    boards = []
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cff7f2"})
        boards.append({"id": name, "title": title, "desc": desc, "note": nt, "screen": screen(name)[0], "leds": leds(name)})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: the looper, proposal 1",
               "items": items}, open(OUT, "w"), indent=1)
    if len(sys.argv) > 4:
        json.dump(boards, open(sys.argv[4], "w"))
    from PIL import Image
    os.makedirs(PNG, exist_ok=True)
    sh = Image.new("RGB", (COLS * 522, rows * 266), (50, 50, 50))
    for i, (_, _, name, *_r) in enumerate(L):
        bits = screen(name)[1]
        im = Image.new("RGB", (128, 64), (0, 0, 0))
        im.putdata([(255, 255, 255) if b else (0, 0, 0) for b in bits])
        big = im.resize((512, 256), Image.NEAREST)
        big.save(os.path.join(PNG, f"loop1_{name}.png"))
        sh.paste(big, ((i % COLS) * 522 + 5, (i // COLS) * 266 + 5))
    sh.save(OUT.replace(".movedesign.json", "-screens.png"))


main()
