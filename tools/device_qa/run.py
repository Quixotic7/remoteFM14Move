# SPDX-License-Identifier: MPL-2.0
"""Runs ON the Move (python3 run.py OUTDIR < program): one line a step, timed here so holds and double clicks are
exact whatever the network does.

  note N V | cc N V | at N V      a control, through the test hook's FIFO (/dev/shm/mr-cmd)
  sleep S                         seconds
  shot NAME                       the screen as OUTDIR/NAME.pbm (128x64, 1 = lit)
  status NAME                     /api/status as OUTDIR/NAME.json (waits for a status newer than the last control)
  waitfor KEY VALUE SECONDS       until status[KEY] == VALUE (strings compared); a line in OUTDIR/log says if it did
  log TEXT                        a line in OUTDIR/log
"""
import json, os, shutil, sys, time, urllib.request

OUT = sys.argv[1]
SHM = "/dev/shm"
os.makedirs(OUT, exist_ok=True)
fifo = os.open(SHM + "/mr-cmd", os.O_WRONLY | os.O_NONBLOCK)
log = open(OUT + "/log", "a")


def status():
    try:
        with urllib.request.urlopen("http://127.0.0.1:7730/api/status", timeout=3) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:  # noqa: BLE001
        return {"_error": str(e)}


def shot(name):
    p = SHM + "/mr-screen.pbm"
    try:
        t0 = os.stat(p).st_mtime_ns
    except OSError:
        t0 = 0
    os.write(fifo, b"shot\n")
    for _ in range(100):
        time.sleep(0.01)
        try:
            if os.stat(p).st_mtime_ns != t0:
                break
        except OSError:
            pass
    shutil.copyfile(p, OUT + "/" + name + ".pbm")


for raw in sys.stdin:
    ln = raw.strip()
    if not ln or ln.startswith("#"):
        continue
    w = ln.split(None, 1)
    op, arg = w[0], (w[1] if len(w) > 1 else "")
    if op in ("note", "cc", "at"):
        os.write(fifo, (ln + "\n").encode())
    elif op == "sleep":
        time.sleep(float(arg))
    elif op == "shot":
        shot(arg)
    elif op == "status":
        time.sleep(1.15)  # the status is rebuilt about once a second
        json.dump(status(), open(OUT + "/" + arg + ".json", "w"))
    elif op == "waitfor":
        key, val, secs = arg.split()
        end = time.time() + float(secs)
        got = None
        while time.time() < end:
            got = str(status().get(key))
            if got == val:
                break
            time.sleep(0.2)
        log.write("waitfor %s %s: %s (%s)\n" % (key, val, "ok" if got == val else "TIMEOUT", got))
    elif op == "log":
        log.write(arg + "\n")
    log.flush()
os.close(fifo)
