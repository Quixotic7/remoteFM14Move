#!/usr/bin/env python3
"""RemoteFM14Move: rfm1cs-abletonmove 0.13.0 (projects that hold their firmwares, States, the Back shortcuts, the
FM-1 knob page, the Loop page, the menus' jog: the owner's answers and messages of 2026-11-22), as an Ableton Move
Designer document (format 'ableton-move-designer' v3). Drawn from the build: the screens and the lights are the
core's own, written by its test (tests/move_sessview.c: sessview_<name>.pbm and .leds in its build directory).
    python3 make_ux_013.py <the core's build directory> <out.movedesign.json>"""
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


def screen(name):
    tok = open(os.path.join(SHOTS, f"sessview_{name}.pbm")).read().split("\n", 2)[2]
    bits = [int(ch) for ch in tok if ch in "01"]
    b = bytearray(1024)
    for y in range(64):
        for x in range(128):
            if bits[y * 128 + x]:
                b[y * 16 + x // 8] |= 0x80 >> (x % 8)
    return base64.b64encode(bytes(b)).decode(), bits


def leds(name):
    """the pads (the top row first), the 16 steps, the 8 knobs, the 16 icons (white), the 4 Track buttons"""
    rows = [ln.split() for ln in open(os.path.join(SHOTS, f"sessview_{name}.leds")).read().strip().split("\n")]
    col = lambda h: None if h == "000000" else "#" + h
    return ([[col(h) for h in r] for r in rows[:4]], [col(h) for h in rows[4]], [col(h) for h in rows[5]],
            [int(h, 16) / 127 for h in rows[6]], [col(h) for h in rows[7]])


def m(**k):
    return {"meta": k}


def controls(name, t):
    """t: the layout's own words for its pads, steps, knobs, jog and buttons"""
    pads, steps, rings, icons, tracks = leds(name)
    c = {}
    for i in range(4):
        c[f"track-{i + 1}"] = {**m(label="SEQ" if i == 3 else f"FW {i + 1}", function=t.get("track", "A firmware's slot: orange, blue, yellow")),
                               "led": {"color": tracks[i]}}
    c["menu"] = m(label="SESSION", function=t.get("menu", "Flips to session view and back. Never closes a menu"))
    c["back"] = m(label="BACK", function=t.get("back", "A quick click closes the latest layer. Its light is on only when there is one"))
    c["shift"] = m(label="SHIFT", function=t.get("shift", "The Shift layer"))
    c["play"] = m(label="", function=t.get("play", "Start / stop the sequencer"))
    c["wheel"] = m(label="", function=t.get("jog", "Nothing here"))
    c["volume"] = m(label="MASTER", function=t.get("volume", "The master volume"))
    for b in ("delete", "copy", "mute", "undo", "left", "right", "up", "down", "capture", "sample", "loop", "record"):
        c[b] = m(label="", function=t.get(b, "As in the layer under this one"))
    for k in range(8):
        c[f"knob-{k + 1}"] = m(label="", function=t.get(f"knob{k + 1}", t.get("knob", "Nothing here (dark)")))
        c[f"knobled-{k + 1}"] = {"led": {"color": rings[k]}}
    for r in range(4):
        for k in range(8):
            c[f"pad-{r + 1}-{k + 1}"] = {**m(label="", function=t.get("pad8" if k == 7 and "pad8" in t else "pad", "")),
                                         "led": {"color": pads[r][k]}}
    for i in range(16):
        c[f"step-{i + 1}"] = {**m(label="", function=t.get("step", "")), "led": {"color": steps[i]}}
        c[f"stepicon-{i + 1}"] = {"led": {"brightness": round(icons[i], 2)}}
    return c


def layout(x, y, title, desc, name, smeta, t):
    ctl = controls(name, t)
    ctl["screen"] = {"meta": smeta}
    return {"id": uid(), "x": x, "y": y, "w": 1048, "h": 616, "type": "layout", "name": title, "description": desc,
            "scope": ALL, "controls": ctl, "screen": screen(name)[0]}


PROJ = {"pad": "A project, in its colour (white: the open one). A press opens it: its firmwares load. Held: what is in it. An empty pad: a new project (three empty slots, the default routing)",
        "knob1": "A held project's colour", "copy": "Held + a project, then an empty pad: a copy with its firmwares and their states",
        "delete": "Held + a project: cut", "track": "Lights in its slot's colour when its firmware is ready"}
STATES = {"pad": "A state of this firmware. Stored (dim): a press loads it. Empty (dark): a press saves there. The active one (full) saves itself. Copy held: copy / paste. Delete held: deleted, into the buffer",
          "step": "Dark and dead", "step14": "New: the firmware restarts as first installed (icon lit). Undo brings the old state back",
          "knob": "Dark and dead", "jog": "Dead", "back": "Closes States, back to where it was opened",
          "undo": "Lit after New: what the firmware held is back", "copy": "Held: copy a state, paste into a pad", "delete": "Held + a state: deleted (an X while held)",
          "track": "A tap: that slot's States"}
MENU = {"pad": "The layout menu's scale and root", "pad17": "States: lit in the slot's colour with a firmware loaded", "pad18": "Latch",
        "step": "The 16 tracks: a click selects at the press, a double click solos, a hold routes", "knob": "CC numbers", "jog": "Root",
        "volume": "Held Track + volume: the slot's level, shown", "track": "Held: its menu"}
TRACK = {"pad": "The track's notes", "step": "The clip's steps", "knob": "The knob page: TRIG, FM-1, CC banks", "jog": "Turn: the knob page. On the FM-1 page a click: Knobs / Knobs + Keys",
         "back": "Held: shortcuts with Left, Up, Loop, Play, Delete, Menu", "left": "Back + Left: bar 1", "loop": "Back + Loop: the whole clip", "play": "Back + Play: to bar 1",
         "delete": "Back + Delete: every layer closed"}
SESS = dict(TRACK, pad="A clip slot. A double click on the playing clip stops it now", pad8="A scene", step="Odd: select the column's track. Even: stop it",
            knob="Touched: the column's track", up="Back + Up: scene 1", left="Back + Left: track 1",
            delete="Held + a clip: deleted, an X only while held")
FWP1 = dict(TRACK, knob="The firmware's knobs (SELECT, PRESETS, ALGO, -, K1..K4), lit where they have a job", jog="Turn: the page. Click: Knobs + Keys")
FWP2 = dict(FWP1, step="The FM-1's keys (the firmware's lights)", jog="Turn: the page (gives the steps back). Click: Knobs",
            capture="FX", sample="SEL", mute="ENV", delete="LFO", copy="EDIT", shift="GLO", menu="HOME", undo="SAVE", loop="ARP", play="PLAY", record="REC", left="OCT-", right="OCT+",
            up="Ours: the pads' octave", down="Ours: the pads' octave")
LOOP = dict(TRACK, pad="The two lower rows: the loop's last bar's length, 1..16 steps (upper row 1-8, bottom row 9-16). The upper rows dark, dead",
            step="The 16 bars. A tap shows one; a double click makes it the loop; one held + another: the range; one held + the jog: its notes' length",
            jog="The loop's length, a step a detent (Shift: a bar). A bar's step held: every note's length in that bar", loop="Held, or latched by a quick click")
SMENU = dict(TRACK, knob="Dark and dead", knob1="The menu's knob: turned, its value; the cursor moves to it", knob2="The menu's knob", knob3="The menu's knob", knob4="The menu's knob",
             jog="Turn: selects a parameter. Click: edit it (the turn changes it). Click: back to selecting", back="Closes the menu, one press")


def main():
    DX, DY, COLS = 1148, 920, 5
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. A project pressed: it loads", "Its number and its three firmwares, a bar each; a Track button lights when its firmware is ready.", "ps_proj_loading", S("Loading", "A bar a firmware; dotted: waiting"), PROJ, "The sound stops while firmwares load."),
        ("B. A firmware not on this Move", "Said for a second; the slot stays empty, the project keeps its name and state.", "ps_proj_missing", S("Message", "A second"), PROJ, ""),
        ("C. A project held", "Number, tempo, clips and its three firmwares.", "ps_proj_held", S("Project", "While held"), PROJ, ""),
        ("D. A new project in session view", "Tracks 1-4 to firmware 1, 5-8 to firmware 2, 9-12 to firmware 3 (channels 1-4 a group); 13-16 to none.", "ps_newproj_session", S("Session", "The default routing"), SESS, "The Track buttons select tracks 1, 5, 9."),
        ("E. Track 2 held: pad 7 is States", "The layout menu with pad 7 lit in the slot's colour.", "ps_trk_menu", S("Slot summary", "While held"), MENU, ""),
        ("F. States opening", "The name across the screen and a sweep over the pads.", "ps_states_open", S("States", "A second"), STATES, ""),
        ("G. States", "Framed: stored. Filled: the active one, which saves itself. A dot: empty. Step 14's icon: New.", "ps_states", S("States", "ChoralRoot"), STATES, ""),
        ("H. States: Copy held", "State 1 copied (dashed); the next pad gets it.", "ps_states_copied", S("States", "The copy icon"), STATES, ""),
        ("I. States: Delete held", "State 8 deleted: an X only while Delete is held.", "ps_states_deleted", S("States", "The scissors"), STATES, ""),
        ("J. Step 14: a new state", "The firmware restarts as first installed. No stored state is touched.", "ps_state_new", S("Message", "A second"), STATES, ""),
        ("K. Undo after it", "What the firmware held is back, and the state that was active.", "ps_state_undo", S("Message", "A second"), STATES, ""),
        ("L. Track 2 held + the volume knob", "The slot's level over the Track's menu, in every layout.", "trk_volume", S("Level", "At the touch and on every turn"), MENU, ""),
        ("M. A held Track: a step pressed", "Selected at the press.", "fwp_tk_press", S("Slot summary", "Track 7, Selected"), MENU, ""),
        ("N. A Track tap: the flash", "For 600 ms the steps show the slot's 16 tracks, the selected one blinking.", "fwp_trackflash", S("Layout popup", "A second"), TRACK, ""),
        ("O. The FM-1 knob page", "Second after TRIG. The knobs are the firmware's.", "fwp_page", S("Page popup", "Knobs / FM-1"), FWP1, ""),
        ("P. The FM-1 page: a knob turned", "The firmware's name and value.", "fwp_l1_value", S("Knob popup", "Cutoff"), FWP1, ""),
        ("Q. The jog's click", "Knobs + Keys, named a second.", "fwp_level2_pop", S("Level popup", "A second"), FWP2, ""),
        ("R. Knobs + Keys", "The steps and these buttons are the FM-1's; the pads stay the note layout; the screen is the FM-1's.", "fwp_level2", S("The FM-1's screen", ""), FWP2, "The jog stays ours."),
        ("S. Back held, track view", "Its shortcuts light; the other buttons are dark and dead.", "back_held_track", S("Track view", "Back held"), TRACK, "The white buttons' lights are not in this picture (the dump does not carry them)."),
        ("T. Back + Left", "Bar 1.", "back_bar1", S("Popup", "A second"), TRACK, ""),
        ("U. Back + Loop", "The loop is the whole clip.", "back_loop", S("Popup", "A second"), TRACK, ""),
        ("V. Back + Play", "To bar 1; never starts or stops.", "back_play", S("Popup", "A second"), TRACK, ""),
        ("W. Back + Delete", "Every layer closed.", "back_closed", S("Popup", "A second"), TRACK, ""),
        ("X. Session view: after a delete", "No X left once Delete is let go.", "x_gone", S("Session", "The slot is empty"), SESS, ""),
        ("Y. Loop: 5 steps", "The length popup with the bar's 16 steps; the step lights show the same; the pads up to the length.", "loop_steps", S("Loop length", "Cells: in the loop. Dots: after its end"), LOOP, ""),
        ("Z. Loop: 1 bar + 8", "Pad 8 with two bars.", "loop_steps_2bars", S("Loop length", "1 Bar +8"), LOOP, ""),
        ("AA. Loop: a bar held + the jog", "Every note's length in that bar.", "mj_page_len", S("Bar length", "The change so far"), LOOP, ""),
        ("AB. Loop: a double click", "That bar alone is the loop.", "mj_loop_dbl", S("Loop page", "Bar 8"), LOOP, ""),
        ("AC. Tempo: the jog selects", "The cursor is the long tick; the header names the parameter.", "mj_tempo_sel", S("Tempo", "Selecting"), SMENU, ""),
        ("AD. Tempo: editing", "After a click: the value between arrows; the turn changes it.", "mj_tempo_edit", S("Tempo", "Editing"), SMENU, ""),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "0.13.0: projects, States, Back, the FM-1 knob page, the Loop page, the menus' jog (2026-11-22)", "color": "#2ad17a"}]
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move 0.13.0", "items": items}, open(OUT, "w"), indent=1)
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
