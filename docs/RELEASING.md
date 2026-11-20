# Releasing RemoteFM1 for the Move

A release is two things on GitHub, both in `Quixotic7/remoteFM14Move`:

- **The module:** the release `vX.Y.Z` with one asset, `remotefm1-module.tar.gz` (Schwung's `<id>-module.tar.gz`,
  one `remotefm1/` folder), and `release.json` on `main` pointing at it. Schwung Manager reads `release.json` from
  `main` (not the releases API), both for **Install Custom Module > From GitHub URL** and for a catalog listing
  (schwung `docs/MODULES.md`, "release.json").
- **The Move catalog:** the release `move-catalog` (a fixed tag), whose assets are `index.json`, `index.json.sig` and
  every firmware's files. The host fetches it from `src/catalog.h`'s `CAT_URL_DEFAULT`. The module bundles a snapshot
  of the same signed index, so the catalog must be published **before** the module that carries its snapshot.

## Why there is no release workflow

Move Remote builds its package on a tag in GitHub Actions. RemoteFM1 cannot, without secrets or private inputs:

- the catalog's index is signed with the owner's Ed25519 key (`~/.config/remotefm14move/catalog-signing-key.pem`,
  never in git), and `make package` refuses a bundled index without a good signature;
- `make` needs input `.rfm1` files that are never in git (`FIRMWARES`, `TEST_FIRMWARES`: RFM1Manager's builds beside
  the repo) and FM1VST beside the repo for the wasm3 reference;
- `make catalog` fetches and checks 118 firmwares in the linux/arm64 image (~20 minutes the first time, then cached on
  the Mac).

A workflow could later rebuild the module only (fetching the published, already-signed `move-catalog` assets for the
bundle), but the release is cut by hand on the owner's Mac for now.

## Steps

1. **The version.** Set it in `src/main.c` (`#define VERSION`), `module/module.json` (`version`) and `release.json`
   (`version` and the tag in `download_url`); add the release's notes to `CHANGES.md` (user-facing). `make
   version-check` (part of `make package`) refuses a mismatch.
2. **The catalog** (when firmwares changed, or the build glue):

   ```sh
   make catalog                                        # dist/move-catalog, signed; report dist/move-catalog-report.md
   python3 tools/publish_move_catalog.py --dry-run     # what would go up
   ```

   Never a raw `.syx`; excluded firmwares are never listed (tools/build_move_catalog.py).
3. **The tests and the package:**

   ```sh
   make test                                           # all green
   make package                                        # dist/remotefm1-module.tar.gz (bundles from dist/move-catalog)
   shasum -a 256 dist/remotefm1-module.tar.gz > dist/remotefm1-module.tar.gz.sha256
   ```

   Run the browser emulator test too when the page changed (RemoteFM1's build_web.sh + test_web_emu.mjs).
4. **Publish, in this order** (each step only after the one before it succeeded):

   ```sh
   python3 tools/publish_move_catalog.py               # the catalog first: the package's snapshot names its files
   git tag -a vX.Y.Z -m "RemoteFM1 for the Move X.Y.Z"
   git push origin vX.Y.Z
   gh release create vX.Y.Z dist/remotefm1-module.tar.gz dist/remotefm1-module.tar.gz.sha256 \
       --repo Quixotic7/remoteFM14Move --title "RemoteFM1 for the Move X.Y.Z" --notes-file <notes> --latest
   git push origin phase1:main                          # release.json on main last: its URL now exists
   ```

   Large pushes can drop ("unexpected disconnect"): retry with `git -c http.postBuffer=524288000 push ...` and check
   with `git ls-remote origin`.
5. **Check:**

   ```sh
   curl -sL https://raw.githubusercontent.com/Quixotic7/remoteFM14Move/main/release.json
   curl -sIL "$(curl -sL https://raw.githubusercontent.com/Quixotic7/remoteFM14Move/main/release.json | \
       python3 -c 'import json,sys; print(json.load(sys.stdin)["download_url"])')" | grep -i '^HTTP\|content-length'
   ```

   Then on a Move: Schwung Manager > Modules > Install Custom Module > From GitHub URL, `Quixotic7/remoteFM14Move`.
6. **Elsewhere:** RemoteFM1's `/move` page data (`web/site/move/data/move.json`: released, version, release URL, the
   bundled count and size), and the Schwung catalog entry's description when the listing exists.

The `move-catalog` release must not become the repository's "Latest": create each module release with `--latest`.
