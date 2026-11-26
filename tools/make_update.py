#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Make a signed update for the Move's page (README "Updating from the page"): a .rfm1u (src/update.h).

    tools/make_update.py                       this build: build/move/remotefm1-host, module/help.json, module/module.json
                                               and build/site as site/ -> dist/updates/remotefm1-<version>-<commit>.rfm1u
    tools/make_update.py --base DIR            only the site files that differ from DIR/site (DIR: an unpacked module)
    tools/make_update.py --add SRC=DEST        another file too (DEST: its name in the module's folder; repeatable)
    tools/make_update.py --full dist/remotefm1-module.tar.gz     a whole module package as the payload, as it is
    tools/make_update.py --check FILE.rfm1u [--pub src/update_key.h]   check a file's signature and list it

The key: ~/.config/remotefm14move/update-signing-key.pem (--key; never in git), refused unless its public half is
src/update_key.h's (--expect; the tests sign with build/testkey/update-test-key.pem and --expect its header). Signing
uses tools/sign_index.py's Ed25519 (Python's cryptography, else an OpenSSL 3 command line).

The commits and the build date come from build/gen/buildinfo.h (scripts/buildinfo.sh, made by make with the host), so
the manifest names what the program was built from; without it, from git.
"""
import argparse
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import gzip

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import sign_index  # noqa: E402

MAGIC = b'RFM1UPD\0'
DOMAIN = b'RFM1-UPDATE-1\0'
FORMAT = 1
MAX = 256 * 1024 * 1024
DEFAULT_KEY = os.path.expanduser('~/.config/remotefm14move/update-signing-key.pem')


def die(msg):
    sys.stderr.write('make_update: %s\n' % msg)
    sys.exit(1)


def rel(p):
    return os.path.relpath(p, os.getcwd())


def buildinfo():
    info = {}
    p = os.path.join(REPO, 'build', 'gen', 'buildinfo.h')
    if os.path.exists(p):
        for k, v in re.findall(r'#define (RFM14_\w+) "([^"]*)"', open(p).read()):
            info[k] = v
    if 'RFM14_GIT' not in info:
        def git(*a):
            try:
                return subprocess.run(['git'] + list(a), cwd=REPO, capture_output=True, text=True).stdout.strip()
            except OSError:
                return ''
        info['RFM14_GIT'] = git('rev-parse', '--short=12', 'HEAD') or 'unknown'
        info['RFM14_GIT_CORE'] = git('-C', 'third_party/rfm1cs-abletonmove', 'rev-parse', '--short=12', 'HEAD') or 'unknown'
        info['RFM14_BUILT'] = ''
    return info


def version():
    m = re.search(r'^#define VERSION "([^"]+)"', open(os.path.join(REPO, 'src', 'main.c')).read(), re.M)
    return m.group(1) if m else die('no VERSION in src/main.c')


def elf_machine(path):
    b = open(path, 'rb').read(20)
    if b[:4] != b'\x7fELF':
        return None
    return struct.unpack('<H' if b[5] == 1 else '>H', b[18:20])[0]


def payload_from(files):
    """files: [(source path, name in the module)] -> a gzip'd ustar, the names sorted, owners and dates plain"""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode='w', format=tarfile.USTAR_FORMAT) as t:
        for src, dest in sorted(files, key=lambda f: f[1]):
            st = os.lstat(src)
            if not os.path.isfile(src) or os.path.islink(src):
                die('%s: not a plain file' % src)
            ti = tarfile.TarInfo(dest)
            ti.size = st.st_size
            ti.mode = 0o755 if st.st_mode & 0o111 else 0o644
            ti.mtime = int(st.st_mtime)
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = ''
            with open(src, 'rb') as f:
                t.addfile(ti, f)
    out = io.BytesIO()
    with gzip.GzipFile(fileobj=out, mode='wb', compresslevel=9, mtime=0) as g:
        g.write(raw.getvalue())
    return out.getvalue()


def names_in(payload):
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as t:
        return [(m.name, m.size) for m in t.getmembers() if m.isfile()]


def build(manifest, payload, key):
    man = json.dumps(manifest, indent=1, sort_keys=True).encode()
    head = MAGIC + struct.pack('<IIQ', FORMAT, len(man), len(payload)) + man
    digest = hashlib.sha256(head + payload).digest()
    sig = sign_index.sign(key, DOMAIN + digest)
    if len(sig) != 64:
        die('the signature is not 64 bytes')
    return head + sig + payload


def parse(data):
    """a .rfm1u -> (manifest dict, signed message, signature, payload); dies when its sizes do not add up"""
    if len(data) < 24 + 64 or data[:8] != MAGIC:
        die('not an .rfm1u')
    fmt, mlen, plen = struct.unpack('<IIQ', data[8:24])
    if fmt != FORMAT or len(data) != 24 + mlen + 64 + plen:
        die('a damaged .rfm1u (format %d, sizes)' % fmt)
    head = data[:24 + mlen]
    sig = data[24 + mlen:24 + mlen + 64]
    payload = data[24 + mlen + 64:]
    return json.loads(head[24:]), DOMAIN + hashlib.sha256(head + payload).digest(), sig, payload


def check(path, pub_header):
    data = open(path, 'rb').read()
    man, msg, sig, payload = parse(data)
    pub = sign_index.header_key(pub_header)
    if not sign_index.verify(pub, msg, sig):
        die('%s: the signature is not good for %s' % (path, pub_header))
    print('make_update: %s: signature good (%s)' % (path, pub_header))
    print('  version %s, host %s, core %s, built %s, %s, %d bytes' % (man.get('version'), man.get('host_commit'),
          man.get('core_commit'), man.get('built'), man.get('kind'), len(data)))
    for n, s in names_in(payload):
        print('  %10d  %s' % (s, n))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--key', default=DEFAULT_KEY, help='the Ed25519 private key (PEM)')
    ap.add_argument('--expect', default=os.path.join(REPO, 'src', 'update_key.h'),
                    help="the C header with the public key the signing key must have ('' to skip)")
    ap.add_argument('--out', help='the .rfm1u (default dist/updates/remotefm1-<version>-<commit>.rfm1u)')
    ap.add_argument('--host', default=os.path.join(REPO, 'build', 'move', 'remotefm1-host'))
    ap.add_argument('--site', default=os.path.join(REPO, 'build', 'site'))
    ap.add_argument('--help-json', default=os.path.join(REPO, 'module', 'help.json'))
    ap.add_argument('--module-json', default=os.path.join(REPO, 'module', 'module.json'))
    ap.add_argument('--base', help='an unpacked module folder: only site files that differ from its site/ go in')
    ap.add_argument('--add', action='append', default=[], metavar='SRC=DEST', help='another file, DEST in the module')
    ap.add_argument('--full', help='a module package (.tar.gz, one remotefm1/ folder) as the payload')
    ap.add_argument('--app', help="another Move app's module package (.tar.gz, one <id>/ folder with its module.json): "
                    "installed into that app's own folder (README \"Other apps\")")
    ap.add_argument('--commit', help='with --app: the commit it was built from (named on the page)')
    ap.add_argument('--version', help='the version named (default: src/main.c VERSION; --app: its module.json)')
    ap.add_argument('--any-machine', action='store_true', help='(tests) do not insist on an aarch64 host program')
    ap.add_argument('--check', metavar='FILE', help='check FILE (with --pub) and list it')
    ap.add_argument('--pub', default=os.path.join(REPO, 'src', 'update_key.h'), help='with --check: the public key header')
    a = ap.parse_args()
    if a.check:
        check(a.check, a.pub)
        return
    if not os.path.isfile(a.key):
        die('no signing key at %s (it lives outside the repo; see src/update_key.h)' % a.key)
    if a.expect:
        pub = sign_index.public_key(a.key)
        if pub != sign_index.header_key(a.expect):
            die('%s is not the key %s names (public %s): refused' % (a.key, a.expect, pub.hex()))
    if a.app:
        app(a)
        return
    info = buildinfo()
    ver = a.version or version()
    commit = info.get('RFM14_GIT', 'unknown')
    if commit.endswith('-dirty') or commit == 'unknown':
        print('make_update: warning: the build is from %s (commit, then make, for a build that names its source)' % commit)
    if a.full:
        payload = open(a.full, 'rb').read()
        listed = names_in(payload)
        if not listed or not all(n.startswith('remotefm1/') for n, _ in listed):
            die('%s: not a module package (every name under remotefm1/)' % a.full)
        if 'remotefm1/remotefm1-host' not in [n for n, _ in listed]:
            die('%s: no remotefm1/remotefm1-host' % a.full)
        kind = 'full'
    else:
        files = []
        if not os.path.isfile(a.host):
            die('%s is missing: run make' % rel(a.host))
        m = elf_machine(a.host)
        if m != 183 and not a.any_machine:   # (EM_AARCH64: the Move)
            die('%s is not an aarch64 Linux program (the Move\'s)' % rel(a.host))
        files.append((a.host, 'remotefm1-host'))
        for src, dest in ((a.help_json, 'help.json'), (a.module_json, 'module.json')):
            json.load(open(src))   # (a damaged JSON file never goes out)
            files.append((src, dest))
        if not os.path.isfile(os.path.join(a.site, 'index.html')):
            die('%s has no index.html: run make site' % rel(a.site))
        skipped = 0
        for d, dirs, fs in os.walk(a.site):
            dirs.sort()
            for f in sorted(fs):
                if f.startswith('.'):
                    continue
                src = os.path.join(d, f)
                r = os.path.relpath(src, a.site)
                if a.base:
                    old = os.path.join(a.base, 'site', r)
                    if os.path.isfile(old) and open(old, 'rb').read() == open(src, 'rb').read():
                        skipped += 1
                        continue
                files.append((src, 'site/' + r))
        for spec in a.add:
            src, _, dest = spec.partition('=')
            if not dest or not os.path.isfile(src):
                die('--add %s: SRC=DEST with SRC a file' % spec)
            files.append((src, dest))
        if skipped:
            print('make_update: %d site files as in %s: left out' % (skipped, a.base))
        payload = payload_from(files)
        listed = names_in(payload)
        kind = 'overlay'
    if len(payload) > MAX:
        die('the payload is %d bytes: more than the Move takes' % len(payload))
    manifest = {
        'format': FORMAT,
        'version': ver,
        'host_commit': commit,
        'core_commit': info.get('RFM14_GIT_CORE', 'unknown'),
        'built': info.get('RFM14_BUILT', ''),
        'kind': kind,
        'module': 'remotefm1',
        'name': 'RemoteFM1',
        'files': [{'path': n[10:] if kind == 'full' else n, 'size': s} for n, s in listed],
    }
    data = build(manifest, payload, a.key)
    out = a.out or os.path.join(REPO, 'dist', 'updates', 'remotefm1-%s-%s.rfm1u' % (ver, commit))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(out + '.tmp', out)
    _, msg, sig, _ = parse(data)
    if not sign_index.verify(sign_index.public_key(a.key), msg, sig):
        die('the new signature does not check: refused')
    print('make_update: %s (%s %s, %s, %d files, %d bytes)' % (rel(out), ver, commit, kind, len(listed), len(data)))


def app(a):
    """(another Move app) its whole module package, for its own folder beside RemoteFM1's"""
    import io, re, tarfile
    payload = open(a.app, 'rb').read()
    listed = names_in(payload)
    tops = {n.split('/')[0] for n, _ in listed}
    if len(tops) != 1:
        die('%s: not a module package (one top folder, its id)' % a.app)
    mid = tops.pop()
    if not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,39}', mid) or mid == 'remotefm1':
        die('%s: its folder %r is not an app id this takes' % (a.app, mid))
    with tarfile.open(fileobj=io.BytesIO(payload), mode='r:gz') as tf:
        try:
            mj = json.load(tf.extractfile('%s/module.json' % mid))
        except (KeyError, ValueError):
            die('%s: no %s/module.json' % (a.app, mid))
    if mj.get('id') != mid:
        die('%s: %s/module.json says id %r' % (a.app, mid, mj.get('id')))
    if len(payload) > MAX:
        die('the payload is %d bytes: more than the Move takes' % len(payload))
    ver = a.version or str(mj.get('version', '0'))
    commit = a.commit or 'unknown'
    manifest = {
        'format': FORMAT, 'version': ver, 'host_commit': commit, 'core_commit': '', 'built': '',
        'kind': 'full', 'module': mid, 'name': str(mj.get('name', mid))[:60],
        'files': [{'path': n[len(mid) + 1:], 'size': s} for n, s in listed if n != mid],
    }
    data = build(manifest, payload, a.key)
    out = a.out or os.path.join(REPO, 'dist', 'updates', '%s-%s-%s.rfm1u' % (mid, ver, commit[:12]))
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out + '.tmp', 'wb') as f:
        f.write(data)
    os.replace(out + '.tmp', out)
    _, msg, sig, _ = parse(data)
    if not sign_index.verify(sign_index.public_key(a.key), msg, sig):
        die('the new signature does not check: refused')
    print('make_update: %s (%s %s %s, %d files, %d bytes)' % (rel(out), manifest['name'], ver, commit, len(listed), len(data)))


if __name__ == '__main__':
    main()
