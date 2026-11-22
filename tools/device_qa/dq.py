# SPDX-License-Identifier: MPL-2.0
"""On-device QA from the Mac: drives the host that runs ON the Move through Move Remote's test hook, and brings
back its screen and status. See README.md here. Nothing in this file installs, pushes or changes the owner's
RemoteFM1 folder: the QA host runs on a copy (standalone-qa).

    from dq import *
    p = P("name"); p.hold_track(1); p.pad(7); p.shot("states"); p.release_track(1); r = p.run()
    r.status["x"]["states_open"]; sheet("name")
"""
import io, json, os, subprocess, sys, tarfile

HOST = os.environ.get("DQ_HOST", "ableton@move.local")
SSH = ["ssh", "-o", "ConnectTimeout=8", "-o", "BatchMode=yes", "-o", "LogLevel=ERROR", HOST]
QA = "/data/UserData/remotefm1-qa"
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.environ.get("DQ_OUT", os.path.join(HERE, "..", "..", "build", "device-qa"))

CC = dict(jog_click=3, jog=14, track4=40, track3=41, track2=42, track1=43, shift=49, menu=50, back=51, capture=52,
          down=54, up=55, undo=56, loop=58, copy=60, left=62, right=63, volume=79, play=85, rec=86, mute=88,
          sample=118, delete=119)


def pad_note(n):
    """the owner's pad numbers: 1 top left .. 8 top right, 25..32 the bottom row"""
    return 92 - 8 * ((n - 1) // 8) + (n - 1) % 8


class R:
    def __init__(self, d):
        self.dir = d
        self.status = {}
        for f in sorted(os.listdir(d)):
            if f.endswith(".json"):
                self.status[f[:-5]] = json.load(open(os.path.join(d, f)))
        lp = os.path.join(d, "log")
        self.log = open(lp).read() if os.path.exists(lp) else ""

    def s(self, name, key):
        return self.status[name].get(key)

    def pads(self, name):
        h = self.status[name].get("move_pads", "")
        return [h[i * 6:i * 6 + 6] for i in range(32)]   # index 0 = note 68 (bottom left)

    def pad(self, name, n):
        return self.pads(name)[pad_note(n) - 68]


class P:
    def __init__(self, name):
        self.name, self.l = name, []

    # ---- raw
    def raw(self, s): self.l.append(s); return self
    def sleep(self, s): return self.raw("sleep %.3f" % s)
    def shot(self, n): return self.raw("shot " + n)
    def status(self, n): return self.raw("status " + n)
    def waitfor(self, k, v, secs=10): return self.raw("waitfor %s %s %s" % (k, v, secs))
    def both(self, n, settle=0.12): self.sleep(settle); self.shot(n); return self.status(n)

    # ---- buttons
    def down(self, b): return self.raw("cc %d 127" % CC[b]).sleep(0.03)
    def up(self, b): return self.raw("cc %d 0" % CC[b]).sleep(0.03)
    def press(self, b, ms=60): self.down(b); self.sleep(ms / 1000.0); return self.up(b)
    def hold(self, b, ms=380): self.down(b); return self.sleep(ms / 1000.0)
    def dbl(self, b, gap=110): self.press(b, 50); self.sleep(gap / 1000.0); return self.press(b, 50)

    # ---- pads (the owner's numbers) and steps (1..16)
    def pad_down(self, n, vel=100): return self.raw("note %d %d" % (pad_note(n), vel)).sleep(0.03)
    def pad_up(self, n): return self.raw("note %d 0" % pad_note(n)).sleep(0.03)
    def pad(self, n, ms=70, vel=100): self.pad_down(n, vel); self.sleep(ms / 1000.0); return self.pad_up(n)
    def pad_dbl(self, n, gap=110): self.pad(n, 50); self.sleep(gap / 1000.0); return self.pad(n, 50)
    def step_down(self, n): return self.raw("note %d 127" % (15 + n)).sleep(0.03)
    def step_up(self, n): return self.raw("note %d 0" % (15 + n)).sleep(0.03)
    def step(self, n, ms=70): self.step_down(n); self.sleep(ms / 1000.0); return self.step_up(n)
    def step_dbl(self, n, gap=110): self.step(n, 50); self.sleep(gap / 1000.0); return self.step(n, 50)

    # ---- Track buttons (1..4)
    def track_down(self, t): return self.raw("cc %d 127" % (44 - t)).sleep(0.03)
    def track_up(self, t): return self.raw("cc %d 0" % (44 - t)).sleep(0.03)
    def track(self, t, ms=70): self.track_down(t); self.sleep(ms / 1000.0); return self.track_up(t)
    def hold_track(self, t, ms=400): self.track_down(t); return self.sleep(ms / 1000.0)

    # ---- knobs (1..8), volume, the jog wheel: relative, 1 = one click right, 127 = one left
    def knob(self, k, d=1, n=1, touch=True):
        if touch: self.raw("note %d 127" % (k - 1)).sleep(0.03)
        for _ in range(n): self.raw("cc %d %d" % (70 + k, d & 127)).sleep(0.02)
        return self
    def knob_off(self, k): return self.raw("note %d 0" % (k - 1)).sleep(0.03)
    def volume(self, d=1, n=1):
        self.raw("note 8 127").sleep(0.03)
        for _ in range(n): self.raw("cc 79 %d" % (d & 127)).sleep(0.02)
        return self
    def volume_off(self): return self.raw("note 8 0").sleep(0.03)
    def jog(self, d=1, n=1):
        for _ in range(n): self.raw("cc 14 %d" % (d & 127)).sleep(0.04)
        return self
    def jog_click(self, ms=60): self.raw("cc 3 127").sleep(ms / 1000.0); return self.raw("cc 3 0").sleep(0.03)

    def run(self):
        d = os.path.join(OUT, self.name)
        os.makedirs(d, exist_ok=True)
        for f in os.listdir(d):
            os.remove(os.path.join(d, f))
        prog = "\n".join(self.l) + "\n"
        open(os.path.join(d, "program.txt"), "w").write(prog)
        rd = "/dev/shm/dq-" + self.name
        cmd = "mkdir -p %s && python3 %s/run.py %s && tar c -C %s ." % (rd, QA, rd, rd)
        pr = subprocess.run(SSH + [cmd], input=prog.encode(), capture_output=True, timeout=600)
        if pr.returncode != 0:
            raise RuntimeError("device run failed: " + pr.stderr.decode()[-600:])
        tarfile.open(fileobj=io.BytesIO(pr.stdout)).extractall(d)
        return R(d)


QROOT = "/data/UserData/RemoteFM1-qa"
WEB = "http://" + HOST.split("@")[-1] + ":7730"
FW_X = 1048576 - 7        # the byte of a firmware's memory the tests change (as tests/projfw_e2e.sh does)


def rsh(cmd):
    """a read-only command on the Move"""
    return subprocess.run(SSH + [cmd], capture_output=True, timeout=60).stdout.decode()


def rbyte(path, off=FW_X):
    """one byte of a file on the Move as two hex digits ('' if it is not there)"""
    # (BusyBox od has no -A / -j: dd + hexdump)
    return rsh("[ -f %s ] && dd if=%s bs=1 skip=%d count=1 2>/dev/null | hexdump -e '1/1 \"%%02x\"'" % (path, path, off)).strip()


def post(path, body):
    return subprocess.run(["curl", "-s", "-m", "8", "-o", "/dev/null", "-w", "%{http_code}", "-X", "POST", "-H",
                           "Content-Type: application/json", "--data", json.dumps(body), WEB + path],
                          capture_output=True).stdout.decode()


def poke(slot, val):
    """change the test byte of a slot's firmware memory (the QA host runs with --test-api)"""
    return post("/api/test/poke", {"slot": slot, "off": FW_X, "val": val})


def load(slot, fid):
    return post("/api/load", {"slot": slot, "id": fid})


def status():
    return json.loads(subprocess.run(["curl", "-s", "-m", "8", WEB + "/api/status"], capture_output=True).stdout.decode())


def sheet(name, cols=4, scale=2, only=None):
    """a contact sheet of a run's screens, in the order they were taken: OUT/<name>.png"""
    from PIL import Image, ImageDraw
    d = os.path.join(OUT, name)
    order = [l.split()[1] for l in open(os.path.join(d, "program.txt")) if l.startswith("shot ")]
    seen, names = set(), []
    for n in order:
        if n not in seen and (only is None or n in only):
            seen.add(n); names.append(n)
    W, H, pad, lab = 128 * scale, 64 * scale, 8, 14
    rows = (len(names) + cols - 1) // cols
    im = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + lab + pad) + pad), (28, 30, 34))
    dr = ImageDraw.Draw(im)
    for i, n in enumerate(names):
        px = open(os.path.join(d, n + ".pbm")).read().split("\n", 2)[2].replace("\n", "")
        s = Image.new("L", (128, 64))
        s.putdata([255 if c == "1" else 0 for c in px[:128 * 64]])
        x, y = pad + (i % cols) * (W + pad), pad + (i // cols) * (H + lab + pad)
        im.paste(s.resize((W, H), Image.NEAREST).convert("RGB"), (x, y))
        dr.text((x, y + H + 1), n, fill=(230, 190, 90))
    out = os.path.join(OUT, name + ".png")
    im.save(out)
    return out


def text_rows(pbm_path):
    """the screen as 64 strings of '#' and '.', for a look without an image"""
    px = open(pbm_path).read().split("\n", 2)[2].split("\n")
    return [r.replace("1", "#").replace("0", ".") for r in px if r]
