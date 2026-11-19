#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Sign (or check) a Move catalog index: index.json -> index.json.sig.

    tools/sign_index.py --key KEY.pem [--expect src/catalog_key.h] INDEX.json     sign; then check the result
    tools/sign_index.py --check --pub src/catalog_key.h INDEX.json                 check only (tools/bundle.py)

index.json.sig is the base64 of the 64-byte Ed25519 signature (RFC 8032) over index.json's exact bytes, then a newline
(src/catsig.h). The key is a PKCS#8 PEM Ed25519 private key; `make catalog` uses
~/.config/remotefm14move/catalog-signing-key.pem (CATALOG_SIGNING_KEY=path names another, e.g. for a CI), never a
file in the repo. --expect: refuse unless the key's public half is the one compiled into the host. The tests sign
with build/testkey/test-key.pem.

Signing uses Python's cryptography when it is installed, else an OpenSSL 3 command line (OPENSSL, openssl on PATH,
Homebrew's): macOS's own /usr/bin/openssl (LibreSSL) has no Ed25519 and is skipped. Nothing is installed.
"""
import argparse
import base64
import os
import re
import shutil
import subprocess
import sys
import tempfile

SPKI_PREFIX = bytes.fromhex('302a300506032b6570032100')   # an Ed25519 SubjectPublicKeyInfo, before the 32 bytes


def die(msg):
    sys.stderr.write('sign_index: %s\n' % msg)
    sys.exit(1)


def find_openssl():
    cands = [os.environ.get('OPENSSL'), shutil.which('openssl'), '/opt/homebrew/bin/openssl',
             '/usr/local/opt/openssl@3/bin/openssl', '/opt/homebrew/opt/openssl@3/bin/openssl', '/usr/bin/openssl']
    for c in cands:
        if not c or not os.path.exists(c):
            continue
        try:
            out = subprocess.run([c, 'list', '-public-key-algorithms'], capture_output=True, text=True, timeout=20)
        except OSError:
            continue
        if out.returncode == 0 and 'ED25519' in out.stdout.upper():
            return c
    return None


def header_key(path):
    """the 32 bytes of the first array in a C header (src/catalog_key.h)"""
    text = open(path).read()
    body = text[text.index('{'):text.index('}')]
    b = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', body))
    if len(b) != 32:
        die('%s: not a 32-byte key' % path)
    return b


def public_key(key_pem):
    try:
        from cryptography.hazmat.primitives import serialization
        k = serialization.load_pem_private_key(open(key_pem, 'rb').read(), password=None)
        return k.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    except ImportError:
        pass
    ossl = find_openssl() or die('no OpenSSL with Ed25519 and no Python cryptography: cannot sign')
    der = subprocess.run([ossl, 'pkey', '-in', key_pem, '-pubout', '-outform', 'DER'], capture_output=True, check=True)
    if not der.stdout.startswith(SPKI_PREFIX) or len(der.stdout) != 44:
        die('%s: not an Ed25519 key' % key_pem)
    return der.stdout[-32:]


def sign(key_pem, data):
    try:
        from cryptography.hazmat.primitives import serialization
        k = serialization.load_pem_private_key(open(key_pem, 'rb').read(), password=None)
        return k.sign(data)
    except ImportError:
        pass
    ossl = find_openssl() or die('no OpenSSL with Ed25519 and no Python cryptography: cannot sign')
    with tempfile.TemporaryDirectory() as t:
        src = os.path.join(t, 'msg')
        open(src, 'wb').write(data)
        out = subprocess.run([ossl, 'pkeyutl', '-sign', '-rawin', '-inkey', key_pem, '-in', src], capture_output=True)
        if out.returncode != 0 or len(out.stdout) != 64:
            die('openssl pkeyutl -sign failed: %s' % out.stderr.decode(errors='replace').strip())
        return out.stdout


def verify(pub, data, sig):
    """True when sig (64 bytes) is pub's (32 bytes) signature of data"""
    try:
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
        try:
            Ed25519PublicKey.from_public_bytes(pub).verify(sig, data)
            return True
        except InvalidSignature:
            return False
    except ImportError:
        pass
    ossl = find_openssl() or die('no OpenSSL with Ed25519 and no Python cryptography: cannot check')
    with tempfile.TemporaryDirectory() as t:
        p = {n: os.path.join(t, n) for n in ('pub.der', 'msg', 'sig')}
        open(p['pub.der'], 'wb').write(SPKI_PREFIX + pub)
        open(p['msg'], 'wb').write(data)
        open(p['sig'], 'wb').write(sig)
        out = subprocess.run([ossl, 'pkeyutl', '-verify', '-pubin', '-inkey', p['pub.der'], '-keyform', 'DER', '-rawin',
                              '-in', p['msg'], '-sigfile', p['sig']], capture_output=True)
        return out.returncode == 0


def read_sig(path):
    try:
        sig = base64.b64decode(open(path, 'rb').read().strip(), validate=True)
    except (OSError, ValueError):
        return None
    return sig if len(sig) == 64 else None


def check(index, pub):
    """index.json and its .sig: True when good"""
    sig = read_sig(index + '.sig')
    return sig is not None and verify(pub, open(index, 'rb').read(), sig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('index')
    ap.add_argument('--key', help='the Ed25519 private key (PEM) to sign with')
    ap.add_argument('--expect', help='a C header with the public key the signing key must have (src/catalog_key.h)')
    ap.add_argument('--check', action='store_true', help='only check index.json.sig')
    ap.add_argument('--pub', help='with --check: the public key\'s C header')
    a = ap.parse_args()
    if a.check:
        if not a.pub:
            die('--check needs --pub')
        if not check(a.index, header_key(a.pub)):
            die('%s.sig: missing or not a good signature by %s\'s key' % (a.index, a.pub))
        print('sign_index: %s: signature good' % a.index)
        return
    if not a.key:
        die('--key KEY.pem is needed (make catalog: CATALOG_SIGNING_KEY)')
    if not os.path.isfile(a.key):
        die('no signing key at %s (the catalog key lives outside the repo; see src/catalog_key.h)' % a.key)
    pub = public_key(a.key)
    if a.expect and pub != header_key(a.expect):
        die('%s is not the key %s names (public %s): refused' % (a.key, a.expect, pub.hex()))
    data = open(a.index, 'rb').read()
    sig = sign(a.key, data)
    if not verify(pub, data, sig):
        die('the new signature does not check: refused')
    tmp = a.index + '.sig.tmp'
    with open(tmp, 'w') as f:
        f.write(base64.b64encode(sig).decode() + '\n')
    os.replace(tmp, a.index + '.sig')
    print('sign_index: %s.sig (Ed25519, public key %s)' % (a.index, pub.hex()))


if __name__ == '__main__':
    main()
