#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A partial Move catalog: the PUBLISHED index (the release's index.json, as it is) with only the named firmwares'
entries taken from a fresh `make catalog` (dist/move-catalog); every other entry, and its files on the release, stay
byte for byte. The output directory holds the merged index.json and those firmwares' files only; sign it
(tools/sign_index.py) and publish it with tools/publish_move_catalog.py --dir OUT (files the release already holds
need no local copy).

  tools/partial_move_catalog.py --ids fizz choralroot [--from dist/move-catalog] [--out dist/move-catalog-partial]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ids", nargs="+", required=True, help="the firmware ids to take from --from")
    ap.add_argument("--from", dest="src", default="dist/move-catalog")
    ap.add_argument("--out", default="dist/move-catalog-partial")
    ap.add_argument("--repo", default="Quixotic7/remoteFM14Move")
    ap.add_argument("--tag", default="move-catalog")
    a = ap.parse_args()

    fresh = json.load(open(os.path.join(a.src, "index.json"), "rb"))
    by_id = {f["id"]: f for f in fresh["firmwares"]}
    missing = [i for i in a.ids if i not in by_id]
    if missing:
        raise SystemExit(f"{a.src}/index.json has no {', '.join(missing)}")

    with tempfile.TemporaryDirectory() as td:   # the published index, as it is
        subprocess.run(["gh", "release", "download", a.tag, "--repo", a.repo, "--pattern", "index.json", "--dir", td],
                       check=True)
        pub = json.load(open(os.path.join(td, "index.json"), "rb"))

    if os.path.exists(a.out):
        raise SystemExit(f"{a.out} exists: move it aside first")
    os.makedirs(a.out)
    out, done = [], set()
    for f in pub["firmwares"]:                  # the published order; a named id replaced in place
        if f["id"] in by_id and f["id"] in a.ids:
            out.append(by_id[f["id"]])
            done.add(f["id"])
        else:
            out.append(f)
    for i in a.ids:                             # a named id the published index lacks: added at the end
        if i not in done:
            out.append(by_id[i])
    for i in a.ids:                             # its files, checked against the fresh index
        for fl in by_id[i].get("files", []):
            src = os.path.join(a.src, fl["name"])
            if sha256(src) != fl["sha256"]:
                raise SystemExit(f"{src}: not the index's sha256")
            shutil.copy2(src, os.path.join(a.out, fl["name"]))
    pub["firmwares"] = out
    pub["generated"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    with open(os.path.join(a.out, "index.json"), "w") as f:
        json.dump(pub, f, separators=(",", ":"), ensure_ascii=False)
    for i in a.ids:
        old = next((f for f in json.load(open(os.path.join(a.out, "index.json")))["firmwares"] if f["id"] == i), None)
        print(f"{i}: {old['version'] if old else '?'} from {a.src}")
    print(f"{a.out}: {len(out)} firmwares ({len(a.ids)} taken from {a.src}); now sign it and publish with --dir {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
