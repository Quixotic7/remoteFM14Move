# On-device QA

Drives the host that runs **on the Move** from a Mac, through Move Remote's test hook, and brings back the real
screen and status. It tests the installed aarch64 build with its real threads, audio thread and firmware loads; the
simulator's end-to-end tests (`tests/*.sh`) do not.

## What it can and cannot see

- **Sees:** the 128x64 screen (`shot`), `/api/status` (with `--test-api`: the pads' colours as last written), the
  files the host writes, the host's log, late audio transfers.
- **Does not see:** the step, button, Track and knob lights; the sound (the hook's output is silent; `peak` and
  `notes` show that sound was made); how the pads feel. Those need the owner at the Move.

## The owner's data is never used

`standalone-qa` launches the installed host with `--root /data/UserData/RemoteFM1-qa`, a copy of the real folder made
just before, and `--test-api`. The real `/data/UserData/RemoteFM1` is not opened during a run.

## A run (ask the owner first: it stops RemoteFM1 on the Move and starts it again)

1. Put the files on the Move: `standalone-qa` as `/data/UserData/remotefm1-qa/standalone` (mode 755), `run.py`
   beside it.
2. Stop the running host (`kill -TERM`, wait for `/dev/shm/schwung-standalone.lock` to go), copy the data folder
   (`cp -a /data/UserData/RemoteFM1 /data/UserData/RemoteFM1-qa`), put a file holding `0` at `/dev/shm/mr-test`
   (the test hook, output at 0 %).
3. `launch-standalone.sh /data/UserData/remotefm1-qa/standalone`; the log says `test hook: on`.
4. Drive it with `dq.py` (below). Screens and status land in `build/device-qa/<run>/`, a contact sheet in
   `build/device-qa/<run>.png`.
5. Put it back: stop the host, delete `/dev/shm/mr-test`, launch the module's own `standalone`. Check that
   `POST /api/test/poke` answers 403 (the test API is off) and that the log has no `test hook: on`.

## dq.py

```python
from dq import *
p = P("states")                       # a program: every step is timed ON the Move (run.py), so holds are exact
p.hold_track(1).pad(7); p.track_up(1) # Track 1 held + pad 7 (the owner's pad numbers: 1 top left)
p.sleep(1.3).both("grid")             # the screen and the status, named "grid"
r = p.run()
r.s("grid", "states_open"); r.pad("grid", 1)
sheet("states")                       # build/device-qa/states.png
poke(0, 0x5a); rbyte(QROOT + "/states/felucca/state-00.bin")   # a firmware's memory changed, a file read back
```

A knob: two ticks are one FM-1 detent (`p.knob(k, 1, 2)`); an option in a menu takes five ticks a step.
