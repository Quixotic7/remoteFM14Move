#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The module's firmwares from the Move catalog (make package, on the Mac, after scripts/package.sh staged the module):

    tools/bundle.py build/pkg/remotefm1 [--catalog dist/move-catalog] [--list module/bundle.txt]

With dist/move-catalog/index.json there (make catalog):
    - firmwares/ holds the files of each firmware module/bundle.txt names (as the catalog has them: the .rfm1m, its
      manifest, licence texts and skin), instead of the build's own (build/fw);
    - catalog/index.json is the catalog's index, with its signature catalog/index.json.sig (make catalog signs it; it
      must check against src/catalog_key.h's key, else the package is refused): the Move's browser lists the whole
      catalog offline (what exists; a download needs Wi-Fi);
    - FIRMWARES.md lists the bundled ones (licence, exact source, the .rfm1's SHA-256).
Without it the module keeps build/fw's firmwares and has no index (the browser lists what is installed)."""
import json, os, shutil, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import sign_index  # noqa: E402


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    pkg = args[0]
    cat = os.path.join(REPO, 'dist', 'move-catalog')
    lst = os.path.join(REPO, 'module', 'bundle.txt')
    if '--catalog' in args:
        cat = args[args.index('--catalog') + 1]
    if '--list' in args:
        lst = args[args.index('--list') + 1]
    ipath = os.path.join(cat, 'index.json')
    if not os.path.exists(ipath):
        print('bundle: no %s (make catalog): the module keeps build/fw\'s firmwares, no catalog index' % ipath)
        return
    if not sign_index.check(ipath, sign_index.header_key(os.path.join(REPO, 'src', 'catalog_key.h'))):
        sys.exit('bundle: %s.sig is missing or not a good signature by src/catalog_key.h\'s key: refused '
                 '(make catalog signs it)' % ipath)
    index = json.load(open(ipath))
    by_id = {f['id']: f for f in index['firmwares']}
    want = [l.strip() for l in open(lst) if l.strip() and not l.startswith('#')]
    fw = os.path.join(pkg, 'firmwares')
    if os.path.exists(fw):
        shutil.rmtree(fw)   # (scripts/package.sh's copy of build/fw, staged moments ago)
    os.makedirs(fw)
    os.makedirs(os.path.join(pkg, 'catalog'), exist_ok=True)
    shutil.copyfile(ipath, os.path.join(pkg, 'catalog', 'index.json'))
    shutil.copyfile(ipath + '.sig', os.path.join(pkg, 'catalog', 'index.json.sig'))
    rows, size = [], 0
    for i in want:
        f = by_id.get(i)
        if not f:
            print('bundle: %s is not in the catalog: skipped' % i, file=sys.stderr)
            continue
        for x in f['files']:
            d = os.path.join(fw, x['name'])
            shutil.copyfile(os.path.join(cat, x['name']), d)
            os.chmod(d, 0o755 if x['name'].endswith('.rfm1m') else 0o644)
            size += x['size']
        rows.append(f)
    rows.sort(key=lambda f: f['name'].lower())
    with open(os.path.join(pkg, 'FIRMWARES.md'), 'w') as o:
        o.write('# The bundled firmwares\n\n'
                'Each is an FM-1 firmware\'s .rfm1 file translated to C by wasm2c 1.0.32 and compiled for the Move\n'
                '(<id>-<version>.rfm1m). Its manifest (<id>-<version>.json) names the .rfm1 it came from and that file\'s\n'
                'SHA-256; <id>-<version>.licenses.txt holds the licence and credit sections the .rfm1 carries. The source each\n'
                'firmware\'s licence promises is at its source_url (the exact commit), with the build glue at FM1VST.\n\n'
                'The module bundles %d of the Move catalog\'s %d firmwares (module/bundle.txt); catalog/index.json lists them\n'
                'all (generated %s), and the Move downloads the others from the catalog\'s base URL (Mute in the projects\n'
                'view).\n\n| Firmware | Version | Licence | Source | .rfm1 SHA-256 |\n|---|---|---|---|---|\n'
                % (len(rows), len(index['firmwares']), index.get('generated')))
        for f in rows:
            o.write('| %s | %s | %s | %s | %s |\n' % (f['name'], f['version'], f['license'], f['source_url'],
                                                      f['rfm1']['sha256']))
    print('bundle: %d firmwares (%.1f MB) and the signed catalog index (%d firmwares) into %s'
          % (len(rows), size / 1e6, len(index['firmwares']), pkg))


if __name__ == '__main__':
    main()
