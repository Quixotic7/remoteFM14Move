#!/usr/bin/env python3
"""RemoteFM14Move: the owner's feedback of 2026-11-21 on the arp, the held Track buttons and session view
(rfm1cs-abletonmove 0.11.0, design/UX-RULES.md), as an Ableton Move Designer document (format 'ableton-move-designer'
v3). The owner had greenlit these, so this board is drawn from the build, not before it: the screens and the lights
are the core's own, written by its test (tests/move_sessview.c: sessview_<name>.pbm and .leds in its build directory).
    python3 make_ux_011.py <the core's build directory> <out.movedesign.json>"""
import base64, json, os, random, sys

SHOTS, OUT = sys.argv[1], sys.argv[2]
random.seed(20261122)
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
    """t: the layout's own words for its pads, steps, knobs, jog, Menu, Back and the Track buttons"""
    pads, steps, rings, icons, tracks = leds(name)
    c = {}
    for i in range(4):
        c[f"track-{i + 1}"] = {**m(label="SEQ" if i == 3 else f"FW {i + 1}", function=t.get("track", "A firmware's slot")),
                               "led": {"color": tracks[i]}}
    c["menu"] = m(label="SESSION", function=t.get("menu", "Track view <-> session view. An open Shift menu stays: the view "
                                                  "flips under it, named a second"))
    c["back"] = m(label="BACK", function=t.get("back", "Closes the latest layer"))
    c["shift"] = m(label="SHIFT", function=t.get("shift", "The Shift layer"))
    c["play"] = m(label="", function="Start / stop the sequencer")
    c["wheel"] = m(label="", function=t.get("jog", "Nothing here"))
    c["volume"] = m(label="MASTER", function="The master volume")
    for b in ("delete", "copy", "mute", "undo", "left", "right", "up", "down", "capture", "sample", "loop", "record"):
        c[b] = m(label="", function=t.get(b, "As in the layer under this one"))
    for k in range(8):
        c[f"knob-{k + 1}"] = m(label="", function=t.get("knob8" if k == 7 and "knob8" in t else "knob", "Nothing here (dark)"))
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


SESS = {"pad": "A clip slot: a click launches it at the next bar (a double click: now); an empty slot: its track stops",
        "pad8": "A scene: a click launches the row (stopped: it plays); a double click: now; Shift + it: the scene stops",
        "step": "Odd: select the column's track. Even: stop it (its icon lit: a function). 16: stop every clip",
        "knob": "Touched: the column's track, its popup. Lit: the firmware's colour = its track reaches the firmware played "
                "now; faint white = it does not",
        "knob8": "Touched: 'Scenes'. Green: the scene launcher",
        "track": "A tap: that firmware is the one played, session view stays. Held: the pads are the routing"}
ROUTE = {"pad": "A column = a track. A click: routed to this firmware or not (acts once the double click's time has passed). "
                "A double click: its solo here. Lit: it reaches the firmware. White: pressed",
         "pad8": "Dark: nothing while the Track is held",
         "step": "The 16 tracks: a click selects, a double click solos, a hold routes or not. Dark: not routed. Blinking: "
                 "the selected one. Blue: the solo",
         "knob": "Dark: nothing in session view", "track": "Held: this firmware's routing"}
HOLD = {"pad": "The layout menu: top row the layouts, pad 8 latch, the rest the scales",
        "step": "The 16 tracks: a click selects (after the double click's 300 ms), a double click solos (at once), a hold "
                "routes or not (at 300 ms, while held). Dark: not routed. Blinking: selected. Blue: solo. White: pressed",
        "knob": "The selected track's CC numbers (lit)", "jog": "The root", "up": "Octave up", "down": "Octave down",
        "track": "Held: its menu"}
ARP = {"pad": "The track's pads, through the arp (not taken by the menu)",
       "step": "The clip's steps (the main page does not use them)",
       "knob": "Pattern, Rate, Octaves, Chance, Mode, Gate, Spread, Sync. Touched: its name and value in the header",
       "jog": "Turn: the pages (Arp, Mod, Transpose). Click: the arp off / on", "back": "Closes the arp menu",
       "menu": "The view flips under the menu (named a second); the menu stays"}
LANE = dict(ARP, step="The lane's 16 steps: a press picks one. White: picked. Green: just played. Orange: does something "
                       "else than play. Dark: past the lane's length. Icons dark: these are steps",
            knob="1: the picked step's function (or semitones). 2: the lane's length. 3: Reset. 4-8: dark, dead")


def main():
    DX, DY = 1148, 920
    pos = lambda k: (60 + (k % 4) * DX, 60 + (k // 4) * DY)
    S = lambda lab, fn: {"label": lab, "function": fn}
    L = [
        ("A. Session: a Track tapped", "Track 1 tapped in session view: ChoralRoot is the firmware played now and the view "
         "stays. Its summary a second: firmware, layout, key, and the 16 tracks (a block: routed to it; underlined: the "
         "one selected for it).", "slot", S("Slot summary", "A second, then the grid again"), SESS,
         "Session view is its own layer. After the popup the knob lights show the routing: a column whose track reaches "
         "this firmware glows in the firmware's colour."),
        ("B. Session: a Track held", "Track 2 held: the pads are the routing for Felucca. A lit column reaches it. The scene "
         "column and the knobs are dark: nothing passes to the clips.", "route_hold",
         S("Slot summary", "While the Track is held"), ROUTE,
         "A click toggles a column (after the double click's 300 ms, its pad white meanwhile); a double click solos it."),
        ("C. Session: a column clicked", "Column 3 clicked: track 3 no longer reaches Felucca: its column and its step are "
         "dark, the screen says 'Track 3 / Not Routed', a dot in the row of 16.", "route_off",
         S("A track acted", "The track and what happened, over the 16 tracks"), ROUTE,
         "Not routed = no light (it was dim red)."),
        ("D. Session: the handover", "Track 1's clip 2 launched over its playing clip 1: clip 1 flashes with a stop icon "
         "(its pad between green and its own colour), clip 2 flashes with the play icon (its pad blinks green).", "handover",
         S("Session", "Stop square: it stops at the bar. Play icon flashing: it waits"), SESS,
         "A double click on a clip launches it now instead of at the bar."),
        ("E. Session: a scene waits", "Scene 3 launched while scene 1 plays: scene 3's box flashes a play icon, scene 1's a "
         "stop icon; every clip of row 1 shows its stop square.", "scenequeue",
         S("Session", "The scene column: play icon = on its way, stop icon = on its way out"), SESS,
         "Stopped, a scene pad starts playback. Shift + a scene pad stops it (a double click: at once)."),
        ("F. Session: knob 8 touched", "Knob 8 touched: 'Scenes', the scene that plays or waits, the rows in view.", "scenes",
         S("Scenes popup", "While knob 8 is touched"), SESS, "Knob 8's light is green."),
        ("G. Track held: a step held", "Track view, Track 2 held, step 7 held 300 ms: track 7 is not routed, while the step "
         "is still down: its light goes dark and the screen says so. The release does nothing.", "hold_notrouted",
         S("A track acted", "Felucca / Track 7 / Not Routed, over the 16 tracks"), HOLD,
         "The hold used to act only at the release."),
        ("H. Track held: at rest, a solo", "The held Track's screen: the firmware, its layout (a padlock: latch on), the "
         "key, the 16 tracks. Here track 9 is soloed (a bar over its block) and track 10 was selected after: the solo "
         "stays.", "hold", S("Slot summary", "Layout, latch, key, the tracks routed"), HOLD,
         "A solo no longer ends when another track is selected."),
        ("I. Arp: at rest", "Shift + step 11. The header says 'Arp Off'; the ticks under it are the knobs in use; the walk "
         "is hollow while the arp is off.", "arp_off", S("Arp", "Header, knob ticks, the walk, pattern and rate"), ARP,
         "The jog's click turns the arp on and off."),
        ("J. Arp: a knob touched", "Knob 6 touched and turned: the header reads 'Gate 95%', its tick is long. A second "
         "after the release the header is the page's again.", "arp_gate", S("Arp", "The header names the knob and its value"), ARP,
         "All eight knobs are lit in the menu colour."),
        ("K. Arp: the Mod lane", "The step keys are the lane's 16 steps. Each step shows what it does: play, rest, tie, "
         "repeat, lowest, highest, power chord, chord, a played note's number. Past the length: dots.", "arp_mod",
         S("Mod lane", "A sign a step; the picked one underlined"), LANE,
         "Knobs 1-3 only; the others are dark and dead."),
        ("L. Arp: the Transpose lane", "The same keys: bars up and down from the line, the picked step's semitones in the "
         "header.", "arp_trans", S("Transpose lane", "Bars from the line"), LANE, ""),
        ("M. Arp: Menu pressed", "Menu flips track view / session view under the arp, named a second; the arp menu stays "
         "until Back.", "arp_view", S("View popup", "A second, then the arp again"), ARP, ""),
    ]
    items = [{"id": uid(), "x": 0, "y": 0, "w": 4 * DX + 60, "h": 4 * DY + 100, "type": "group",
              "title": "0.11.0: the arp, the held Track buttons and session view after the owner's feedback (2026-11-21)",
              "color": "#2ad17a"}]
    for k, (title, desc, name, smeta, t, nt) in enumerate(L):
        x, y = pos(k)
        items.append(layout(x, y, title, desc, name, smeta, t))
        if nt:
            items.append({"id": uid(), "x": x, "y": y + 640, "w": 1048, "h": 160, "type": "note", "text": nt, "color": "#cfe3ff"})
    json.dump({"format": "ableton-move-designer", "version": 3, "name": "RemoteFM1 for the Move 0.11.0: arp, Track hold, session",
               "items": items}, open(OUT, "w"), indent=1)
    try:
        from PIL import Image
        sh = Image.new("RGB", (4 * 522, 4 * 266), (50, 50, 50))
        for i, (_, _, name, *_r) in enumerate(L):
            bits = screen(name)[1]
            im = Image.new("RGB", (128, 64), (0, 0, 0))
            im.putdata([(255, 255, 255) if b else (0, 0, 0) for b in bits])
            sh.paste(im.resize((512, 256), Image.NEAREST), ((i % 4) * 522 + 5, (i // 4) * 266 + 5))
        sh.save(OUT.replace(".movedesign.json", "-screens.png"))
    except ImportError:
        pass


main()
