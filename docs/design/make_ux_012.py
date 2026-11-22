#!/usr/bin/env python3
"""RemoteFM14Move: the owner's answers of 2026-11-22 on the review page (rfm1cs-abletonmove 0.12.0), as an Ableton
Move Designer document (format 'ableton-move-designer' v3). The items were agreed on the page first, so this board is
drawn from the build: the screens and the lights are the core's own, written by its test (tests/move_sessview.c:
sessview_<name>.pbm and .leds in its build directory).
    python3 make_ux_012.py <the core's build directory> <out.movedesign.json>"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(20261123)
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


SESS = {"pad": "A clip slot, in the colour of the slot its track reaches (grey: none). A column that does not reach the "
               "firmware played is dimmed. A click launches by the Launch setting; a double click: now",
        "pad8": "A scene: a click launches the row; Shift + it: the scene stops. Copy / Delete held: copy, paste, delete the row",
        "step": "Odd: select the column's track (held: its clip's settings). Even: stop it. 16: stop every clip. No icon lit at rest",
        "knob": "Touched: the column's track. Lit in the slot's colour when its track reaches the firmware played",
        "knob8": "Touched: 'Scenes'", "left": "A track left; Shift: 7", "right": "A track right; Shift: 7",
        "up": "A row up; Shift: 4", "down": "A row down; Shift: 4",
        "delete": "Held + a clip or a scene pad: deleted, into the buffer. Held + Play: the clip cleared. A quick click: nothing",
        "copy": "Held + a clip or scene: copied, or pasted into an empty one; replaces after a copy or a delete. A quick click: nothing",
        "menu": "Back to the played slot's own layout. Shift + Menu, Back + Menu, Track held + Menu get here from anywhere",
        "track": "A tap: that firmware is the one played, the view stays. Held: the pads are the routing"}
CLIPSET = dict(SESS, knob="Dark and dead while the select step is held", knob1="Launch: Global, Instant, 1 Beat, 1 Bar, 2 Bars, 4 Bars, Clip",
               knob2="After: Loop, Next", volume="The level of the firmware this track plays",
               step="The held select step (white). The others as in session view")
TRACK = {"pad": "The track's notes", "step": "The clip's steps", "knob": "TRIG or a CC bank", "jog": "The knob page",
         "up": "Octave up: a popup and a sweep", "down": "Octave down: a popup and a sweep",
         "delete": "Held: scissors on the screen. + Play: the clip cleared", "copy": "Held: two sheets on the screen",
         "loop": "A quick click sticks; held: momentary", "mute": "A quick click sticks; held: momentary",
         "undo": "Nothing to undo: 'None'"}
ARP = dict(TRACK, pad="The track's pads, through the arp",
           knob="Pattern, Rate, Octaves, Chance, Mode, Gate, Spread, Sync. Touched: its name and value in the header",
           jog="Turn: the pages (Arp, Mod, Transpose). Click: the arp off / on, named across the screen", back="Closes the arp menu")
LANE = dict(ARP, step="The lane's 16 steps: a press picks one; held: the pads are its shortcuts. Step 1 held + step n: the length",
            knob="Dark, dead", knob1="The picked step's function (or semitones)", knob2="The lane's length", knob3="Reset")
LANEPAD = dict(LANE, pad="The held step's shortcuts. White: its value now. Dark pads are dead; none plays")
MENU = dict(TRACK, knob="Dark and dead", knob1="The menu's knob: touched, its name and value in the header",
            knob2="The menu's knob", knob3="The menu's knob", knob4="The menu's knob", jog="The menu's value", back="Closes the menu")
T4 = {"pad": "Pad 1: Mixer. Pad 2: Routing. The rest dark and dead", "step": "The Shift shortcuts (icons lit)",
      "knob": "Dark and dead", "jog": "Dead", "track": "Track 4 held: its layer, all at once at 300 ms",
      "up": "Dead", "down": "Dead", "left": "Dead", "right": "Dead", "menu": "Dead", "copy": "Dead", "delete": "Dead", "mute": "Dead"}
MUTE = dict(TRACK, step="The 16 tracks' mutes (icons dark: these are tracks)", mute="Latched by a quick click; again, or Back, ends it",
            back="Closes the Mute layer")
BROWSER = {"pad": "A firmware, in its own colour", "step": "A filter: its icon is lit only when the filter is on. White at the press",
           "knob": "As the browser page", "jog": "The list"}


def main():
    DX, DY, COLS = 1148, 920, 5
    pos = lambda k: (60 + (k % COLS) * DX, 60 + (k // COLS) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. Session: a colour a slot", "Columns take the colour of the slot their track reaches: orange slot 1, blue "
         "slot 2, yellow slot 3, grey none. Solo is purple now.", "slotcols", S("Session", "The grid"), SESS,
         "The 16 track colours are gone from session view; track view keeps them."),
        ("B. Session: columns not routed", "Columns 2 and 5 do not reach the firmware played: their pads at a quarter, "
         "their cells dotted on the screen.", "routedcols", S("Session", "Dotted outline: not routed to the firmware played"), SESS, ""),
        ("C. Session: the stop icon", "The clip on its way out: a filled cell with a black square, flashing against the "
         "waiting clip's play icon. Recording is a filled cell with a black dot.", "handover", S("Session", "The handover"), SESS, ""),
        ("D. Session: Delete held", "Scissors in the scene column while Delete is held. Delete + a clip: gone, into the "
         "buffer. Delete never pastes.", "delete_held", S("Session", "Scissors: cutting"), SESS,
         "Delete held + Play clears the selected clip (Shift + step 13 is gone)."),
        ("E. Session: Copy held, a clip copied", "Two sheets while Copy is held; the copied clip dashed. The next press on "
         "a clip replaces it; on an empty slot it pastes.", "copied", S("Session", "Two sheets: copying"), SESS, ""),
        ("F. Session: a scene copied", "Copy held + scene pad 1: the row copied into the scene buffer.", "scene_copied",
         S("Session", "The row dashed"), SESS, "An empty row then gets it; a row with clips is replaced after a copy or a delete."),
        ("G. Session: a scene deleted", "Delete held + scene pad 1: the row gone, into the scene buffer. One undo.",
         "scene_deleted", S("Session", "The row crossed"), SESS, ""),
        ("H. Global settings: Launch", "The new row: Instant, 1 Beat, 1 Bar, 2 Bars, 4 Bars, Clip.", "global_launch",
         S("Global settings", "The jog moves through the rows"), {"jog": "The rows; click edits", "back": "Closes it"}, ""),
        ("I. Session: a select step held", "Held 300 ms (or a knob touched while down): the selected clip's Launch on knob "
         "1, After on knob 2. Knobs 3-8 dark and dead.", "clipset_launch", S("Clip settings", "Knob 1 touched"), CLIPSET,
         "A quick click only selects."),
        ("J. Session: step held + volume", "The volume knob is the level of the firmware this track plays.", "clipset_level",
         S("Level", "The firmware named"), CLIPSET, ""),
        ("K. Session view from the FM-1 layout", "Track 1 held + Menu in the FM-1 layout (or Back + Menu anywhere): session "
         "view, named a second. The Track's own menu does not open.", "from_fm1", S("View popup", "A second"), SESS,
         "Back never leaves session view."),
        ("L. Arp: turned off", "Shift + step 11 held 300 ms, or the jog's click in the menu: the arp off, named across "
         "the screen for a second.", "arp_toggle_offpop", S("Arp popup", "A second"), ARP,
         "Shift + step 11 quick: the menu opens at the release. Held: toggles, the menu does not open."),
        ("M. Arp: off, dimmed", "While the arp is off the menu's pictures are dimmed.", "arp_offdim", S("Arp", "The walk dimmed"), ARP, ""),
        ("N. Arp lanes: the wave", "Entering the Mod or Transpose page: the 16 steps flash white and settle left to right "
         "(about 360 ms). A press during it works at once.", "arp_wave2", S("Mod lane", "The lane"), LANE, ""),
        ("O. Arp Mod: a step held", "Step 5 held: the pads are its functions. Bottom row: play, rest, tie, repeat, lowest, "
         "highest, power chord, chord. The row above: notes 1-6. White: its function now.", "arp_modpads",
         S("Mod lane", "The header: 5 Tie"), LANEPAD, "A quick click on the step only picks it."),
        ("P. Arp Transpose: a step held", "Step 3 held: the pads are semitones, -16 bottom left to +15 top right; 0 and the "
         "octaves marked.", "arp_transpads", S("Transpose lane", "The header: 3 +7"), LANEPAD, ""),
        ("Q. Arp lanes: step 1 + step 12", "Step 1 held + step 12: the lane is 12 long. The same again: 1.", "arp_len",
         S("Mod lane", "The header: Length 12"), LANE, ""),
        ("R. Octave up", "Up in track view: the octave, the pads' note range, and a white row sweeps up the pads.",
         "octave_pop", S("Octave popup", "A second"), TRACK, ""),
        ("S. A held step transposed", "A step held, Up / Down: its notes by name after the move.", "held_transpose",
         S("Transpose popup", "The notes"), TRACK, "Held step + a later step: the length; the same again: 1 step."),
        ("T. Fill", "Shift + Loop held: Loop's light blinks fast, the screen says Fill, a wave runs over the pads.",
         "fill", S("Fill", "While held"), TRACK, "59 pad LED writes a second; a pressed pad is white and plays."),
        ("U. A Shift menu: a knob touched", "Tempo, knob 1 touched: its name and value in the header, a tick a knob in "
         "use. Every Shift menu has it now.", "menu_tempo_k1", S("Tempo", "The header names the knob"), MENU, ""),
        ("V. Keys & Scales, redrawn", "Knob 2 touched: 'Scale Maj'. The key row, the layouts and Global / Track under it.",
         "menu_keys_k2", S("Keys & Scales", "The header names the knob"), MENU, "Redrawn to make room for the header."),
        ("W. A menu over the Mixer", "Tempo opened from the Mixer: the menu owns the knob lights, the labels and the jog "
         "there too.", "menu_over_mixer_k1", S("Tempo", "As from track view"), MENU, "It showed the Mixer's lights and popups."),
        ("X. A step held over a menu", "Workflow open, a step held: the step's popup and controls take over; let go: the "
         "menu is back.", "over_menu_step", S("Step popup", "Over the menu, while held"), TRACK, ""),
        ("Y. Loop held over a menu", "Loop's pages over the open menu, while held (or latched after the menu opened).",
         "over_menu_loop", S("Loop", "Over the menu"), TRACK, ""),
        ("Z. Mute latched", "A quick click on Mute: the layer stays. The steps are the 16 tracks' mutes.", "mute_latched",
         S("Mute", "The 16 tracks"), MUTE, "The quick click no longer mutes the selected track."),
        ("AA. Track 4 held", "At 300 ms the whole layer at once: screen, steps, icons, pads 1 and 2. Everything else dark "
         "and dead.", "t4_layer", S("Track 4", "Mixer / Routing"), T4, ""),
        ("AB. Delete + Play", "Delete held + Play: the selected clip cleared; the transport untouched.", "clip_cleared",
         S("Clip Cleared", "A second"), TRACK, ""),
        ("AC. The browser's filters", "A filter step's icon is lit only when the filter is on.", "browser_icons",
         S("Browser", "The firmwares"), BROWSER, ""),
        ("AD. A Track held: slot colours", "Track 2 held in session view: the routing in slot 2's blue.", "route_hold",
         S("Slot summary", "While held"), dict(SESS, pad="A column = a track. A click routes it or not; a double click solos (purple)",
                                               knob="Dark: nothing while the Track is held"), ""),
    ]
    rows = (len(L) + COLS - 1) // COLS
    items = [{"id": uid(), "x": 0, "y": 0, "w": COLS * DX + 60, "h": rows * DY + 100, "type": "group",
              "title": "0.12.0: the owner's answers on the review page (2026-11-22)", "color": "#2ad17a"}]
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move 0.12.0: the review's answers",
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
