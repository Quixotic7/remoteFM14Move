#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The page's Update test files (tests/update_test.sh, tests/update_e2e.sh), into OUT (build/update-test):

    good.rfm1u         this build as tools/make_update.py makes it (the tests' key), version 9.9.9-test, with an extra
                       file update-marker.txt
    good2.rfm1u        the same as 9.9.10-test (a second install over the first: .prev replaced)
    full.rfm1u         a whole module package (every name under remotefm1/, as dist/remotefm1-module.tar.gz)
    tampered.rfm1u     good with one payload byte changed;  tamper-man.rfm1u: one manifest byte changed
    catsig.rfm1u       good's bytes signed as the catalog signs index.json (the catalog's test key, the raw bytes)
    catdom.rfm1u       the catalog's test key over the update message (a key the updates do not trust)
    nodomain.rfm1u     the update key over the digest without "RFM1-UPDATE-1\\0"
    dotdot, abs, symlink, hardlink, chardev, fifo, dotpart, reserved .rfm1u   a payload breaking a rule, well signed
    gnulong, paxlong .rfm1u   names over 100 characters (GNU 'L', pax "path")
    skips.rfm1u        ._ and .DS_Store entries beside the files (skipped)
    noexec.rfm1u       remotefm1-host not executable;  notelf.rfm1u: remotefm1-host a shell script

    tests/update_fixtures.py OUT --key build/testkey/update-test-key.pem --catkey build/testkey/test-key.pem
"""
import argparse
import gzip
import hashlib
import io
import os
import shutil
import struct
import subprocess
import sys
import tarfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import make_update  # noqa: E402
import sign_index  # noqa: E402

HOST = os.path.join(REPO, 'build', 'move', 'remotefm1-host')


def tgz(entries, fmt=tarfile.USTAR_FORMAT):
    """entries: [(name, type, data|linkname, mode)] -> gzip'd tar"""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=fmt) as t:
        for name, typ, data, mode in entries:
            ti = tarfile.TarInfo(name)
            ti.type = typ
            ti.mode = mode
            ti.mtime = 1700000000
            if typ in (tarfile.SYMTYPE, tarfile.LNKTYPE):
                ti.linkname = data
                t.addfile(ti)
            elif typ == tarfile.REGTYPE:
                ti.size = len(data)
                t.addfile(ti, io.BytesIO(data))
            else:
                if typ in (tarfile.CHRTYPE, tarfile.BLKTYPE):
                    ti.devmajor, ti.devminor = 1, 3
                t.addfile(ti)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode='wb', mtime=0) as g:
        g.write(raw.getvalue())
    return out.getvalue()


def manifest(version, payload, kind='overlay'):
    names = [(m.name, m.size) for m in tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz').getmembers() if m.isfile()]
    return {'format': 1, 'version': version, 'host_commit': 'test', 'core_commit': 'test', 'built': '2026-11-25 00:00 UTC',
            'kind': kind, 'files': [{'path': n, 'size': s} for n, s in names]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--key', required=True)
    ap.add_argument('--catkey', required=True)
    a = ap.parse_args()
    out = a.out
    os.makedirs(out, exist_ok=True)
    w = lambda name, data: open(os.path.join(out, name), 'wb').write(data)   # noqa: E731
    host = open(HOST, 'rb').read()
    marker = os.path.join(out, 'marker.txt')
    open(marker, 'w').write('update marker\n')
    hdr = os.path.join(REPO, 'build', 'testkey', 'update_test_key.h')
    for v, name in (('9.9.9-test', 'good.rfm1u'), ('9.9.10-test', 'good2.rfm1u')):
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'make_update.py'), '--key', a.key, '--expect', hdr,
                        '--version', v, '--add', '%s=update-marker.txt' % marker, '--out', os.path.join(out, name)],
                       check=True)
    good = open(os.path.join(out, 'good.rfm1u'), 'rb').read()
    _, msg, sig, payload = make_update.parse(good)
    mlen = struct.unpack('<I', good[12:16])[0]
    head = good[:24 + mlen]
    # tampered: a byte of the payload, a byte of the manifest
    b = bytearray(good)
    b[-100] ^= 0x01
    w('tampered.rfm1u', bytes(b))
    b = bytearray(good)
    i = good.index(b'9.9.9-test')
    b[i] = ord('8')
    w('tamper-man.rfm1u', bytes(b))
    # other keys and messages
    w('catsig.rfm1u', head + sign_index.sign(a.catkey, head + payload) + payload)
    w('catdom.rfm1u', head + sign_index.sign(a.catkey, msg) + payload)
    w('nodomain.rfm1u', head + sign_index.sign(a.key, hashlib.sha256(head + payload).digest()) + payload)
    # payloads that break a rule (signed well: the rules alone refuse them)
    ok = ('help.json', tarfile.REGTYPE, b'{}', 0o644)
    bad = {
        'dotdot': [ok, ('site/../../evil.txt', tarfile.REGTYPE, b'x', 0o644)],
        'abs': [ok, ('/tmp/evil.txt', tarfile.REGTYPE, b'x', 0o644)],
        'symlink': [ok, ('site/link', tarfile.SYMTYPE, '/etc/passwd', 0o777)],
        'hardlink': [ok, ('site/hard', tarfile.LNKTYPE, 'help.json', 0o644)],
        'chardev': [ok, ('site/dev', tarfile.CHRTYPE, None, 0o644)],
        'fifo': [ok, ('site/fifo', tarfile.FIFOTYPE, None, 0o644)],
        'dotpart': [ok, ('site/./x.txt', tarfile.REGTYPE, b'x', 0o644)],
        'reserved': [ok, ('.update-prev/x', tarfile.REGTYPE, b'x', 0o644)],
    }
    for name, ents in bad.items():
        p = tgz(ents)
        w(name + '.rfm1u', make_update.build(manifest('6.6.6-' + name, p), p, a.key))
    long = 'site/' + 'a' * 60 + '/' + 'b' * 70 + '/long-name.txt'
    for name, fmt in (('gnulong', tarfile.GNU_FORMAT), ('paxlong', tarfile.PAX_FORMAT)):
        p = tgz([ok, (long, tarfile.REGTYPE, b'long\n', 0o644)], fmt)
        w(name + '.rfm1u', make_update.build(manifest('7.7.7-' + name, p), p, a.key))
    p = tgz([ok, ('._help.json', tarfile.REGTYPE, b'apple', 0o644), ('site/.DS_Store', tarfile.REGTYPE, b'ds', 0o644),
             ('site/x.txt', tarfile.REGTYPE, b'x', 0o644)])
    w('skips.rfm1u', make_update.build(manifest('7.7.8-skips', p), p, a.key))
    # the program checks (the install's, after the payload is good)
    p = tgz([ok, ('remotefm1-host', tarfile.REGTYPE, host, 0o644)])
    w('noexec.rfm1u', make_update.build(manifest('5.5.5-noexec', p), p, a.key))
    p = tgz([ok, ('remotefm1-host', tarfile.REGTYPE, b'#!/bin/sh\necho not a program\n', 0o755)])
    w('notelf.rfm1u', make_update.build(manifest('5.5.6-notelf', p), p, a.key))
    # a whole module package, as make package tars it (remotefm1/ at the top, folders listed)
    pkg = os.path.join(out, 'pkg')
    if os.path.isdir(pkg):
        shutil.rmtree(pkg)   # (a stale one never mixes in)
    os.makedirs(os.path.join(pkg, 'remotefm1', 'site'))
    os.makedirs(os.path.join(pkg, 'remotefm1', 'firmwares'))
    shutil.copy2(HOST, os.path.join(pkg, 'remotefm1', 'remotefm1-host'))
    open(os.path.join(pkg, 'remotefm1', 'help.json'), 'w').write('{"full": 1}\n')
    open(os.path.join(pkg, 'remotefm1', 'site', 'index.html'), 'w').write('<!doctype html><title>full</title>\n')
    open(os.path.join(pkg, 'remotefm1', 'firmwares', 'full-marker.txt'), 'w').write('full\n')
    tball = os.path.join(out, 'remotefm1-module.tar.gz')
    with tarfile.open(tball, 'w:gz', format=tarfile.PAX_FORMAT) as t:
        t.add(os.path.join(pkg, 'remotefm1'), arcname='remotefm1')
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'make_update.py'), '--key', a.key, '--expect', hdr,
                    '--version', '9.9.11-full', '--full', tball, '--out', os.path.join(out, 'full.rfm1u')], check=True)
    print('update_fixtures: %s' % out)


if __name__ == '__main__':
    main()
