# Draft: listing RemoteFM1 in Schwung Manager

**Not sent.** For the owner to review and send.

## The process

Schwung Manager's module list is `module-catalog.json` on `main` of
[charlesvestal/schwung](https://github.com/charlesvestal/schwung), served live: an entry merged there shows in every
Schwung Manager at once (schwung `CONTRIBUTING.md`). Modules are added by a **pull request** that adds one entry to
that file, not by an issue: Move Remote went in that way (charlesvestal/schwung#718, "Add Move Remote to the module
catalog", from the fork `Quixotic7/schwung`, branch `add-move-remote`). Schwung's `CONTRIBUTING.md` asks the PR to say
which AI coding tools were used. Before opening it, run the catalog checks Move Remote's PR ran:
`tests/host/test_taxonomy.sh`, `test_module_categories.sh`, `test_release_version_agreement.sh` and
`test_host_channels_mirror_stable.sh`.

Open it only after v0.1.1 (with help.json) is released and `release.json` is on `main` (Schwung Manager fetches it for the version and
the download).

```sh
# in a clone of Quixotic7/schwung, up to date with charlesvestal/schwung main
git switch -c add-remotefm1
# add the entry below at the end of "modules" in module-catalog.json, then run the four tests above
git commit -am "Add RemoteFM1 to the module catalog"
git push -u origin add-remotefm1
gh pr create --repo charlesvestal/schwung --base main --head Quixotic7:add-remotefm1 \
    --title "Add RemoteFM1 to the module catalog" --body-file <the PR text below>
```

## The entry (module-catalog.json)

```json
{
  "id": "remotefm1",
  "name": "RemoteFM1",
  "description": "FM-1 firmwares on the Move itself: 118 synths and grooveboxes from the RemoteFM1 catalog, three at once, played from the pads, knobs and sequencer. Schwung MIDI and audio FX inside; a browser page at remotefm1.local:7730; an FM-1 on USB-A or Wi-Fi as its panel",
  "author": "Quixotic7",
  "component_type": "tool",
  "subcategory": "performance",
  "github_repo": "Quixotic7/remoteFM14Move",
  "default_branch": "main",
  "asset_name": "remotefm1-module.tar.gz",
  "min_host_version": "1.6.0"
}
```

## The PR text

> Adds **RemoteFM1** ([Quixotic7/remoteFM14Move](https://github.com/Quixotic7/remoteFM14Move), MPL-2.0) to the
> catalog.
>
> It is a standalone tool that runs FM-1 firmwares (RemoteFM1's `.rfm1` files) on the Move itself, as native code
> (wasm2c), played from the Move's own pads, knobs, buttons and sequencer.
>
> - **Firmwares:** a signed catalog of 118. 39 come in the
>   package; the rest download over Wi-Fi from a browser on the Move or on the page. Each keeps its own licence; the
>   package's `FIRMWARES.md` lists the bundled ones with their exact sources.
> - **Three slots** at once, each on its own thread and core.
> - **Schwung FX:** the user's installed MIDI FX and audio FX run inside it (3 MIDI FX in series, 3 audio FX on
>   send / return buses), through `midi_fx_api_v1` and `audio_fx_api_v2`.
> - **The page** at `remotefm1.local:7730`: the FM-1 faceplate live, the firmware's Editor, Mix & FX, a file browser
>   for its folder. An FM-1 on the Move's USB-A (or over Wi-Fi) works as a panel.
> - **Leaving it:** Shift + volume touch + jog click returns to Move.
>
> The entry:
>
> - `component_type`: `tool`, `subcategory`: `performance`.
> - `min_host_version`: `1.6.0`: the first release with top-level `"standalone": true`. Its real-time helper goes
>   through `schwung-heal`, as Move Remote's does.
> - `asset_name`: `remotefm1-module.tar.gz`, from the v0.1.1 release, with `release.json` on `main`. It installs today
>   through Install Custom Module > From GitHub URL.
>
> Checks run locally: `tests/host/test_taxonomy.sh`, `test_module_categories.sh`, `test_release_version_agreement.sh`
> and `test_host_channels_mirror_stable.sh`.
>
> Tested daily on a Move with Schwung 1.9.3: Install from GitHub, launch from Tools and exit back to Move, three
> firmware slots, catalog downloads and updates, Schwung MIDI and audio FX, the page at remotefm1.local:7730, and an
> FM-1 as the panel over USB-A and Wi-Fi.
>
> It ships a `help.json` (on-device help: modes, the firmware view, FX & Mix, MIDI, the sequencer, leaving).
> Screen-reader support is not there yet.
>
> Written with Claude Code.

## Screenshots (if asked, or for the PR)

From `docs/` in the repo (the page on the Move):

- `move-page.png`: the page, the faceplate with a firmware running
- `move-page-slots.png`: the three slots
- `move-page-browser.png`, `move-page-browser-all.png`: the firmware browser
- `move-page-mixfx.png`, `move-page-fxpages.png`: Mix & FX, Schwung FX pages
- `move-page-editor.png`, `move-page-editor-felucca.png`: a firmware's Editor
- `move-page-files.png`: the file browser
- `move-page-phone.png`, `move-page-phone-wifi.png`: on a phone; the FM-1's Wi-Fi setup

A photo of the Move's own screen with a firmware running would show the on-device side; there is none in the repo yet.

## Open points for the owner

- `subcategory`: `performance` (as Move Remote) or `sequencer`; the taxonomy's tool subcategories are sequencer,
  sampling-editing, tuner-analysis, assistant and performance.
- `min_host_version` 1.6.0 is Move Remote's reasoning (top-level `standalone`). The FX APIs RemoteFM1 hosts
  (`audio_fx_api_v2.h`, `midi_fx_api_v1.h`) are in Schwung since January 2026, well before 1.6.0 (September 2026).
  Nothing here records which Schwung versions RemoteFM1 was tried on; fill the PR's on-device line.
- Schwung's `CONTRIBUTING.md` asks new modules for a `help.json` (on-device help) and screen-reader support. Move
  Remote was merged without them; RemoteFM1 has neither.
- The on-device test list in the PR text.
