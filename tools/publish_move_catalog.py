#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Publish the Move catalog (make catalog's dist/move-catalog/) to a GitHub release, as a delta.

Only the files the release does not already hold byte for byte are uploaded: each file of the signed index.json
(its `files` list: name + sha256) is compared with the release's assets by name and GitHub's sha256 digest. A new or
updated firmware is a few files; nothing else moves. index.json.sig and index.json go up last, back to back, so a
Move never sees an index naming a file that is not there yet (between the two a Move refuses the pair and tries again
10 minutes later: it fails safe).

Assets the new index no longer names (older versions) are kept unless --prune: a Move that fetched the previous index
a moment ago can still finish its download. --prune deletes them (after the new index is up).

    tools/publish_move_catalog.py [--dir dist/move-catalog] [--repo Quixotic7/remoteFM14Move] [--tag move-catalog]
                                  [--dry-run] [--prune]

Needs `gh` logged in with write access to the repository. The release must exist (--create makes it, on the repo's
default branch, which therefore needs a commit). Exit 0: the release matches the directory.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def gh(*args, capture=True):
    r = subprocess.run(["gh", *args], capture_output=capture, text=True)
    if r.returncode != 0:
        raise SystemExit(f"gh {' '.join(args)}: {(r.stderr or r.stdout).strip()}")
    return r.stdout


def release_assets(repo, tag):
    """name -> (sha256 or None, id) of the release's assets; None if there is no such release"""
    r = subprocess.run(["gh", "api", f"repos/{repo}/releases/tags/{tag}"], capture_output=True, text=True)
    if r.returncode != 0:
        if "Not Found" in (r.stderr + r.stdout):
            return None
        raise SystemExit(f"the release {repo} {tag}: {r.stderr.strip()}")
    rel = json.loads(r.stdout)
    assets = {}
    page = 1
    while True:
        out = gh("api", f"repos/{repo}/releases/{rel['id']}/assets?per_page=100&page={page}")
        batch = json.loads(out)
        for a in batch:
            d = a.get("digest") or ""
            assets[a["name"]] = (d.split(":", 1)[1] if d.startswith("sha256:") else None, a["id"])
        if len(batch) < 100:
            break
        page += 1
    return assets


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default="dist/move-catalog")
    ap.add_argument("--repo", default="Quixotic7/remoteFM14Move")
    ap.add_argument("--tag", default="move-catalog")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--prune", action="store_true", help="delete assets the new index no longer names")
    ap.add_argument("--create", action="store_true", help="create the release if it is missing")
    a = ap.parse_args()

    index_p, sig_p = os.path.join(a.dir, "index.json"), os.path.join(a.dir, "index.json.sig")
    if not (os.path.isfile(index_p) and os.path.isfile(sig_p)):
        raise SystemExit(f"{a.dir}: no index.json + index.json.sig (run make catalog)")
    index = json.load(open(index_p, "rb"))
    want = {}                                   # name -> sha256, every file the index names
    for fw in index.get("firmwares", []):
        for f in fw.get("files", []):
            want[f["name"]] = f["sha256"]
    have = release_assets(a.repo, a.tag)
    for name, sha in want.items():              # the directory must match its own index before anything goes up
        p = os.path.join(a.dir, name)           # (a file the release already holds byte for byte needs no local
        if not os.path.isfile(p) and have and have.get(name, (None,))[0] == sha:   # copy: a partial catalog)
            continue
        if not os.path.isfile(p) or sha256(p) != sha:
            raise SystemExit(f"{p}: missing or not the index's sha256 (rebuild with make catalog)")
    if have is None:
        if not a.create:
            raise SystemExit(f"no release {a.tag} on {a.repo} (--create makes it)")
        print(f"create the release {a.tag} on {a.repo}")
        if not a.dry_run:
            gh("release", "create", a.tag, "--repo", a.repo, "--title", "Move catalog",
               "--notes", "RemoteFM1 on Ableton Move: the firmware catalog (signed index.json + the Move builds). "
                          "Updated in place by tools/publish_move_catalog.py; not a software release.")
        have = {}

    up = sorted(n for n, s in want.items() if have.get(n, (None,))[0] != s)
    meta = [n for n in ("index.json.sig", "index.json") if have.get(n, (None,))[0] != sha256(os.path.join(a.dir, n))]
    stale = sorted(n for n in have if n not in want and n not in ("index.json", "index.json.sig"))
    size = sum(os.path.getsize(os.path.join(a.dir, n)) for n in up)
    print(f"{len(want)} files in the index; the release has {len(have)} assets")
    print(f"upload {len(up)} file(s), {size / 1048576:.1f} MB" + (": " + ", ".join(up[:12]) + (" ..." if len(up) > 12 else "") if up else ""))
    print(f"then the index: {'yes' if meta else 'unchanged'}; not in the index: {len(stale)}" +
          (" (deleted: --prune)" if a.prune and stale else " (kept)" if stale else ""))
    if a.dry_run:
        return 0
    for i in range(0, len(up), 20):             # firmware files first (a batch per call)
        gh("release", "upload", a.tag, "--repo", a.repo, "--clobber", *[os.path.join(a.dir, n) for n in up[i:i + 20]])
        print(f"  uploaded {min(i + 20, len(up))}/{len(up)}")
    if meta:                                    # the signature, then the index it signs: back to back, last
        gh("release", "upload", a.tag, "--repo", a.repo, "--clobber", sig_p, index_p)
        print("  uploaded index.json.sig, index.json")
    if a.prune:
        for n in stale:
            gh("api", "-X", "DELETE", f"repos/{a.repo}/releases/assets/{have[n][1]}")
        if stale:
            print(f"  deleted {len(stale)} asset(s) the index no longer names")
    after = release_assets(a.repo, a.tag) or {}
    bad = [n for n, s in list(want.items()) + [("index.json", sha256(index_p)), ("index.json.sig", sha256(sig_p))]
           if after.get(n, (None,))[0] not in (s, None) or n not in after]
    if bad:
        raise SystemExit(f"after the upload these do not match: {', '.join(bad[:10])}")
    print("the release matches the directory")
    return 0


if __name__ == "__main__":
    sys.exit(main())
