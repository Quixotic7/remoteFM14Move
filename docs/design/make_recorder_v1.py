#!/usr/bin/env python3
"""RemoteFM14Move: the recorder, the recordings and the looper (the owner's request of 2026-11-24), as an Ableton Move
Designer document (format 'ableton-move-designer' v3). A PROPOSAL, drawn before any code: the screens and lights are
mock_recorder_v1.c's (rec_<name>.pbm and .leds in its working directory).
    python3 make_recorder_v1.py <the mock's directory> <out.movedesign.json> [<boards.json for the review page>]"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(20261124)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
BTN = ["sample", "record", "play", "loop", "mute", "delete", "copy", "undo"]


def screen(name):
    tok = open(os.path.join(SHOTS, f"rec_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons, the 8 buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"rec_{name}.leds")).read().strip().split("\n")]
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


UNDER = {"pad": "The layer under the recorder: the firmware's note layout", "step": "The clip's steps, as before"}
MENU = {**UNDER, "sample": "Shift + Sample: the recorder's menu (again, or Back: closed)",
        "knob1": "Source: Master, Slot 1, Slot 2, Slot 3, Input, Stems", "knob2": "Mode: Capture, Bars, Threshold, Manual",
        "knob3": "Length: Open, 1, 2, 4, 8, 16, 32 bars (Capture: Auto, 1 .. 16)", "knob4": "Level: the threshold, and the source's level now",
        "knob5": "Monitor: the input heard in the mix, Off / On", "knob6": "Looper: a finished take also lands in the next empty loop and plays, Off / On",
        "knob7": "Dark and dead", "knob8": "Dark and dead", "jog": "Selects a row; its click edits it", "back": "Closes the menu"}
REC = {**UNDER, "sample": "Sample: the record key. Blinking red: armed. Red: recording. Dim white: Capture is listening. "
                          "A green flash: saved", "track": "Held + Sample: record that firmware now"}
TAKES = {"pad": "A take, the newest first, in its source's colour (white the master, a slot's colour, teal the input, "
                "purple stems). A press plays it (green) and selects it; again: stopped", "step": "Dark and dead",
         "step1": "All takes", "step2": "The master's", "step3": "Slot 1's", "step4": "Slot 2's", "step5": "Slot 3's",
         "step6": "The input's", "delete": "Held + a take: deleted (Undo: back)", "copy": "Held + a take: copied; an empty loop in the looper takes it",
         "left": "Older takes", "right": "Newer takes", "jog": "The next / previous take; its click plays or stops it",
         "volume": "The level it plays at", "back": "Leaves the recordings", "track4": "Track 4 held + pad 3 opened this",
         "knob": "Dark and dead", "sample": "The record key, as everywhere"}
LOOPER = {"pad": "A loop. Empty (dark): a press records there, from the recorder's source. Stored (its source's colour): "
                 "a press plays it from the next bar (green), a double click now. Playing: a double click stops it. "
                 "One loop a column plays at a time", "step": "Odd: the column above selected. Even: it stops. Step 16: all stop",
          "knob": "The column's level, lit in its colour", "record": "Overdub into the chosen loop, from its next start; again: done",
          "undo": "The last layer or the last take off", "delete": "Held + a loop: deleted", "copy": "Held + a loop: copied; an empty pad takes it",
          "jog": "The chosen loop", "back": "Leaves the looper (the loops play on)", "track4": "Track 4 held + pad 4 opened this",
          "play": "Loops start and stop with the transport", "sample": "Records the next empty loop of the chosen column"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. Shift + Sample: the recorder's menu", "A Shift menu like the others: a row a knob, the jog selects, its click edits.",
         "menu", S("Recorder", "Source, Mode, Length; three rows show, the list scrolls"), MENU,
         "Rows: Source, Mode, Length, Level, Monitor, Looper. Track 4 held + Sample opens it too."),
        ("B. The menu: Level", "The threshold on the source's level now.", "level", S("Recorder", "The level, the threshold's mark"), MENU,
         "Threshold mode starts the take when the level passes the mark."),
        ("C. Sample pressed: armed", "One line on top; the view under it stays. Sample blinks red until the bar line.",
         "armed", S("Top line", "The source, the bars to record"), REC, "Bars mode, 4 bars, slot 2."),
        ("D. Recording: the top line (option 1)", "The bars done / wanted. Nothing else changes: you keep playing in the view you were in.",
         "line", S("Top line", "The source, bars done / wanted"), REC, "My recommendation."),
        ("E. Recording: the wave (option 2)", "The take's levels fill the screen, as the Sampling screen does today.",
         "wave", S("Wave", "The take, the bar lines, the strip its bars"), REC, "Takes the screen while it records."),
        ("F. Capture", "The last 4 bars are kept. Said for a second; Sample flashes green.", "captured", S("Message", "A second"), REC,
         "Capture listens all the time (Sample dim white). A press keeps what you just played."),
        ("G. The recordings", "Track 4 held + pad 3. 32 takes a bank, the newest first. The chosen one: its length, tempo, wave, firmware and time.",
         "takes", S("A take", "Length, tempo, wave, firmware, time"), TAKES, "Pad colours: the source."),
        ("H. A take plays", "The pad green; the place a line over the wave.", "takes_play", S("A take playing", "Bar now / bars"), TAKES, ""),
        ("I. The looper", "Track 4 held + pad 4. 8 columns of 4 loops; a column plays one loop at a time, in time with the song.",
         "looper", S("A loop", "Its column, the bar now / bars, the wave"), LOOPER, "The same rules as session view's clips."),
        ("J. The looper: an empty pad records", "From the recorder's source, from the next bar; it plays on when its bars are done.",
         "looper_rec", S("A loop recording", "Bars done / wanted"), LOOPER, ""),
        ("K. The looper: overdub", "Record adds a layer to the chosen loop.", "looper_dub", S("Overdub", "The layers so far"), LOOPER,
         "Undo takes the last layer off."),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL (not built): the recorder, the recordings, the looper (2026-11-24)", "color": "#ffb000"}]
    boards = []
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
        boards.append({"id": name, "title": title, "desc": desc, "note": nt, "screen": screen(name)[0], "leds": leds(name)})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: recorder and looper, proposal 1",
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
