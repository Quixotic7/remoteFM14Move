#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The Move catalog: every published .rfm1 of RemoteFM1's catalog, compiled for the Move, with the data the Move's
firmware browser shows (rfm1cs-abletonmove's rfm_move_fw_list) and the files its downloads fetch.

    tools/build_move_catalog.py [--out dist/move-catalog] [--only id,id] [--catalog FILE|URL] [--jobs 4]

Run on the Mac, from the repo, after `make` (it uses build/tools and build/ref in the build image, Docker). Writes:

    OUT/index.json              the catalog (schema 1): each firmware's id, name, version, categories, ASCII
                                description, licence, source, the .rfm1's SHA-256, the files to install (name,
                                SHA-256, size; the manifest .json last) and a 64x64 1-bit thumbnail (base64)
    OUT/<id>-<version>.rfm1m    the compiled firmware (tools/build_fw.sh), with .json (its manifest), .licenses.txt
                                and its skin's files (.rfm1skin, .rfm1skin.bg, .rfm1skin.bgl) when it has one
    OUT-report.md, OUT-report.json   what was tried, built, passed, left out and why; the sizes

The inputs:
    - RemoteFM1's catalog (https://remotefm1.com/catalog.json): the firmwares with an `rfm1` entry are the ones the site
      hosts as .rfm1 files (hosted-only ones included: they are hosted, not bundled). Each file is fetched from the
      site (rfm1.file), checked against the catalog's SHA-256; failing that, RFM1Manager/out's copy with that SHA-256.
    - RemoteFM1FW's catalog/catalog.json and images (read only, beside this repo): the thumbnails (a centre square,
      squeezed to 64 x 64, autocontrast, an Atkinson dither on luma: the mockup's gen_data.py) and the hosted-only
      marks (README.md's daggers).
Firmwares the owner excludes are never listed; never a raw .syx (only .rfm1 files are read, only compiled objects and their sections written).
Each firmware is built and checked by tools/catalog_fw.sh in the image; one that fails is left out and reported.
"""
import argparse, base64, concurrent.futures, hashlib, io, json, os, re, shutil, subprocess, sys, time
import unicodedata

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GH = os.path.dirname(REPO)
CATALOG_URL = 'https://remotefm1.com/catalog.json'
SITE = 'https://remotefm1.com/'
FW_REPO = os.path.join(GH, 'RemoteFM1FW')
MANAGER_OUT = os.path.join(GH, 'RFM1Manager', 'out')
CATS = ['Synth', 'Groovebox', 'Chords', 'Drums', 'Other']    # rfm1cs's bits 1 2 4 8 16, steps 3..7
IMAGE = os.environ.get('IMAGE', 'rfm14move-build')
UA = 'RemoteFM14Move-catalog-build'


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def fetch(url, dest):
    # curl (the system's certificates; python.org's Python on macOS has none of its own)
    subprocess.run(['curl', '-fsSL', '--retry', '2', '-m', '300', '-A', UA, '-o', dest + '.part', url], check=True,
                   stderr=subprocess.PIPE)
    os.replace(dest + '.part', dest)


def ascii_text(s):
    """the description as the Move's font draws it: ASCII only (accents stripped, typographic marks made plain)"""
    if not s:
        return ''
    for a, b in (('—', ' - '), ('–', '-'), ('‘', "'"), ('’', "'"), ('“', '"'),
                 ('”', '"'), ('…', '...'), ('×', 'x'), ('°', ' deg'), ('→', '->'),
                 (' ', ' '), ('•', '-'), ('·', '-'), ('′', "'"), ('½', '1/2')):
        s = s.replace(a, b)
    s = unicodedata.normalize('NFKD', s)
    s = ''.join(c for c in s if not unicodedata.combining(c))
    s = s.encode('ascii', 'ignore').decode()
    s = re.sub(r'[\x00-\x1f\x7f]', ' ', s)
    return re.sub(r' {2,}', ' ', s).strip()


def atkinson64(img_bytes):
    """64 x 64, 1 bit, rows top first, MSB left (512 bytes): the mockup's gen_data.py, exactly"""
    from PIL import Image, ImageOps
    n = 64
    im = Image.open(io.BytesIO(img_bytes)).convert('RGB')
    w, h = im.size
    s = min(w, h)
    im = im.crop(((w - s) // 2, (h - s) // 2, (w - s) // 2 + s, (h - s) // 2 + s))
    g = ImageOps.autocontrast(im.convert('L').resize((n, n), Image.LANCZOS), cutoff=1)
    px = [[float(g.getpixel((x, y))) for x in range(n)] for y in range(n)]
    out = [[0] * n for _ in range(n)]
    for y in range(n):
        for x in range(n):
            o = px[y][x]
            v = 255 if o >= 128 else 0
            out[y][x] = 1 if v else 0
            e = (o - v) / 8
            for dx, dy in ((1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)):
                X, Y = x + dx, y + dy
                if 0 <= X < n and 0 <= Y < n:
                    px[Y][X] += e
    bits = bytearray()
    for y in range(n):
        for x in range(0, n, 8):
            b = 0
            for k in range(8):
                if out[y][x + k]:
                    b |= 0x80 >> k
            bits.append(b)
    assert len(bits) == 512
    return bytes(bits)


def hosted_only_files():
    """README.md's table: a dagger after the name marks a hosted-only file (RFM1Manager's README section 6)"""
    try:
        text = open(os.path.join(FW_REPO, 'README.md'), encoding='utf-8').read()
    except OSError:
        return set()
    return set(m.group(1) for m in re.finditer(r'^\| `([^`]+\.rfm1)` \|[^|]*†', text, re.M))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(REPO, 'dist', 'move-catalog'))
    ap.add_argument('--only', default='')
    ap.add_argument('--catalog', default=CATALOG_URL)
    ap.add_argument('--cache', default=os.path.join(REPO, 'build', 'catalog-cache'))
    ap.add_argument('--jobs', type=int, default=4)
    args = ap.parse_args()
    out = os.path.abspath(args.out)
    cache = os.path.abspath(args.cache)
    for d in (cache, os.path.join(cache, 'rfm1'), os.path.join(cache, 'built')):
        os.makedirs(d, exist_ok=True)
    if not cache.startswith(REPO + '/') or not out.startswith(REPO + '/'):
        sys.exit('build_move_catalog: --out and --cache must be inside the repo (the build image sees only it)')
    t0 = time.time()

    # ---- the catalog
    if re.match(r'https?://', args.catalog):
        cpath = os.path.join(cache, 'catalog.json')
        try:
            fetch(args.catalog, cpath)
        except Exception as e:
            if not os.path.exists(cpath):
                sys.exit('build_move_catalog: cannot fetch %s: %s' % (args.catalog, e))
            log('build_move_catalog: %s: %s; using the cached copy' % (args.catalog, e))
    else:
        cpath = args.catalog
    cat = json.load(open(cpath, encoding='utf-8'))
    cards = {}
    try:
        cards = json.load(open(os.path.join(FW_REPO, 'catalog', 'catalog.json'), encoding='utf-8'))['firmwares']
    except (OSError, KeyError, ValueError) as e:
        log('build_move_catalog: no RemoteFM1FW card data (%s): no thumbnails' % e)
    hosted = hosted_only_files()
    only = set(x for x in args.only.split(',') if x)

    tried, left = [], []
    for f in cat['firmwares']:
        r = f.get('rfm1')
        if only and f['id'] not in only:
            continue
        if 'bubba' in json.dumps(f).lower():
            left.append(dict(id=f['id'], name=f.get('name'), why='excluded by the owner'))
            continue
        if not r:
            continue   # not an .rfm1 the site hosts: gated, unpublished, or another engine only (not counted)
        if not r.get('fileName', '').endswith('.rfm1') or not re.match(r'^[a-z0-9_]{1,31}$', f['id']):
            left.append(dict(id=f['id'], name=f.get('name'), why='not an .rfm1 file: %s' % r.get('fileName')))
            continue
        tried.append(f)
    log('build_move_catalog: %d .rfm1 firmwares in the catalog (%s)' % (len(tried), cat.get('generated')))

    # ---- the .rfm1 files: the site's (the catalog's own URL), else RFM1Manager/out's with the same SHA-256
    def get_rfm1(f):
        r = f['rfm1']
        dest = os.path.join(cache, 'rfm1', r['fileName'])
        if os.path.exists(dest) and sha256(dest) == r['sha256']:
            return dest, 'cache'
        url = SITE + r['file'].lstrip('/')
        try:
            fetch(url, dest)
            if sha256(dest) == r['sha256']:
                return dest, url
            os.replace(dest, dest + '.bad')
            why = 'the site\'s file is not the catalog\'s SHA-256'
        except Exception as e:
            why = 'cannot fetch %s: %s' % (url, e)
        local = os.path.join(MANAGER_OUT, r['fileName'])
        if os.path.exists(local) and sha256(local) == r['sha256']:
            shutil.copyfile(local, dest)
            return dest, 'RFM1Manager/out'
        return None, why + '; no local copy with that SHA-256'

    inputs = {}
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        for f, (path, src) in zip(tried, ex.map(get_rfm1, tried)):
            if path:
                inputs[f['id']] = (path, src)
            else:
                left.append(dict(id=f['id'], name=f['name'], version=f['rfm1']['version'], why='unpublished: ' + src))

    # ---- build and check each in the image (tools/catalog_fw.sh), several at once
    lst = os.path.join(cache, 'list.txt')
    with open(lst, 'w') as fh:
        for fid, (path, _) in inputs.items():
            fh.write(os.path.relpath(path, REPO) + '\n')
    built = os.path.relpath(os.path.join(cache, 'built'), REPO)
    log('build_move_catalog: building %d in the image, %d at once' % (len(inputs), args.jobs))
    subprocess.run(['docker', 'run', '--rm', '-v', REPO + ':/w', '-w', '/w', IMAGE, 'sh', '-c',
                    'make -s in-tools in-ref >/dev/null && xargs -P %d -I{} tools/catalog_fw.sh {} %s < %s'
                    % (args.jobs, built, os.path.relpath(lst, REPO))], check=True)

    # ---- the catalog for the Move
    staged = out + '.new'
    if os.path.exists(staged):   # (an interrupted run's)
        shutil.rmtree(staged)
    os.makedirs(staged)
    entries, passed = [], []
    for f in tried:
        if f['id'] not in inputs:
            continue
        path, src = inputs[f['id']]
        r = f['rfm1']
        d = os.path.join(cache, 'built', r['sha256'])
        try:
            res = json.load(open(os.path.join(d, 'result.json')))
        except (OSError, ValueError):
            res = dict(ok=False, reason='no build result')
        if not res.get('ok'):
            left.append(dict(id=f['id'], name=f['name'], version=r['version'], why=res.get('reason'),
                             fwrun=res.get('fwrun'), wasm3=res.get('wasm3')))
            continue
        names = sorted(os.listdir(os.path.join(d, 'files')))
        if any(n.lower().endswith('.syx') for n in names):
            sys.exit('build_move_catalog: a .syx file in %s' % d)
        so = [n for n in names if n.endswith('.rfm1m')][0]
        base = so[:-len('.rfm1m')]
        order = [so] + [n for n in names if n not in (so, base + '.json')] + [base + '.json']
        files = []
        for n in order:
            p = os.path.join(d, 'files', n)
            shutil.copyfile(p, os.path.join(staged, n))
            files.append(dict(name=n, sha256=sha256(p), size=os.path.getsize(p)))
        man = json.load(open(os.path.join(d, 'files', base + '.json')))['manifest']
        card = cards.get(f['id'], {})
        thumb = None
        if card.get('image'):
            try:
                thumb = base64.b64encode(atkinson64(open(os.path.join(FW_REPO, 'catalog', card['image']), 'rb')
                                                    .read())).decode()
            except Exception as e:
                log('build_move_catalog: %s: thumbnail: %s' % (f['id'], e))
        fcats = [c if c in CATS else 'Other' for c in (f.get('categories') or [f.get('category') or 'Other'])]
        main_cat = f.get('category') if f.get('category') in CATS else fcats[0]
        bits = 0
        for c in fcats:
            bits |= 1 << CATS.index(c)
        e = dict(id=f['id'], name=f['name'], version=man.get('version') or r['version'],
                 categories=[c for c in CATS if bits >> CATS.index(c) & 1], cats=bits, cat=CATS.index(main_cat),
                 thumb=thumb,
                 description=ascii_text(f.get('what') or f.get('tag') or ''), tag=ascii_text(f.get('tag') or ''),
                 author=f.get('author') or '', license=man.get('license') or f.get('license') or '',
                 source_url=man.get('source_url') or r.get('sourceUrl') or '', beta=bool(f.get('beta')),
                 hosted_only=r['fileName'] in hosted, rfm1=dict(file=r['fileName'], sha256=r['sha256'], size=r['size']),
                 rfm1m=dict(name=so, sha256=files[0]['sha256'], size=files[0]['size']),
                 skin=[n for n in names if '.rfm1skin' in n], licenses=base + '.licenses.txt' in names,
                 files=files)
        entries.append(e)
        passed.append(dict(id=f['id'], name=f['name'], version=e['version'], source=src, fwrun=res.get('fwrun'),
                           note=res.get('reason') or '',
                           hosted_only=e['hosted_only'], bytes=sum(x['size'] for x in files),
                           rfm1m=files[0]['size'], thumb=bool(thumb)))
    entries.sort(key=lambda e: (e['name'].lower(), e['id']))
    index = dict(schema=1, generated=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                 catalog_generated=cat.get('generated'), source=args.catalog if '://' in args.catalog else 'file',
                 arch='linux-aarch64', firmwares=entries)
    # nothing but the time stamps changed since the last build: keep its index byte for byte, so its sha256 (and the
    # signature, Ed25519 being deterministic) stay and publish_move_catalog.py uploads nothing (RFM1Manager's request)
    prev_bytes = None
    try:
        with open(os.path.join(out, 'index.json'), 'rb') as fh:
            prev_bytes = fh.read()
        prev = json.loads(prev_bytes)
        strip = lambda d: {k: v for k, v in d.items() if k not in ('generated', 'catalog_generated')}
        if strip(prev) != strip(json.loads(json.dumps(index))):
            prev_bytes = None
    except (OSError, ValueError):
        prev_bytes = None
    if prev_bytes is not None:
        log('build_move_catalog: no firmware changed: the previous index.json kept (%s)' % prev.get('generated'))
        with open(os.path.join(staged, 'index.json'), 'wb') as fh:
            fh.write(prev_bytes)
        index['generated'] = prev.get('generated')
    else:
        with open(os.path.join(staged, 'index.json'), 'w') as fh:
            json.dump(index, fh, separators=(',', ':'), ensure_ascii=True)
            fh.write('\n')
    if os.path.exists(out):   # (the previous build: this tool's own output, replaced whole)
        os.replace(out, out + '.prev')
    os.replace(staged, out)
    if os.path.exists(out + '.prev'):
        shutil.rmtree(out + '.prev')

    # ---- the report
    total = sum(os.path.getsize(os.path.join(out, n)) for n in os.listdir(out))
    rep = dict(generated=index['generated'], catalog=cat.get('generated'), tried=len(tried), fetched=len(inputs),
               passed=len(entries), left_out=sorted(left, key=lambda x: x['id']), firmwares=passed,
               total_bytes=total, rfm1m_bytes=sum(p['rfm1m'] for p in passed), seconds=round(time.time() - t0))
    with open(out + '-report.json', 'w') as fh:
        json.dump(rep, fh, indent=1)
    with open(out + '-report.md', 'w') as fh:
        fh.write('# The Move catalog build (%s)\n\n' % index['generated'])
        fh.write('From RemoteFM1\'s catalog of %s: %d .rfm1 firmwares tried, %d fetched, **%d passed** (built, booted, '
                 'sounded, the same PCM as wasm3), %d left out. %s in %d files, %.1f MB (the .rfm1m objects %.1f MB).\n\n'
                 % (cat.get('generated'), len(tried), len(inputs), len(entries), len(left), os.path.basename(out),
                    len(os.listdir(out)), total / 1e6, rep['rfm1m_bytes'] / 1e6))
        fh.write('## Left out\n\n| Firmware | Version | Why |\n|---|---|---|\n')
        for x in rep['left_out']:
            fh.write('| %s (%s) | %s | %s |\n' % (x.get('name'), x['id'], x.get('version', ''), x['why']))
        fh.write('\n## In the catalog\n\n| Firmware | Version | Hosted only | .rfm1m | Thumbnail | The check (fwrun) |\n'
                 '|---|---|---|---|---|---|\n')
        for p in sorted(passed, key=lambda p: p['name'].lower()):
            fh.write('| %s (%s) | %s | %s | %.2f MB | %s | %s |\n' % (p['name'], p['id'], p['version'],
                     'yes' if p['hosted_only'] else '', p['rfm1m'] / 1e6, 'yes' if p['thumb'] else 'no',
                     (p['fwrun'] or '').split(': ', 1)[-1] + (' (%s)' % p['note'] if p['note'] else '')))
    log('build_move_catalog: %d tried, %d passed, %d left out; %s (%.1f MB); report %s-report.md'
        % (len(tried), len(entries), len(left), out, total / 1e6, out))


if __name__ == '__main__':
    main()
