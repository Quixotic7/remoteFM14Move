#!/usr/bin/env python3
"""RemoteFM14Move: the recorder, the recordings and the looper (the owner's request of 2026-11-24), as an Ableton Move
Designer document (format 'ableton-move-designer' v3). A PROPOSAL, round 2 (the sampler as an app, the kit on Track 4), drawn before any code: the screens and lights are
mock_recorder_v2.c's (rec2_<name>.pbm and .leds in its working directory).
    python3 make_recorder_v2.py <the mock's directory> <out.movedesign.json> [<boards.json for the review page>]"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(202611242)
uid = lambda: "".join(random.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(12))

ALL = (["screen"] + [f"knob-{i}" for i in range(1, 9)] + [f"knobled-{i}" for i in range(1, 9)] +
       ["volume", "wheel", "back", "menu"] + [f"track-{i}" for i in range(1, 5)] +
       [f"pad-{r}-{c}" for r in range(1, 5) for c in range(1, 9)] +
       ["capture", "sample", "loop", "mute", "delete", "copy", "undo", "shift", "play", "record"] +
       [f"step-{i}" for i in range(1, 17)] + [f"stepicon-{i}" for i in range(1, 17)] +
       ["left", "right", "up", "down", "power"])
BTN = ["sample", "record", "play", "loop", "mute", "delete", "copy", "undo"]


def screen(name):
    tok = open(os.path.join(SHOTS, f"rec2_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons, the 4 Track buttons, the 8 buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"rec2_{name}.leds")).read().strip().split("\n")]
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
       "jog": "Turn: the next setting. Click: edit it; turn changes it; click or Back: done",
       "back": "Editing: done. Else: closes the app", "left": "The Record tab", "right": "The Recordings tab",
       "track": "Held + Sample: the app opens with that firmware as the source (nothing records yet)"}
TAKES = {**APP, "jog": "Turn: the next / previous take. Click: plays it, again stops it",
         "delete": "Held + the jog's click: the take deleted (Undo: back)", "copy": "Held + the jog's click: copied; an empty loop or a kit pad takes it"}
KIT = {"pad": "The kit's 16 pads, bottom-left 4 x 4. A press plays its sample and selects it (white). Dark: empty. The right half: dark",
       "step": "The kit's track: its steps", "knob": "The chosen pad: Level, Pan, Tune, Start, Length, Decay, Send, Mode",
       "jog": "Click: pick the chosen pad's sample from a list", "track4": "Tap: the kit. Held: its menu, the Mixer (pad 1), Routing (pad 2)",
       "sample": "Opens the sampler", "back": "As everywhere"}
PICK = {**KIT, "jog": "Turn: the next sample, heard at once. Click: loaded into the pad. Back: the pad as it was",
        "left": "The folder above", "right": "Into a folder"}


def main():
    DX, DY, COLS = 1148, 920, 4
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. The sampler app: Record", "It stays until Back. The source's sound now as a wave and a level; the jog on a setting, its value in the bottom row.",
         "app", S("Sampler, Record", "Source, the tabs' marks, wave, level with the threshold, the settings' places, the chosen value"), APP,
         "The app takes the screen, the jog and Back. Pads, steps and knobs stay with the layer under it."),
        ("B. The jog's click: editing", "The value framed between arrows; the jog changes it; a click or Back: done.", "app_edit",
         S("Sampler, Record", "Length edited"), APP, "Settings: Source, Mode, Length, Threshold, Input level, Monitor, To."),
        ("C. The input and its level", "Source Input: the level is set in the app, on the meter.", "app_input",
         S("Sampler, Record", "Input level edited"), APP, "A gain in software, -24 .. +24 dB."),
        ("D. Sample: recording", "The take as a wave, bar lines, bars done / wanted.", "app_rec", S("Sampler, recording", "The take, its bars"), APP, ""),
        ("E. The Recordings tab", "One take a jog detent: its number, length, tempo, wave, firmware and time. The jog's click plays it.",
         "app_takes", S("Sampler, Recordings", "A take"), TAKES, "Right arrow from Record; Left goes back."),
        ("F. The kit on Track 4", "16 pads, a sample each. The chosen pad: its sample with start and end.", "kit",
         S("Kit", "The chosen pad's sample"), KIT, "Pink is the kit's colour here."),
        ("G. A pad's sample picked from a list", "The jog scrolls the samples, each heard as it is passed; a click loads it.", "kit_pick",
         S("Kit, a pad's sample", "The list"), PICK, "Nothing is mapped to pads by itself."),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "PROPOSAL, round 2 (not built): the sampler app, the kit on Track 4 (2026-11-24)", "color": "#ffb000"}]
    boards = []
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
        boards.append({"id": name, "title": title, "desc": desc, "note": nt, "screen": screen(name)[0], "leds": leds(name)})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move: sampler app and kit, proposal 2",
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
