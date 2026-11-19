// SPDX-License-Identifier: MPL-2.0
// The Move's page end to end, on the Mac: the host on Move Remote's simulator (the build image, its :7730 published on
// 127.0.0.1), RemoteFM1's site from it (scripts/build_site.sh), in headless Chromium (no window, a fresh profile; the
// browser and the container ended by their ids) at http://remotefm1.local:PORT/ (the name mapped to 127.0.0.1 inside
// Chromium only), so the page is NOT a secure context, as on a real Move:
//   - every file the page asks for loads (no 4xx), no exception, no console error, no AudioContext, Web MIDI, WebUSB
//     or microphone asked for; no https redirect;
//   - the panel draws, the Move's live screen shows on it, the header names the firmware the Move runs;
//   - a click on a key of the drawn keybed sounds in the simulator (its mix peak);
//   - the header's slot chips (in place of remotefm1.com's firmware picker): a chip's menu lists the Move's firmwares
//     and loads ChoralRoot into the active slot (the host's status); its skin (its .rfm1's rfm1.skin sections) shows;
//     a load from outside the page (as the Move's own projects view) is followed;
//   - the chips play a slot (ACTIVE) and follow the Move's switch; a load into an empty slot and an unload from a
//     chip's menu; ⌥1 ⌥2 ⌥3, ⌥M and Esc (Esc lets a latch go first);
//   - the Mix & FX view (rfm1cs 0.5.0: FX setup as one layer, the chain, and the Mixer): a module's own parameter pages
//     (Page Test: its tabs, the gap, an edit on page 2 into the core, the Move's page followed), and the page's edits
//     (a level fader, a send dial, routing cells, Freeverb into audio FX 1, a
//     parameter) land in the core (GET /api/mix), edits from outside (POST /api/edit, as the Move's knobs) show on
//     the page; the tab follows the Move's FX page and sets it;
//   - the Editor (Felucca's, from fixtures for remotefm1.com and its author's site): a SysEx request from the editor in
//     its sandbox reaches the firmware on the Move, the reply reaches the editor (live page, then the hosted copy);
//   - the firmware browser (js/movebrowse.js over RemoteFM1's js/fw-browser.js): the load menus' thumbnails; Browse all
//     shows the Move's whole catalog (GET /api/catalog) as tiles: RemoteFM1FW's catalog image where it has one (served
//     by the test), else the Move's own 64 x 64 1-bit thumbnail (pixelated); the filters (Favourites or All, All by
//     itself with none; Installed with its updates / Not installed; one category); a tap on AMB-1 (not installed)
//     downloads it on the Move (POST /api/download, from build/testcat served in the container) and loads it into the
//     slot; dist/move-catalog's ~118 firmwares as tiles: drawn as they come into view (lazily), all once scrolled;
//     on the phone the whole screen, names never cut off (move-page-browser.png, move-page-browser-all.png,
//     move-page-phone-browser.png);
//   - Files (js/movefiles.js, js/movefs.js): the sheet's Add samples puts a WAV into the Move's audio/; the header's
//     Files button's browser: the root read-only, a folder made, a file dropped into it, renamed, deleted, a folder
//     deleted with what is in it, firmwares/ read-only; Export states and Export all projects as real downloads;
//   - the phone (390 x 844): the slot button, its menu, the Mix & FX sheet; FM-1 Wi-Fi (js/movewifi.js) from the
//     settings sheet: the USB-A line, a canned list, a password picked and shown (move-page-phone-wifi.png);
//   - screenshots: docs/move-page.png (or --shot=FILE), move-page-slots.png, move-page-fxpages.png, move-page-mixfx.png,
//     move-page-phone.png (the slot menu open), move-page-phone-mixfx.png and move-page-phone-fxpages.png,
//     move-page-editor.png (the Editor), move-page-files.png (the file browser).
//   node tests/page_e2e.mjs [--shot=docs/move-page.png] [--port=N] [--live-images] [--shots-also=DIR]
//   (after make; needs Docker and Chromium. --live-images: RemoteFM1FW's real catalog images from GitHub, not the
//   test's; --shots-also: the browser's screenshots copied there too)
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, existsSync, readdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOT = arg("shot", join(ROOT, "docs/move-page.png"));
const DOCS = join(ROOT, "docs");
const LIVE_IMAGES = process.argv.includes("--live-images"), ALSO = arg("shots-also", null);
const BIG = join(ROOT, "dist/move-catalog/index.json");
const CHROMIUM = process.env.CHROMIUM || "/Applications/Chromium.app/Contents/MacOS/Chromium";
const IMAGE = process.env.IMAGE || "rfm14move-build";
const PORT = +arg("port", 17800 + Math.floor(Math.random() * 100)), DEBUG = 9300 + Math.floor(Math.random() * 600);
const HOSTNAME = "remotefm1.local", BASE = `http://${HOSTNAME}:${PORT}/`, API = `http://127.0.0.1:${PORT}/api/`;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
let fails = 0, checks = 0;
const ok = (c, what) => { checks++; if (!c) fails++; console.log(`  ${c ? "ok  " : "FAIL"}  ${what}`); return c; };

if (!existsSync(CHROMIUM)) { console.log(`page_e2e: skipped (no Chromium at ${CHROMIUM})`); process.exit(0); }
for (const f of ["build/sim/remotefm1-host", "build/site/index.html"])
  if (!existsSync(join(ROOT, f))) { console.error(`page_e2e: ${f} is missing: run make`); process.exit(1); }

// ---- the host on the simulator, in the image
const cid = execFileSync("docker", ["run", "-d", "--rm", "-p", `127.0.0.1:${PORT}:7730`, "-v", `${ROOT}:/w`, "-w", "/w", IMAGE, "sh", "-c",
  // (the test catalog, build/testcat, served in the container: the page's download of AMB-1 comes from it)
  (existsSync(join(ROOT, "build/testcat/index.json")) ? "busybox httpd -p 127.0.0.1:18090 -h build/testcat && " : "") +
  "mkdir -p /tmp/s && exec build/sim/remotefm1-host --root /tmp/r --sim-dir /tmp/s --firmwares build/fw --site build/site --fw felucca --catalog-url " +
  (existsSync(join(ROOT, "build/testcat/index.json")) ? "http://127.0.0.1:18090" : "http://127.0.0.1:1") + " --fx-dir build/fxmods" +
  (existsSync(join(ROOT, "build/testcat/index.json")) ? " --catalog-index build/testcat/index.json" : "")],
  { encoding: "utf8" }).trim();
const tmp = mkdtempSync(join(tmpdir(), "rfm14-page-")), profile = join(tmp, "profile");
let chrome = null;
const stop = () => {
  try { if (chrome) chrome.kill(); } catch {}
  try { execFileSync("docker", ["stop", "-t", "3", cid], { stdio: "ignore" }); } catch {}
};
process.on("exit", stop);
process.on("SIGINT", () => process.exit(130));

const api = async (p, o) => (await fetch(API + p, o)).json();
const status = () => api("status").catch(() => null);
const mixNow = () => api("mix").catch(() => null);
const edit = (op, a, b) => fetch(API + "edit", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ op, a, b }) });
const slotId = async (s) => { const st = await status(); return st && st.slots && st.slots[s] ? st.slots[s].id : null; };

let ws, id = 0;
const waiting = new Map();
const cdp = (method, params = {}) => new Promise((r) => { const i = ++id; waiting.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
async function js(expression, timeout = 30000) {
  const r = await cdp("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true, timeout });
  if (r.result?.exceptionDetails) throw new Error(`page: ${JSON.stringify(r.result.exceptionDetails).slice(0, 400)}`);
  return r.result?.result?.value;
}
async function until(expr, ms, what) {
  const t0 = Date.now();
  while (Date.now() - t0 < ms) { if (await js(`!!(${expr})`).catch(() => false)) return true; await sleep(100); }
  console.log(`  (timed out: ${what})`);
  return false;
}
async function untilApi(f, ms) { const t0 = Date.now(); while (Date.now() - t0 < ms) { if (await f()) return true; await sleep(150); } return false; }
const mouse = (type, x, y) => cdp("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons: type === "mouseReleased" ? 0 : 1, clickCount: 1 });
const click = async (sel) => {
  const r = await js(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; e.scrollIntoView({ block: "center" }); const b = e.getBoundingClientRect(); return { x: b.left + b.width / 2, y: b.top + b.height / 2 }; })()`);
  if (!r) return false;
  await mouse("mouseMoved", r.x, r.y); await mouse("mousePressed", r.x, r.y); await sleep(60); await mouse("mouseReleased", r.x, r.y);
  return true;
};

const box = (sel) => js(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; e.scrollIntoView({ block: "nearest" }); const b = e.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height }; })()`);
const drag = async (x0, y0, x1, y1) => {
  await mouse("mouseMoved", x0, y0); await mouse("mousePressed", x0, y0);
  for (let i = 1; i <= 8; i++) { await mouse("mouseMoved", x0 + (x1 - x0) * i / 8, y0 + (y1 - y0) * i / 8); await sleep(25); }
  await mouse("mouseReleased", x1, y1);
};
// a key, with ⌥ or not (macOS: ⌥ changes e.key; the page reads e.code)
const key = async (code, { alt = false, keyName, vk } = {}) => {
  const k = { type: "keyDown", code, key: keyName || code, windowsVirtualKeyCode: vk || 0, modifiers: alt ? 1 : 0 };
  await cdp("Input.dispatchKeyEvent", k); await cdp("Input.dispatchKeyEvent", { ...k, type: "keyUp" });
};
const altDigit = (n) => key(`Digit${n}`, { alt: true, keyName: ["¡", "™", "£"][n - 1], vk: 48 + n });
const esc = () => key("Escape", { keyName: "Escape", vk: 27 });
const shot = async (file) => {
  const r = await cdp("Page.captureScreenshot", { format: "png" }), b = Buffer.from(r.result.data, "base64");
  writeFileSync(file, b); console.log(`  shot  ${file}`);
  if (ALSO && /browser/.test(file)) writeFileSync(join(ALSO, file.split("/").pop().replace("move-page-", "firmware-browser-move-")), b);
};
const active = async () => { const st = await status(); return st ? st.active : -1; };
const dv = (sel) => js(`(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.dataset.v : null; })()`);

console.log(`page_e2e (${BASE}, container ${cid.slice(0, 12)})`);
try {
  ok(await untilApi(async () => !!(await slotId(0)), 20000), "the host runs on the simulator (its status answers)");
  // the files the page asks for, by URL: their HTTP status
  chrome = spawn(CHROMIUM, ["--headless=new", "--use-mock-keychain", "--password-store=basic", "--disable-gpu",
    "--no-sandbox", "--no-first-run", `--user-data-dir=${profile}`, "--hide-scrollbars", "--window-size=1280,800",
    "--mute-audio", `--host-resolver-rules=MAP ${HOSTNAME} 127.0.0.1`, `--remote-debugging-port=${DEBUG}`, "about:blank"], { stdio: "ignore" });
  let targets;
  for (let i = 0; i < 50 && !targets; i++) { await sleep(200); try { targets = await (await fetch(`http://127.0.0.1:${DEBUG}/json/list`)).json(); } catch {} }
  ws = new WebSocket(targets.find((t) => t.type === "page").webSocketDebuggerUrl);
  await new Promise((r, j) => { ws.onopen = r; ws.onerror = j; });
  const exceptions = [], consoleErrors = [], responses = new Map(), failed = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) { waiting.get(d.id)(d); waiting.delete(d.id); }
    if (d.method === "Runtime.exceptionThrown") exceptions.push(JSON.stringify(d.params.exceptionDetails).slice(0, 300));
    if (d.method === "Runtime.consoleAPICalled" && d.params.type === "error") consoleErrors.push(d.params.args.map((a) => a.value ?? a.description).join(" ").slice(0, 300));
    if (d.method === "Log.entryAdded" && d.params.entry.level === "error") consoleErrors.push(`${d.params.entry.text} ${d.params.entry.url || ""}`.slice(0, 300));
    if (d.method === "Network.responseReceived") responses.set(d.params.response.url, d.params.response.status);
    if (d.method === "Network.loadingFailed" && !d.params.canceled) failed.push(d.params.errorText);
    if (d.method === "Fetch.requestPaused") {
      const u = d.params.request.url, f = FIX[u];
      fetched.push(u);
      const headers = [{ name: "Content-Type", value: f ? f[0] : "text/plain" }, { name: "Access-Control-Allow-Origin", value: "*" }];
      cdp("Fetch.fulfillRequest", { requestId: d.params.requestId, responseCode: f ? 200 : 404, responseHeaders: headers,
        body: Buffer.from(f ? f[1] : "not here").toString("base64") });
    }
  };
  // remotefm1.com (its catalog's editor fields, its managers index, a hosted copy) and an author's editor site, served
  // by the test (no network needed): Felucca's editor is a fixture that asks the firmware for INFO and, once the reply
  // comes, sends PING (so the reply reached the editor inside its sandbox)
  const EDITOR = `<!doctype html><html><head><meta charset="utf-8"><title>Felucca editor (test fixture)</title>%SHIM%</head><body>
<p id="s">waiting</p><script>
navigator.requestMIDIAccess({ sysex: true }).then((a) => {
  const out = [...a.outputs.values()][0], inp = [...a.inputs.values()][0];
  inp.onmidimessage = (e) => { const d = e.data; if (d[0] === 0xF0 && d[1] === 0x7D && d[4] === 1) { document.getElementById("s").textContent = "INFO " + d.length; out.send([0xF0, 0x7D, 0x46, 0x4C, 0x18, 0xF7]); } };
  out.send([0xF0, 0x7D, 0x46, 0x4C, 0x01, 0xF7]);
});
</script></body></html>`;
  const FIX = {
    "https://remotefm1.com/catalog.json": ["application/json", JSON.stringify({ firmwares: [
      { id: "felucca", editor: "https://editor.test/felucca/", port: "Felucca", repo: "hugelton/Felucca" },
      { id: "choralroot", editor: "https://quixotic7.example/manager.html" }] })],
    "https://remotefm1.com/managers/index.json": ["application/json", JSON.stringify({ ports: { felucca: "Felucca" }, managers: [
      { id: "felucca", title: "Felucca editor", firmwares: ["felucca"], path: "felucca/", author: "Leo Kuroshita", license: "GPL-3.0-only",
        source: "https://github.com/hugelton/Felucca", upstream: "https://editor.test/felucca/" }] })],
    "https://editor.test/felucca/": ["text/html; charset=utf-8", EDITOR.replace("%SHIM%", "")],
    // (a hosted copy carries its own shim's tag, which must not load in the sandbox: it is dropped)
    "https://remotefm1.com/managers/felucca/": ["text/html; charset=utf-8", EDITOR.replace("%SHIM%", `<script src="../fm1-shim.js" data-manager="felucca"></script>`)],
  };
  // RemoteFM1FW's catalog (the browser's thumbnails): an image for AMB-1 only (an SVG stands in for its PNG)
  const RFW = "https://raw.githubusercontent.com/Quixotic7/RemoteFM1FW/main/catalog/";
  FIX[RFW + "catalog.json"] = ["application/json", JSON.stringify({ schema: 1, firmwares: { amb: { image: "images/amb.svg", category: "Synth" } } })];
  FIX[RFW + "images/amb.svg"] = ["image/svg+xml", `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><defs><linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#2a6fdb"/><stop offset="1" stop-color="#f2b134"/></linearGradient></defs><rect width="64" height="64" fill="url(#g)"/><circle cx="22" cy="26" r="9" fill="#fff" opacity=".8"/><circle cx="42" cy="38" r="13" fill="#fff" opacity=".45"/></svg>`];
  const fetched = [];
  await cdp("Fetch.enable", { patterns: [{ urlPattern: "https://remotefm1.com/*" }, { urlPattern: "https://editor.test/*" },
    ...(LIVE_IMAGES ? [] : [{ urlPattern: "https://raw.githubusercontent.com/*" }])] });
  await cdp("Runtime.enable"); await cdp("Page.enable"); await cdp("Network.enable"); await cdp("Log.enable");
  await cdp("Emulation.setDeviceMetricsOverride", { width: 1280, height: 800, deviceScaleFactor: 1, mobile: false });
  // what the page asks the browser for (none of it may be used on the Move: no secure context, no audio here)
  await cdp("Page.addScriptToEvaluateOnNewDocument", { source: `(() => {
    const asked = window.__asked = [];
    const wrap = (o, k, name) => { try { if (o && typeof o[k] === "function") { const f = o[k]; o[k] = function (...a) { asked.push(name); return f.apply(this, a); }; } } catch (_) {} };
    wrap(navigator, "requestMIDIAccess", "requestMIDIAccess");
    if (navigator.usb) { wrap(navigator.usb, "requestDevice", "usb.requestDevice"); wrap(navigator.usb, "getDevices", "usb.getDevices"); }
    if (navigator.mediaDevices) wrap(navigator.mediaDevices, "getUserMedia", "getUserMedia");
    for (const k of ["AudioContext", "webkitAudioContext"]) if (window[k]) { const C = window[k]; window[k] = function (...a) { asked.push(k); return new C(...a); }; window[k].prototype = C.prototype; }
    if (window.showDirectoryPicker) wrap(window, "showDirectoryPicker", "showDirectoryPicker");
  })();` });
  await cdp("Page.navigate", { url: BASE });
  ok(await until(`window.FM1Site && FM1Site.current().info && FM1Site.current().info.id === "felucca" && FM1Device.L`, 30000, "the page"),
    "the page loads, its engine is the Move: Felucca's info over /ws");
  await sleep(1500);
  const page = await js(`({ secure: isSecureContext, href: location.href, host: document.documentElement.dataset.host,
    meta: document.querySelector('meta[name="fm1-host"]').content, title: document.title,
    name: document.getElementById("fw-name").textContent, ver: document.getElementById("fw-ver").textContent,
    hidden: ["install-link", "vst-link", "ios-link", "android-link", "fm1-btn", "rec", "fw-btn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent === null; }).length,
    shown: ["mv-chips", "fw-src", "site-src", "menu-btn", "mv-mixbtn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent !== null; }).length,
    start: document.getElementById("start").hidden, asked: window.__asked.slice(),
    plate: (() => { const c = document.getElementById("device"); return [c.width, c.height]; })(),
    lit: (() => { const p = FM1Site.screen().pixels; let n = 0; for (const v of p) if (v) n++; return n; })(),
    seq: FM1Site.screen().seq, fonts: [...document.fonts].filter((f) => f.status === "loaded").map((f) => f.family) })`);
  ok(page.href === BASE && page.secure === false, `plain http on ${HOSTNAME}: not a secure context, no https redirect (${page.href})`);
  ok(page.host === "move" && page.meta === "move", "the host flag: move (index.html's meta, <html data-host>)");
  ok(page.name === "Felucca" && page.ver === "1.8.2" && /RemoteFM1 on Move/.test(page.title), `the header names what the Move runs: ${page.name} ${page.ver} (${page.title})`);
  ok(page.hidden === 7 && page.shown === 5, `the header: slot chips, Source, Site source, settings, Mix & FX shown; the firmware picker, Install, app links, FM-1, recorder hidden (${page.shown}/5, ${page.hidden}/7)`);
  ok(page.start === true, "no Play overlay (no audio to start in the browser)");
  ok(page.asked.length === 0, `nothing asked of the browser: no AudioContext, Web MIDI, WebUSB, microphone, folder (${page.asked.join(", ") || "none"})`);
  ok(page.plate[0] > 1000 && page.lit > 2000 && page.seq > 0, `the panel drawn (${page.plate.join(" x ")}), the Move's screen live on it (${page.lit} pixels lit, frame ${page.seq})`);
  ok(["Chakra Petch", "DM Sans"].every((f) => page.fonts.includes(f)), `the site's fonts loaded (${[...new Set(page.fonts)].join(", ")})`);

  // ---- a click on a key of the drawn keybed: it sounds in the simulator
  await sleep(1200);
  const q = (await status()).peak;
  const at = await js(`(() => { const w = FM1Device.where(0, 12), r = document.getElementById("device").getBoundingClientRect(); return w && { x: r.left + w.x, y: r.top + w.y }; })()`);
  await mouse("mouseMoved", at.x, at.y); await mouse("mousePressed", at.x, at.y);
  const loud = await untilApi(async () => (await status()).peak > 1000, 3000);
  const p = (await status()).peak;
  await mouse("mouseReleased", at.x, at.y);
  ok(loud, `a click on key 12 of the page's keybed sounds on the Move (mix peak ${q} -> ${p})`);
  const ledsLit = await js(`(() => { let n = 0; for (const v of FM1Site.leds().levels) if (v) n++; return n; })()`);
  ok(ledsLit > 0, `the Move's FM-1 LEDs on the page (${ledsLit} lit)`);

  // ---- the slot chips: slot 1's menu lists the Move's firmwares; ChoralRoot into the active slot
  ok(await until(`document.querySelector('.mv-chip[data-slot="0"].on') && document.querySelectorAll('.mv-chip.empty').length === 2`, 5000, "the chips"),
    "the header's chips: slot 1 playing (Felucca), slots 2 and 3 empty");
  await click('.mv-chip[data-slot="0"] .mv-chev');
  await until(`document.querySelector(".mv-pop .mv-pop-item[data-id]")`, 5000, "slot 1's menu");
  const cards = await js(`[...document.querySelectorAll(".mv-pop .mv-pop-item[data-id]")].map((c) => c.dataset.id)`);
  ok(cards.join() === "choralroot,felucca", `a chip's menu lists the Move's firmwares: ${cards.join(", ")}`);
  const fileCard = await js(`!!document.querySelector(".sheet .card.file")`);
  ok(!fileCard, "no .rfm1 file opening in the browser");
  await click('.mv-pop .mv-pop-item[data-id="choralroot"]');
  ok(await untilApi(async () => (await slotId(0)) === "choralroot", 15000), `the pick loaded ChoralRoot into the Move's slot 1 (${await slotId(0)})`);
  ok(await until(`FM1Site.current().info && FM1Site.current().info.id === "choralroot" && document.body.classList.contains("skinned")`, 10000, "ChoralRoot's info and skin"),
    "the page shows ChoralRoot from the Move, with its skin (its .rfm1's rfm1.skin sections)");
  await sleep(1500);
  const cr = await js(`({ name: document.getElementById("fw-name").textContent, body: getComputedStyle(document.documentElement).getPropertyValue("--body").trim(),
    labels: FM1Site.current().info && FM1Site.current().info.buttonNames, seq: FM1Site.screen().seq,
    chip: document.querySelector('.mv-chip[data-slot="0"] .mv-nm').textContent })`);
  ok(cr.name === "ChoralRoot" && cr.chip === "ChoralRoot" && /201,\s*162,\s*107|#c9a26b/i.test(cr.body), `ChoralRoot in the header and its chip, the skin's colours on the page (--body ${cr.body})`);
  ok(Array.isArray(cr.labels) && cr.labels.length === 14, `its button names from the Move (${(cr.labels || []).join(" ")})`);
  await cdp("Page.captureScreenshot", { format: "png" }).then((r) => { writeFileSync(SHOT, Buffer.from(r.result.data, "base64")); console.log(`  shot  ${SHOT}`); });

  // ---- the Move switches by itself (as its projects view does; here the same load from outside the page): the page follows
  await fetch(API + "load", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ slot: 0, id: "felucca" }) });
  ok(await until(`document.getElementById("fw-name").textContent === "Felucca" && FM1Site.current().info && FM1Site.current().info.id === "felucca" && !document.body.classList.contains("skinned")`, 15000, "the follow"),
    "the Move loaded Felucca by itself: the page follows (its name, its info, ChoralRoot's skin gone)");
  // ---- the Editor (the firmware's own editor, as on remotefm1.com): the header's button (remotefm1.com's editor list,
  // read live); the author's page in the panel's sandbox, its SysEx to Felucca on the Move over /ws (op 32), Felucca's
  // reply back (op 5) into the editor, which answers it; then remotefm1.com's hosted copy ("Use hosted copy") the same
  ok(await until(`!document.getElementById("ed-btn").hidden && document.getElementById("ed-btn").offsetParent`, 10000, "the Editor button"),
    `the header's Editor button for Felucca (remotefm1.com's catalog and managers index: ${fetched.filter((u) => /catalog|index/.test(u)).length} fetched)`);
  const st0 = await status();
  await click("#ed-btn");
  const roundTrip = async (what) => {
    const got = await until(`(() => { const b = FM1Editor.bridge, i = b.stats.lastIn, o = b.stats.lastOut;
      return b.count >= 1 && o && o[0] === 0xF0 && o[1] === 0x7D && o[4] === 1 && o[o.length - 1] === 0xF7 && i && i[4] === 0x18; })()`, 15000, what);
    const r = await js(`(() => { const b = FM1Editor.bridge, o = b.stats.lastOut; return { n: o ? o.length : 0, v: o ? String.fromCharCode(...o.slice(5, 5 + Math.max(0, o.indexOf(0, 5) - 5))) : "",
      sub: document.getElementById("ed-sub").textContent, dot: document.getElementById("ed-dot").className }; })()`);
    return { got, ...r };
  };
  const live = await roundTrip("the live editor's round trip");
  await untilApi(async () => { const t = await status(); return t.pmidi_msgs >= st0.pmidi_msgs + 2 && t.midi_out_bytes > st0.midi_out_bytes; }, 3000);
  const st1 = await status();
  ok(live.got && st1.pmidi_msgs >= st0.pmidi_msgs + 2 && st1.midi_out_bytes > st0.midi_out_bytes,
    `the author's editor (editor.test, live in the sandbox): its INFO request reached Felucca on the Move, the reply (${live.n} bytes, "${live.v}") reached the editor, which answered (PING); ${st1.pmidi_msgs - st0.pmidi_msgs} messages in, ${st1.midi_out_bytes - st0.midi_out_bytes} bytes out (${live.sub})`);
  await shot(join(DOCS, "move-page-editor.png"));
  await js(`FM1Editor.bridge.stats.lastIn = FM1Editor.bridge.stats.lastOut = null`);
  ok(await until(`!document.getElementById("ed-mode").hidden && document.getElementById("ed-mode").textContent === "Use hosted copy"`, 5000, "Use hosted copy"), "the panel offers the hosted copy");
  await click("#ed-mode");
  const hosted = await roundTrip("the hosted copy's round trip");
  ok(hosted.got && fetched.includes("https://remotefm1.com/managers/felucca/") && !fetched.some((u) => /fm1-shim/.test(u) && /remotefm1\.com/.test(u)),
    `remotefm1.com's hosted copy in the same sandbox (its own shim's tag dropped): the same round trip (${hosted.n} bytes back; ${hosted.sub})`);
  await click("#ed-mode");   // (back to the live page, remembered per firmware otherwise)
  await click("#ed-close");
  ok(await until(`document.getElementById("ed-panel").hidden`, 3000, "the panel closed"), "the Editor panel closes");

  // ---- the slots: a load into an empty slot from its chip, the chips play a slot, they follow the Move's switch
  ok(await until(`document.querySelector('.mv-chip[data-slot="1"].empty')`, 3000, "slot 2 empty"), "slot 2's chip: empty, dashed (\"Load into 2\")");
  await click('.mv-chip[data-slot="1"] .mv-chip-main');
  await until(`document.querySelector('.mv-pop .mv-pop-item[data-id="choralroot"]')`, 5000, "slot 2's menu");
  const where = await js(`document.querySelector('.mv-pop .mv-pop-item[data-id="felucca"]').textContent`);
  ok(/on 1/.test(where), `the menu marks a firmware loaded on another slot (${where})`);
  await click('.mv-pop .mv-pop-item[data-id="choralroot"]');
  ok(await untilApi(async () => (await slotId(1)) === "choralroot" && (await active()) === 1, 15000), `ChoralRoot loaded into the empty slot 2 from its chip, and plays (active ${await active()})`);
  ok(await until(`document.querySelector('.mv-chip[data-slot="1"].on') && document.getElementById("fw-name").textContent === "ChoralRoot"`, 10000, "chip 2 on"),
    "chip 2 lit, the faceplate follows it (ChoralRoot)");
  await click('.mv-chip[data-slot="0"] .mv-chip-main');
  ok(await untilApi(async () => (await active()) === 0, 5000), `a tap on chip 1 plays slot 1 on the Move (active ${await active()})`);
  ok(await until(`document.querySelector('.mv-chip[data-slot="0"].on') && document.getElementById("fw-name").textContent === "Felucca"`, 10000, "chip 1 on"), "chip 1 lit, the faceplate shows Felucca");
  await edit(16, 1, 0);   // (the Move switches: its own track change, the same core path)
  ok(await until(`document.querySelector('.mv-chip[data-slot="1"].on') && !document.querySelector('.mv-chip[data-slot="0"].on') && document.getElementById("fw-name").textContent === "ChoralRoot"`, 10000, "the follow"),
    "the Move switched to slot 2 itself: the chips and the faceplate follow");
  await altDigit(1);
  ok(await untilApi(async () => (await active()) === 0, 5000), `⌥1 plays slot 1 (active ${await active()})`);
  await altDigit(2);
  ok(await untilApi(async () => (await active()) === 1, 5000), `⌥2 plays slot 2 (active ${await active()})`);
  await altDigit(1);
  await untilApi(async () => (await active()) === 0, 5000);
  await altDigit(3);
  ok(await until(`document.querySelector(".mv-pop .mv-pop-h") && /slot 3/.test(document.querySelector(".mv-pop .mv-pop-h").textContent)`, 3000, "slot 3's menu"), "⌥3 on the empty slot 3 opens its load menu");
  await sleep(300);
  await shot(join(DOCS, "move-page-slots.png"));
  await esc();
  ok(await until(`!document.querySelector(".mv-pop")`, 2000, "the menu closed"), "Esc closes the menu");
  // felucca into slot 3 too (it is "on 1"), then unloaded from slot 3's menu
  await click('.mv-chip[data-slot="2"] .mv-chip-main');
  await until(`document.querySelector('.mv-pop .mv-pop-item[data-id="felucca"]')`, 5000, "slot 3's menu");
  await click('.mv-pop .mv-pop-item[data-id="felucca"]');
  ok(await untilApi(async () => (await slotId(2)) === "felucca", 15000), `Felucca loaded into slot 3 as well (${await slotId(2)})`);
  await until(`document.querySelector('.mv-chip[data-slot="2"]:not(.empty):not(.loading)')`, 5000, "chip 3 loaded");
  await click('.mv-chip[data-slot="2"] .mv-chev');
  await until(`document.querySelector('.mv-pop [data-unload="2"]:not([disabled])')`, 5000, "Unload slot 3");
  await click('.mv-pop [data-unload="2"]');
  ok(await untilApi(async () => (await slotId(2)) === null, 10000), "Unload slot 3 from its menu: the Move's slot 3 is empty");
  ok(await until(`document.querySelector('.mv-chip[data-slot="2"].empty')`, 5000, "chip 3 empty"), "chip 3 dashed again");
  await altDigit(1);
  await untilApi(async () => (await active()) === 0, 5000);
  await until(`document.getElementById("fw-name").textContent === "Felucca"`, 5000, "Felucca");

  // ---- the firmware browser (js/movebrowse.js): the Move's catalog as tiles, the filters, a download that loads
  if (existsSync(join(ROOT, "build/testcat/index.json"))) {
    const cat = await api("catalog");
    const byId = Object.fromEntries((cat.firmwares || []).map((f) => [f.id, f]));
    ok(cat.firmwares.length === 3 && byId.amb && byId.amb.installed === "" && byId.amb.indexed && byId.choralroot.installed === "0.41" && byId.choralroot.version === "0.42"
      && cat.firmwares.every((f) => f.thumb && f.thumb.length === 684) && Array.isArray(cat.favorites),
      `GET /api/catalog: the index's 3 (AMB-1 not installed, ChoralRoot 0.41 here, 0.42 in the index), each with its 64 x 64 thumbnail, the favourites (${cat.favorites.length})`);
    await altDigit(3);
    ok(await until(`document.querySelector(".mv-pop .mv-pop-item[data-id='choralroot'] .mv-th img") && document.querySelector(".mv-pop [data-browse='2']")`, 5000, "the menu's thumbnails"),
      "slot 3's load menu: each row with its thumbnail, and Browse all");
    await click(".mv-pop [data-browse='2']");
    ok(await until(`!document.getElementById("mv-browse").hidden && document.querySelectorAll("#mv-browse .fb-tile").length === 3`, 5000, "the browser"),
      "Browse all: the firmware browser for slot 3, a tile per firmware of the Move's catalog (3)");
    ok(await until(`document.querySelectorAll("#mv-browse .fb-art img.in").length === 3`, 8000, "the thumbnails"), "every tile's thumbnail drawn");
    const th = await js(`(() => { const t = (id) => document.querySelector('#mv-browse .fb-tile[data-id="' + id + '"]'), im = (id) => t(id).querySelector(".fb-art img.in");
      return { amb: im("amb").src.slice(0, 90), ambRich: !im("amb").classList.contains("fb-bits"), bits: ["choralroot", "felucca"].every((id) => im(id).classList.contains("fb-bits") && im(id).naturalWidth === 64),
        px: getComputedStyle(im("felucca")).imageRendering, up: !!t("choralroot").querySelector(".fb-up"), get: !!t("amb").querySelector(".fb-get"),
        slot: document.querySelector("#mv-browse .mv-br-slot.on").dataset.slot, favOff: document.querySelector('#mv-browse .fb-chip[data-v="fav"]').disabled }; })()`);
    ok(th.ambRich && (LIVE_IMAGES || (/amb\.svg/.test(th.amb) && th.bits && /pixelated|crisp/.test(th.px))),
      `AMB-1: RemoteFM1FW's catalog image; ChoralRoot and Felucca: the Move's 1-bit thumbnails, 64 x 64, ${th.px}`);
    ok(th.up && th.get && th.slot === "2", "marks: UPDATE on ChoralRoot (0.41 here, 0.42 in the index), a download arrow on AMB-1; the head says slot 3");
    ok(th.favOff, "no favourites on the Move: the Favourites chip is off, All shows");
    const tiles = () => js(`[...document.querySelectorAll("#mv-browse .fb-tile")].map((t) => t.dataset.id).join(",")`);
    await click('#mv-browse .fb-chips[aria-label="Installed or not"] .fb-chip[data-v="out"]');
    ok(await tiles() === "amb", "Not installed: AMB-1 only");
    await click('#mv-browse .fb-chips[aria-label="Installed or not"] .fb-chip[data-v="in"]');
    const inst = await js(`({ ids: [...document.querySelectorAll("#mv-browse .fb-tile")].map((t) => t.dataset.id).sort().join(","), n: document.querySelector('#mv-browse .fb-chip[data-v="in"] .fb-n').textContent })`);
    ok(inst.ids === "choralroot,felucca" && /1 update/.test(inst.n), `Installed: ChoralRoot and Felucca, "${inst.n}"`);
    await click('#mv-browse .fb-chips[aria-label="Installed or not"] .fb-chip[data-v="any"]');
    const catChip = await js(`[...document.querySelectorAll('#mv-browse .fb-chips[aria-label="Category"] .fb-chip')].map((c) => c.dataset.v).find((c) => c !== "All")`);
    await click(`#mv-browse .fb-chips[aria-label="Category"] .fb-chip[data-v="${catChip}"]`);
    const inCat = (await tiles()).split(",").filter(Boolean), wantCat = cat.firmwares.filter((f) => f.categories.includes(catChip)).map((f) => f.id);
    ok(inCat.length === wantCat.length && inCat.every((x) => wantCat.includes(x)) && await js(`document.querySelectorAll('#mv-browse .fb-chips[aria-label="Category"] .fb-chip.on').length === 1`),
      `one category at a time: ${catChip} (${inCat.join(", ")})`);
    await click('#mv-browse .fb-chips[aria-label="Category"] .fb-chip[data-v="All"]');
    await sleep(500);
    await shot(join(DOCS, "move-page-browser.png"));
    // a tap on AMB-1 (not installed): the Move downloads it, then it loads into slot 3
    await click('#mv-browse .fb-main[data-id="amb"]');
    const ring = await until(`document.querySelector('#mv-browse .fb-tile[data-id="amb"].busy .fb-ring') || document.getElementById("mv-browse").hidden`, 5000, "the download");
    ok(ring, "a tap on AMB-1: its tile shows the download's ring");
    ok(await untilApi(async () => { const st = await status(); return st && st.dl_ok > 0 && st.dl_id === "amb"; }, 60000), "the Move downloaded AMB-1 (its status: dl_ok)");
    ok(await untilApi(async () => (await slotId(2)) === "amb", 30000), `then it loads into slot 3 (${await slotId(2)})`);
    ok(await until(`document.getElementById("mv-browse").hidden && document.querySelector('.mv-chip[data-slot="2"]:not(.empty)') && /AMB/.test(document.querySelector('.mv-chip[data-slot="2"] .mv-nm').textContent)`, 10000, "chip 3"),
      "the browser closed; chip 3 names AMB-1");
    ok((await api("catalog")).firmwares.find((f) => f.id === "amb").installed === "1.3.2", "the catalog says AMB-1 1.3.2 is installed");
    // the whole Move catalog (dist/move-catalog, when built): the tiles drawn as they come into view, all once scrolled
    if (existsSync(BIG)) {
      const big = JSON.parse(readFileSync(BIG, "utf8")).firmwares;
      const here = new Set(["felucca", "choralroot", "amb"]);
      const rows = big.map((f) => ({ id: f.id, name: f.name, version: f.version, installed: here.has(f.id) ? (f.id === "choralroot" ? "0.41" : f.version) : "",
        description: f.description, categories: f.categories, virtual: !!f.virtual, indexed: true, thumb: f.thumb || "" }));
      await js(`(() => { const b = FM1MoveUi.browse; b.load = async () => b.cat; b.cat = { generated: "dist/move-catalog", from: 2, firmwares: ${JSON.stringify(rows)}, favorites: ${JSON.stringify(rows.slice(4, 9).map((r) => r.id))} };
        b.fb.filters.scope = "fav"; b.fb.filters.inst = "any"; b.fb.filters.cat = "All"; b.open(0); return true; })()`);
      ok(await until(`document.querySelectorAll("#mv-browse .fb-tile").length === 5`, 5000, "Favourites"), "with favourites (the Move's, five here): Favourites shows those five");
      await click('#mv-browse .fb-chip[data-v="all"]');
      const N = rows.length, withThumb = rows.filter((r) => r.thumb).length;
      ok(await until(`document.querySelectorAll("#mv-browse .fb-tile").length === ${N}`, 5000, "all"), `All: ${N} tiles (dist/move-catalog)`);
      await sleep(LIVE_IMAGES ? 3000 : 800);
      const first = await js(`document.querySelectorAll("#mv-browse .fb-art[data-painted]").length`);
      ok(first > 0 && first < N, `lazily: ${first} of ${N} thumbnails drawn before scrolling`);
      await shot(join(DOCS, "move-page-browser-all.png"));
      for (let y = 0; y < 30; y++) { await js(`(() => { const b = document.querySelector("#mv-browse .mv-br-body"); b.scrollTop += b.clientHeight * 0.8; return true; })()`); await sleep(120); }
      ok(await until(`document.querySelectorAll("#mv-browse .fb-art img.in").length >= ${withThumb}`, 15000, "every thumbnail"),
        `scrolled through: every one of the ${withThumb} thumbnails drawn (${await js(`document.querySelectorAll("#mv-browse .fb-art img.in").length`)})`);
      await js(`(() => { const b = FM1MoveUi.browse; delete b.load; b.fb.filters.scope = "all"; b.fb.keep(); b.close(); b.load(); return true; })()`);
    }
    if (await js(`!document.getElementById("mv-browse").hidden`)) {
      await esc();
      ok(await until(`document.getElementById("mv-browse").hidden`, 3000, "closed"), "Esc closes the browser");
    }
    // (back as it was: slot 3 empty, slot 1 playing)
    await edit(17, 2, 0);
    await untilApi(async () => (await slotId(2)) === null, 10000);
    await altDigit(1);
    await untilApi(async () => (await active()) === 0, 5000);
    await until(`document.getElementById("fw-name").textContent === "Felucca"`, 5000, "Felucca");
  }

  // ---- Mix & FX (rfm1cs 0.5.0): ⌥M; FX setup (one layer: the chain) and the Mixer follow the Move's and set it
  await edit(22, 3, 0);
  await key("KeyM", { alt: true, keyName: "µ", vk: 77 });
  ok(await until(`document.querySelector("#mv-view.open") && document.getElementById("mv-mixbtn").classList.contains("lit")`, 3000, "the view"), "⌥M opens the Mix & FX view");
  ok(await until(`document.querySelector('.mv-tab.on[data-tab="1"]') && document.querySelector('.mv-strip[data-slot="0"]')`, 5000, "Mixer"), "the tab follows the Move's FX setup: Mixer (T4)");
  await edit(22, 1 | 2 << 2, 0);   // (the Move: firmware 2 selected, column 3)
  ok(await until(`document.querySelector('.mv-tab.on[data-tab="0"]') && document.querySelector(".mv-chain") && document.querySelector('.mv-chfw.sel[data-slot="1"]') && document.querySelector('.mv-fwedit[data-slot="1"]')`, 5000, "FX setup"),
    "the Move selects firmware 2 in FX setup: the page shows the chain, firmware 2 selected (its filter and knobs)");
  ok(await js(`[...document.querySelectorAll(".mv-chaincol")].map((c) => c.querySelectorAll(".mv-card").length).join()`) === "3,3,3", "the chain: 3 MIDI FX, 3 firmwares, 3 audio FX (the Move's pad columns 1, 3, 5)");
  await click('.mv-fxslot[data-kind="1"][data-fxslot="0"]');
  ok(await untilApi(async () => { const m = await mixNow(); return m && m.page === 2 && m.focus === 0; }, 5000), `a tap on audio FX 1 selects it on the Move (page ${((await mixNow()) || {}).page}, slot ${((await mixNow()) || {}).focus})`);
  // Freeverb into audio FX 1 from the page's module list, a parameter dragged
  await until(`document.querySelector('.mv-modrow[data-mod="2"]')`, 5000, "the module list");
  await click('.mv-modrow[data-mod="2"]');
  ok(await untilApi(async () => { const m = await mixNow(); return m && m.afx[0].mod === 2 && m.afx[0].name === "Freeverb"; }, 8000), "Freeverb loaded into audio FX 1 from the page (the core's afx[0])");
  ok(await until(`document.querySelector('[data-ctl="p-1-0-0"]') && /Freeverb/.test(document.querySelector(".mv-modpick").textContent)`, 5000, "Freeverb's parameters"), "its page shows (Room Size, ...)");
  const room0 = (await mixNow()).afx[0].vals[0];
  { const b = await box('[data-ctl="p-1-0-0"] .mv-dk'); await drag(b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2 - 60); }
  ok(await untilApi(async () => (await mixNow()).afx[0].vals[0] > room0 + 20, 4000), `a dial drag sets Room Size in the core (${room0} -> ${(await mixNow()).afx[0].vals[0]})`);
  ok(await until(`/wet for a send/.test(document.querySelector(".mv-fxedit").textContent) && /%/.test(document.querySelector('[data-ctl="p-1-0-0"] .mv-val').textContent)`, 2000, "the hint"),
    `Wet has the amber hint (100 % wet for a send); values read as the Move shows them (${await js(`document.querySelector('[data-ctl="p-1-0-0"] .mv-val').textContent`)})`);
  await edit(21, 1 << 5 | 0 << 3 | 1, 99);   // (the Move's knob: Damping, the 0.4 op)
  ok(await until(`document.querySelector('[data-ctl="p-1-0-1"]').dataset.v === "99"`, 5000, "Damping 99"), `an edit on the Move (Damping 99) shows on the page (${await dv('[data-ctl="p-1-0-1"]')})`);
  // Page Test (a multi-level ui_hierarchy) into audio FX 2: its pages, an edit on page 2, the Move's page followed
  await click('.mv-fxslot[data-kind="1"][data-fxslot="1"]');
  await until(`document.querySelector('.mv-fxslot.sel[data-kind="1"][data-fxslot="1"]') && document.querySelector('.mv-modrow[data-mod="3"]')`, 5000, "audio FX 2");
  await click('.mv-modrow[data-mod="3"]');
  ok(await untilApi(async () => (await mixNow()).afx[1].mod === 3, 8000), "Page Test loaded into audio FX 2 from the page");
  ok(await until(`document.querySelectorAll(".mv-pager .mv-pg").length === 6 && document.querySelectorAll('.mv-knobs [data-knob]').length === 8 && document.querySelector('.mv-knobs [data-knob="2"].gap')`, 5000, "its pages"),
    `its 6 pages as tabs (${await js(`[...document.querySelectorAll(".mv-pager .mv-pg")].map((b) => b.textContent).join(", ")`)}), 8 knob cells, the gap at knob 3`);
  await click('.mv-pg[data-pg="1"]');
  ok(await until(`document.querySelector('.mv-pg.on[data-pg="1"]') && document.querySelector('[data-ctl="p-1-1-9"]')`, 3000, "page 2"), "a tap on \"Pages 2\": its knobs (Spare One, index 9)");
  ok(await untilApi(async () => (await mixNow()).afx[1].pg === 1, 3000), "the Move's knobs follow: audio FX 2 on page 2 in the core (FXPG)");
  const sp0 = (await mixNow()).afx[1].vals[9];
  { const b = await box('[data-ctl="p-1-1-9"] .mv-dk'); await drag(b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2 - 50); }
  ok(await untilApi(async () => (await mixNow()).afx[1].vals[9] > sp0 + 20, 4000), `an edit on page 2 reaches the core (FXPAR): Spare One ${sp0} -> ${(await mixNow()).afx[1].vals[9]}`);
  await edit(24, 4 + 1, 2);   // (the Move's jog: audio FX 2 on Advanced)
  ok(await until(`document.querySelector('.mv-pg.on[data-pg="2"]') && document.querySelector('[data-ctl="p-1-1-11"]')`, 5000, "Advanced"), "the Move turns to Advanced: the page follows (Delay Time, index 11)");
  await edit(23, 11, 1 << 9 | 1 << 7 | 100);
  ok(await until(`document.querySelector('[data-ctl="p-1-1-11"]').dataset.v === "100" && /1\\.57 s/.test(document.querySelector('[data-ctl="p-1-1-11"] .mv-val').textContent)`, 5000, "Delay Time 100"),
    `Delay Time 100 from outside shows on page 3 (${await js(`document.querySelector('[data-ctl="p-1-1-11"] .mv-val').textContent`)})`);
  await sleep(300);
  await js(`document.querySelector(".mv-body").scrollTop = 0`);
  await shot(join(DOCS, "move-page-fxpages.png"));
  // Mixer: a send dial, the level fader; the Move's edits show
  await click('.mv-tab[data-tab="1"]');
  ok(await untilApi(async () => ((await mixNow()) || {}).page === 3, 5000), "a tap on Mixer sets the Move's FX setup to its Mixer (page 3)");
  await until(`document.querySelector('[data-ctl="send-0-0"]:not(.off)')`, 5000, "the send to Freeverb");
  { const b = await box('[data-ctl="send-0-0"] .mv-dk'); await drag(b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2 - 50); }
  ok(await untilApi(async () => (await mixNow()).slots[0].sends[0] > 20, 4000), `a send dial drag: Felucca's send to Freeverb in the core (${(await mixNow()).slots[0].sends[0]})`);
  { const b = await box('.mv-strip[data-slot="0"] .mv-fader-t'); await drag(b.x + b.w / 2, b.y + b.h * 0.2, b.x + b.w / 2, b.y + b.h * 0.8); }
  const lv = (await mixNow()).slots[0].level;
  ok(lv > 5 && lv < 50, `the level fader dragged down: Felucca's level in the core ${lv}`);
  const knob = await js(`(() => { const t = document.querySelector('.mv-strip[data-slot="0"] .mv-fader-t'), f = t.querySelector("i").getBoundingClientRect(), r = t.getBoundingClientRect(); return !t.querySelector("b") && f.top >= r.top && f.bottom <= r.bottom && f.height > 0; })()`);
  ok(knob, "the level fader: no handle, its coloured fill inside the track");
  await edit(18, 0 * 8 + 0, 33); await edit(18, 1 * 8 + 2, 70); await edit(18, 1 * 8 + 1, 40);
  ok(await until(`document.querySelector('[data-ctl="level-0"]').dataset.v === "33" && document.querySelector('[data-ctl="send-1-0"]').dataset.v === "70" && document.querySelector('[data-ctl="pan-1"]').dataset.v === "40"`, 5000, "the Move's mix edits"),
    `edits on the Move show in the Mixer (level ${await dv('[data-ctl="level-0"]')}, send ${await dv('[data-ctl="send-1-0"]')}, pan ${await dv('[data-ctl="pan-1"]')})`);
  await edit(18, 0, 100); await edit(18, 2, 40);
  // (two of the faceplate's computer keys held: Felucca sounds, its send feeds Freeverb: the meters move)
  for (const [code, k, vk] of [["KeyS", "s", 83], ["KeyG", "g", 71]]) await cdp("Input.dispatchKeyEvent", { type: "keyDown", code, key: k, windowsVirtualKeyCode: vk });
  await sleep(900);
  const met = await js(`[...document.querySelectorAll(".mv-mixer [data-meter]")].map((m) => +m.dataset.v)`);
  ok(met[0] > 0 && met[met.length - 1] > 0, `the meters move: Felucca ${met[0]}, master ${met[met.length - 1]} (${met.join(" ")})`);
  await shot(join(DOCS, "move-page-mixfx.png"));
  for (const [code, k, vk] of [["KeyS", "s", 83], ["KeyG", "g", 71]]) await cdp("Input.dispatchKeyEvent", { type: "keyUp", code, key: k, windowsVirtualKeyCode: vk });
  // FX setup: a MIDI FX, a firmware's track filter, a MIDI FX's one track
  await click('.mv-tab[data-tab="0"]');
  await until(`document.querySelector('.mv-fxslot[data-kind="0"][data-fxslot="0"]')`, 5000, "the chain");
  await click('.mv-fxslot[data-kind="0"][data-fxslot="0"]');
  await until(`document.querySelector('.mv-fxslot.sel[data-kind="0"][data-fxslot="0"]') && document.querySelector('.mv-modrow[data-mod="0"]')`, 5000, "the MIDI FX modules");
  await click('.mv-modrow[data-mod="0"]');
  ok(await untilApi(async () => (await mixNow()).mfx[0].mod === 0, 5000), "the Arpeggiator into MIDI FX 1 from the page");
  // (the page's own copy of the mix catches up before the next click: its rebuild would swallow a click in flight)
  await until(`document.querySelector('.mv-modrow[data-mod="0"].on')`, 5000, "the page shows the Arpeggiator");
  await click('.mv-picker .mv-st[data-track="7"]');
  ok(await untilApi(async () => (await mixNow()).mfx[0].filt === 64, 4000), `the Arpeggiator acts on track 7 only (${(await mixNow()).mfx[0].filt})`);
  await click('.mv-chfw[data-slot="1"]');
  await until(`document.querySelector('.mv-fwedit[data-slot="1"] .mv-st[data-track="9"]')`, 5000, "firmware 2's filter");
  const filt0 = (await mixNow()).slots[1].filt;
  await click('.mv-fwedit[data-slot="1"] .mv-st[data-track="9"]');
  ok(await untilApi(async () => (await mixNow()).slots[1].filt === (filt0 ^ 0x100), 4000), `a routing cell: track 9 toggled in ChoralRoot's filter (${filt0.toString(16)} -> ${(await mixNow()).slots[1].filt.toString(16)})`);
  await edit(22, 1 | 1 << 2, 0);   // (the Move: firmware 1 selected)
  await edit(19, 0, 0x00ff);
  ok(await until(`document.querySelector('.mv-fwedit[data-slot="0"] .mv-st[data-track="8"].on') && !document.querySelector('.mv-fwedit[data-slot="0"] .mv-st[data-track="9"].on')`, 5000, "Felucca 1-8"),
    "the Move selects firmware 1 and sets its filter (tracks 1-8): the page shows it");
  // Esc: a latch first, then the view
  await esc();
  ok(await until(`!document.querySelector("#mv-view.open")`, 3000, "Esc"), "Esc goes back to the faceplate");
  {
    const w = await js(`(() => { const w = FM1Device.where(0, 3), r = document.getElementById("device").getBoundingClientRect(); return w && { x: r.left + w.x, y: r.top + w.y }; })()`);
    await cdp("Input.dispatchMouseEvent", { type: "mousePressed", x: w.x, y: w.y, button: "left", buttons: 1, clickCount: 1, modifiers: 8 });
    await cdp("Input.dispatchMouseEvent", { type: "mouseReleased", x: w.x, y: w.y, button: "left", buttons: 0, clickCount: 1, modifiers: 8 });
  }
  await key("KeyM", { alt: true, keyName: "µ", vk: 77 });
  const latched = await js(`FM1MoveUi.o.controls.anyLatched()`);
  await esc();
  const after = await js(`({ open: !!document.querySelector("#mv-view.open"), latched: FM1MoveUi.o.controls.anyLatched() })`);
  ok(latched && after.open && !after.latched, `with a key latched, Esc lets it go and the view stays (latched ${latched} -> ${after.latched}, open ${after.open})`);
  await esc();
  ok(await until(`!document.querySelector("#mv-view.open")`, 3000, "Esc again"), "...and the next Esc goes back to the faceplate");

  // ---- Files (js/movefiles.js, js/movefs.js: the RemoteFM1 folder on the Move): the settings sheet's Files section
  // (Add samples one click away, the exports), the file browser from the header (breadcrumbs, sizes and dates, a new
  // folder, files dropped on it, rename, delete, read-only folders), the exports as real downloads
  {
    const wav = join(tmp, "page kick.wav"), frames = 4410, d = frames * 4, b = Buffer.alloc(44 + d);
    b.write("RIFF", 0); b.writeUInt32LE(36 + d, 4); b.write("WAVEfmt ", 8); b.writeUInt32LE(16, 16); b.writeUInt16LE(1, 20);
    b.writeUInt16LE(2, 22); b.writeUInt32LE(44100, 24); b.writeUInt32LE(176400, 28); b.writeUInt16LE(4, 32); b.writeUInt16LE(16, 34);
    b.write("data", 36); b.writeUInt32LE(d, 40);
    for (let i = 0; i < frames * 2; i++) b.writeInt16LE(Math.round(8000 * Math.sin(i / 20)), 44 + 2 * i);
    writeFileSync(wav, b);
    const fsList = async (p) => api(`fs/list?path=${encodeURIComponent(p)}`).catch(() => null);
    const onMove = async (p, name) => { const j = await fsList(p); return j && j.entries && j.entries.find((f) => f.name === name); };
    await js(`FM1MoveUi.o.sheet.open()`);
    ok(await until(`[...document.querySelectorAll(".sheet h3.heading")].some((e) => e.textContent === "Files") && document.querySelector(".sheet .mv-add-samples") && document.querySelector(".sheet .mv-exp-proj") && document.querySelector(".sheet .mv-exp-states")`, 5000, "the sheet's Files"),
      "the settings sheet's Files section: Add samples, Browse files, Export all projects, Export states (in place of Audio files)");
    const inp = await cdp("Runtime.evaluate", { expression: `document.querySelector(".mv-files input[type=file]")` });
    await cdp("DOM.setFileInputFiles", { files: [wav], objectId: inp.result.result.objectId });
    ok(await untilApi(async () => { const f = await onMove("audio", "page kick.wav"); return f && f.size === b.length; }, 10000),
      `Add samples: page kick.wav (${b.length} bytes) uploaded into the Move's audio/ in one click`);
    await js(`FM1MoveUi.o.sheet.close()`);
    // the header's Files button: the browser at the folder's root
    await click("#mv-filesbtn");
    ok(await until(`!document.getElementById("mv-fs").hidden && document.querySelectorAll("#mv-fs .mv-fs-row").length >= 6`, 5000, "the Files view"),
      "the header's Files button opens the file browser: the RemoteFM1 folder's root");
    const root = await js(`({ names: [...document.querySelectorAll("#mv-fs .mv-fs-row")].map((r) => r.dataset.name), ro: !document.querySelector("#mv-fs .mv-fs-ro").hidden,
      up: document.querySelector("#mv-fs .mv-fs-up").disabled })`);
    ok(["audio", "code", "data", "firmwares", "move", "states"].every((n) => root.names.includes(n)) && root.ro && root.up,
      `its root: ${root.names.join(", ")}; read-only there (the note, no upload)`);
    await click('#mv-fs .mv-fs-row[data-name="audio"] .nm');
    ok(await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="page kick.wav"]') && document.querySelector("#mv-fs .mv-fs-crumbs").textContent.includes("audio")`, 5000, "audio/"),
      "a folder opens: audio/ in the breadcrumbs, the sample listed");
    const row = await js(`(() => { const r = document.querySelector('#mv-fs .mv-fs-row[data-name="page kick.wav"]'); return { sz: r.querySelector(".sz").textContent, dt: r.querySelector(".dt").textContent }; })()`);
    ok(/KB$/.test(row.sz) && /\d/.test(row.dt), `its size and date (${row.sz}, ${row.dt})`);
    // a new folder (its name asked), a file dropped into it, renamed, deleted; the folder deleted with what is in it
    await js(`window.prompt = () => "Kits"; window.confirm = () => true`);
    await click("#mv-fs .mv-fs-new");
    ok(await untilApi(async () => { const f = await onMove("audio", "Kits"); return f && f.dir; }, 5000), "New folder: audio/Kits on the Move");
    await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="Kits"]')`, 5000, "Kits listed");
    await click('#mv-fs .mv-fs-row[data-name="Kits"] .nm');
    await until(`document.querySelector("#mv-fs .mv-fs-crumbs").textContent.includes("Kits")`, 5000, "into Kits");
    await js(`(() => { const dt = new DataTransfer(); dt.items.add(new File([new Uint8Array(${JSON.stringify([...b.subarray(0, 2000)])})], "snare one.wav", { type: "audio/wav" }));
      const el = document.getElementById("mv-fs"); el.dispatchEvent(new DragEvent("dragenter", { dataTransfer: dt, bubbles: true }));
      el.dispatchEvent(new DragEvent("drop", { dataTransfer: dt, bubbles: true, cancelable: true })); })()`);
    ok(await untilApi(async () => { const f = await onMove("audio/Kits", "snare one.wav"); return f && f.size === 2000; }, 8000) &&
      await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="snare one.wav"]')`, 5000, "the dropped file"),
      "a file dropped on the browser lands in the folder shown (audio/Kits/snare one.wav)");
    // a folder dropped (its entries, as the browser gives them: a folder in it, a hidden file left out) lands as folders
    await js(`(() => { const bytes = new Uint8Array(${JSON.stringify([...b.subarray(0, 1500)])});
      const file = (name) => ({ name, isFile: true, isDirectory: false, file: (ok) => ok(new File([bytes], name, { type: "audio/wav" })) });
      const dir = (name, kids) => ({ name, isFile: false, isDirectory: true, createReader: () => { let done = false; return { readEntries: (ok) => { ok(done ? [] : kids); done = true; } }; } });
      const tree = dir("Pack", [file("hat.wav"), file(".DS_Store"), dir("Toms", [file("tom 1.wav"), file("tom 2.wav")])]);
      const ev = new Event("drop", { bubbles: true, cancelable: true });
      Object.defineProperty(ev, "dataTransfer", { value: { types: ["Files"], files: [], items: [{ kind: "file", webkitGetAsEntry: () => tree }] } });
      document.getElementById("mv-fs").dispatchEvent(ev); })()`);
    ok(await untilApi(async () => { const t = await onMove("audio/Kits/Pack/Toms", "tom 2.wav"); return t && t.size === 1500; }, 10000) &&
      (await onMove("audio/Kits/Pack", "hat.wav"))?.size === 1500 && (await onMove("audio/Kits/Pack/Toms", "tom 1.wav"))?.size === 1500 &&
      !(await onMove("audio/Kits/Pack", ".DS_Store")) && await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="Pack"]')`, 5000, "Pack listed"),
      "a folder dropped on the browser: audio/Kits/Pack/hat.wav and Pack/Toms/tom 1.wav, tom 2.wav on the Move, .DS_Store left out");
    await shot(join(DOCS, "move-page-files.png"));
    await js(`window.prompt = () => "snare 2.wav"`);
    await js(`document.querySelector('#mv-fs .mv-fs-row[data-name="snare one.wav"] button[aria-label^="Rename"]').click()`);
    ok(await untilApi(async () => !!(await onMove("audio/Kits", "snare 2.wav")) && !(await onMove("audio/Kits", "snare one.wav")), 5000), "Rename: snare one.wav -> snare 2.wav on the Move");
    await js(`document.querySelectorAll('#mv-fs .mv-fs-crumbs button')[1].click()`);   // (audio/)
    await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="Kits"]')`, 5000, "back in audio/");
    await js(`document.querySelector('#mv-fs .mv-fs-row[data-name="Kits"] button[aria-label^="Delete"]').click()`);
    ok(await untilApi(async () => !(await onMove("audio", "Kits")), 5000) && await until(`!document.querySelector('#mv-fs .mv-fs-row[data-name="Kits"]')`, 5000, "Kits gone"),
      "Delete a folder with a file in it: asked twice, then deleted with what is in it");
    await js(`document.querySelector('#mv-fs .mv-fs-row[data-name="page kick.wav"] button[aria-label^="Delete"]').click()`);
    ok(await untilApi(async () => !(await onMove("audio", "page kick.wav")), 5000), "Delete a file: page kick.wav gone from the Move");
    // read-only: firmwares/ (its note, no upload, no rename or delete)
    await until(`!document.querySelector('#mv-fs .mv-fs-row[data-name="page kick.wav"]')`, 5000, "the list again");
    await js(`document.querySelector('#mv-fs .mv-fs-crumbs button').click()`);   // (the breadcrumbs' first: the root)
    await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="firmwares"]')`, 5000, "the root");
    await click('#mv-fs .mv-fs-row[data-name="firmwares"] .nm');
    await until(`document.querySelector('#mv-fs .mv-fs-row[data-name="felucca-1.8.2.rfm1m"]')`, 5000, "firmwares/");
    const fwv = await js(`({ note: document.querySelector("#mv-fs .mv-fs-ro").textContent, up: document.querySelector("#mv-fs .mv-fs-up").disabled,
      del: document.querySelector('#mv-fs .mv-fs-row[data-name="felucca-1.8.2.rfm1m"] button[aria-label^="Delete"]').disabled })`);
    ok(fwv.up && fwv.del && /signed catalog/.test(fwv.note), `firmwares/ read-only on the page: "${fwv.note.slice(0, 80)}..."`);
    // the exports: real downloads (Export states, Export all projects)
    const dl = join(tmp, "dl");
    await cdp("Browser.setDownloadBehavior", { behavior: "allow", downloadPath: dl, eventsEnabled: true });
    await click("#mv-fs .mv-fs-exp-states");
    await click("#mv-fs .mv-fs-exp-proj");
    const got = await untilApi(async () => { try { const f = readdirSync(dl); return f.includes("states.zip") && f.includes("projects.zip"); } catch { return false; } }, 10000);
    const zhead = got ? readFileSync(join(dl, "states.zip")).subarray(0, 4).toString("latin1") : "";
    ok(got && zhead === "PK\x03\x04", `Export states and Export all projects: states.zip and projects.zip downloaded (${got ? readdirSync(dl).join(", ") : "none"})`);
    await key("Escape", { keyName: "Escape", vk: 27 });
    ok(await until(`document.getElementById("mv-fs").hidden`, 3000, "Esc"), "Esc closes the file browser");
  }

  // ---- the phone (390 x 844): the slot button, its menu, the Mix & FX sheet
  await edit(22, 3, 0);
  await cdp("Emulation.setDeviceMetricsOverride", { width: 390, height: 844, deviceScaleFactor: 2, mobile: true });
  await cdp("Emulation.setTouchEmulationEnabled", { enabled: true, maxTouchPoints: 5 });
  await cdp("Page.reload", { ignoreCache: false });
  ok(await until(`window.FM1MoveUi && document.getElementById("mv-slotbtn").offsetParent && FM1Site.current().info && FM1Site.current().info.id === "felucca" && document.querySelector("#mv-slotbtn canvas")`, 20000, "the phone page"),
    "the phone: the slot button in the header (the playing slot's screen and name)");
  const sb = await js(`(() => { const b = document.getElementById("mv-slotbtn").getBoundingClientRect(), g = document.querySelector("#corner .grab-bar, #corner"), n = document.querySelector("#mv-slotbtn .mv-sb-n");
    const gb = document.getElementById("corner").getBoundingClientRect();
    return { chips: !!document.getElementById("mv-chips").offsetParent, left: b.left, right: b.right, w: b.width, dots: !!document.querySelector("#mv-slotbtn .mv-dot"),
      handle: gb.left + gb.width / 2 - 20, ell: getComputedStyle(n).textOverflow }; })()`);
  ok(!sb.chips && sb.left < 30 && sb.w <= 150 && !sb.dots && sb.right < sb.handle && sb.ell === "ellipsis",
    `no chips on the phone; the slot button top left, ${Math.round(sb.w)} px (at most 150), no dots, ellipsis, clear of the centre handle (${Math.round(sb.right)} < ${Math.round(sb.handle)})`);
  const plate = await js(`(() => { const L = FM1Device.L, oct = L.btns.find((b) => b.b === 13 || b.b === 12), fx = L.btns.find((b) => b.b === 0);
    return { fx: fx.font, oct: oct.font, film: !!L.film, kind: FM1Device.kind }; })()`);
  ok(plate.kind === "phonePortrait" && Math.abs(plate.fx - 13) < 1.5 && Math.abs(plate.oct - 15) < 1.5 && !plate.film,
    `the phone faceplate as iOS draws it: legends ${plate.fx.toFixed(1)} px, OCT ${plate.oct.toFixed(1)} px, no red protector tab by KN1 (${plate.kind})`);
  await sleep(1500);
  await click("#mv-slotbtn");
  ok(await until(`document.querySelectorAll(".mv-ipmenu .mv-iprow").length === 3 && document.querySelector(".mv-ipmenu .mv-iprow.on") && document.querySelector(".mv-ipmenu .mv-iprow.empty")`, 3000, "the slot menu"),
    "a tap drops the slot menu: three rows (the playing one ringed, the empty one \"Load into slot 3\")");
  await sleep(400);
  const rowH = await js(`document.querySelector(".mv-ipmenu .mv-iprow").getBoundingClientRect().height`);
  ok(rowH >= 64, `the slot rows are 64 px tall (${rowH})`);
  await shot(join(DOCS, "move-page-phone.png"));
  await click('.mv-ipmenu .mv-iprow[data-slot="1"]');
  ok(await untilApi(async () => (await active()) === 1, 5000), `a row plays its slot (active ${await active()})`);
  await click("#mv-slotbtn");
  await until(`document.getElementById("mv-ip-mix")`, 3000, "the menu again");
  await click("#mv-ip-mix");
  ok(await until(`document.querySelector("#mv-view.open.phone") && document.querySelector(".mv-ph-strip") && document.querySelector('.mv-tab.on[data-tab="1"]')`, 5000, "the phone sheet"), "Mix & FX from the slot menu: the full-screen sheet, Mixer");
  await sleep(600);
  await shot(join(DOCS, "move-page-phone-mixfx.png"));
  await click('.mv-tab[data-tab="0"]');
  await until(`document.querySelector('.mv-fxslot[data-kind="1"][data-fxslot="1"]')`, 5000, "the phone's chain");
  await click('.mv-fxslot[data-kind="1"][data-fxslot="1"]');
  ok(await until(`document.querySelectorAll(".mv-pager .mv-pg").length === 6 && document.querySelector('.mv-knobs.ph [data-knob="0"] .mv-bar')`, 5000, "the phone's pages"),
    "the phone: FX setup's chain, Page Test's pages as tabs, its knobs as bar sliders");
  await sleep(500);
  await shot(join(DOCS, "move-page-phone-fxpages.png"));
  await click('.mv-tab[data-tab="1"]');
  await until(`document.querySelector('.mv-ph-strip[data-slot="1"] [data-ctl="level-1"]')`, 5000, "the phone's Mixer");
  { const b = await box('.mv-ph-strip[data-slot="1"] [data-ctl="level-1"]'); await drag(b.x + b.w * 0.5, b.y + b.h / 2, b.x + b.w * 0.25, b.y + b.h / 2); }
  await untilApi(async () => { const v = (await mixNow()).slots[1].level; return v > 20 && v < 45; }, 2000);   // (the drag's last edits land)
  const lv1 = (await mixNow()).slots[1].level;
  ok(lv1 > 20 && lv1 < 45, `the phone's Level bar dragged: ChoralRoot's level in the core ${lv1}`);
  await edit(18, 1 * 8, 100);
  await click(".mv-close");
  ok(await until(`!document.querySelector("#mv-view.open")`, 3000, "close"), "the sheet's close button goes back to the faceplate");

  // ---- the firmware browser on the phone: from the slot menu's load page, the whole screen, nothing cut off
  if (existsSync(join(ROOT, "build/testcat/index.json"))) {
    await click("#mv-slotbtn");
    await until(`document.querySelector('.mv-ipmenu .mv-iprow.empty[data-slot="2"]')`, 3000, "the slot menu");
    await click('.mv-ipmenu .mv-iprow.empty[data-slot="2"]');
    ok(await until(`document.querySelector(".mv-ipmenu .mv-ip-list .mv-th") && document.querySelector('.mv-ipmenu [data-browse="2"]')`, 3000, "the load page"),
      "the phone's load page: thumbnails on its rows, and Browse all");
    await click('.mv-ipmenu [data-browse="2"]');
    ok(await until(`!document.getElementById("mv-browse").hidden && document.querySelectorAll("#mv-browse .fb-art img.in").length >= 3`, 8000, "the phone browser"), "Browse all: the browser, its thumbnails drawn");
    await sleep(LIVE_IMAGES ? 2500 : 500);
    const ph = await js(`(() => { const b = document.getElementById("mv-browse").getBoundingClientRect();
      return { w: b.width, x: b.left, h: b.height, sw: document.documentElement.scrollWidth, cols: getComputedStyle(document.querySelector("#mv-browse .fb-grid")).gridTemplateColumns.split(" ").length,
        cut: [...document.querySelectorAll("#mv-browse .fb-name, #mv-browse .fb-ver, #mv-browse .fb-chip, #mv-browse .mv-br-slot")].filter((e) => e.scrollWidth > e.clientWidth + 1 || e.getBoundingClientRect().right > innerWidth + 1).map((e) => e.textContent) }; })()`);
    ok(ph.w >= 389 && ph.x <= 1 && ph.h >= 800 && ph.sw <= 390, `the phone: the whole screen (${Math.round(ph.w)} x ${Math.round(ph.h)}), no sideways scroll`);
    ok(ph.cols >= 3 && !ph.cut.length, `${ph.cols} tiles a row; names, versions, chips whole, never cut off (${ph.cut.join(", ") || "none cut"})`);
    await shot(join(DOCS, "move-page-phone-browser.png"));
    await esc();
    ok(await until(`document.getElementById("mv-browse").hidden`, 3000, "closed"), "Esc closes it");
  }

  // ---- FM-1 Wi-Fi (js/movewifi.js) on the phone: the settings sheet's button opens it; with no FM-1 on USB-A it says
  // setup needs it there; with an FM-1 (the host's answer stood in for: a canned /api/fm1/wifi) the networks with
  // their bars and locks, a pick opens the password (hidden, its show toggle), Join; Esc closes it
  await js(`FM1MoveUi.o.sheet.open()`);
  ok(await until(`[...document.querySelectorAll(".sheet h3.heading")].some((e) => e.textContent === "FM-1 Wi-Fi") && document.getElementById("mv-wf-open")`, 5000, "the sheet's FM-1 Wi-Fi"),
    "the settings sheet's FM-1 Wi-Fi section: Set up the FM-1's Wi-Fi");
  await sleep(500);   // (the sheet slides in: a tap before it is there lands on the scrim)
  await click("#mv-wf-open");
  ok(await until(`!document.getElementById("mv-wf").hidden && /USB-A/.test((document.getElementById("mv-wf-need") || {}).textContent || "")`, 6000, "the Wi-Fi panel"),
    "it opens; no FM-1 on USB-A: \"Setup needs the FM-1 plugged into the Move's USB-A\"");
  const wfBox = await js(`(() => { const b = document.getElementById("mv-wf").getBoundingClientRect(); return { w: b.width, x: b.left, sw: document.documentElement.scrollWidth }; })()`);
  ok(wfBox.w >= 389 && wfBox.x <= 1 && wfBox.sw <= 390, `the panel fills the phone's width (${Math.round(wfBox.w)} px), no sideways scroll`);
  await js(`(() => { const real = window.fetch, net = (ssid, rssi, ch, secured, kept, joined) => ({ ssid, rssi, ch, secured, kept, joined });
    window.__wfFetch = real;
    window.fetch = (u, o) => String(u).endsWith("api/fm1/wifi") ? Promise.resolve(new Response(JSON.stringify({ on: true, usb: true, usb_wifi: true, usb_known: true,
      usb_id: "FM-1_729", usb_fw: "DEV", link: "usb", addr: "", search: "searching", wifi_off: false,
      fm1: { known: true, state: "connected", ssid: "Dogsontheroof", ip: "10.0.0.165", rssi: -63, why: "", saved: 1, host: 0 },
      job: { phase: "", ssid: "", why: "", ip: "", ms: 0, same_lan: -1 }, err: "",
      scan: { scanning: false, seq: 1, err: 0, nets: [net("Dogsontheroof", -52, 6, true, true, true), net("Owner's iPhone", -58, 1, true, false, false),
        net("Cafe open", -71, 11, false, false, false), net("A neighbour's network with a long name", -84, 3, true, false, false)] },
      kept: [{ ssid: "Dogsontheroof", heard: true, rssi: -52, joined: true }], counts: {} }), { headers: { "Content-Type": "application/json" } })) : real(u, o); })()`);
  ok(await until(`document.querySelectorAll("#mv-wf .mv-wf-row").length === 4 && document.querySelectorAll("#mv-wf .mv-wf-row .lk .mv-ic").length === 3 && document.querySelectorAll("#mv-wf .mv-wf-kept").length === 1`, 5000, "the canned list"),
    "the networks the FM-1 hears: four rows, three locks, signal bars; the network it keeps with Forget");
  await click(`#mv-wf .mv-wf-row[data-ssid="Owner's iPhone"]`);
  ok(await until(`document.querySelector("#mv-wf .mv-wf-net.sel #mv-wf-pass") && document.getElementById("mv-wf-pass").type === "password" && document.getElementById("mv-wf-join").disabled`, 3000, "the password"),
    "a pick opens its password (hidden; Join waits for 8 characters)");
  await js(`(() => { const p = document.getElementById("mv-wf-pass"); p.value = "fake-pass-1"; p.dispatchEvent(new Event("input")); })()`);
  await click("#mv-wf .mv-wf-eye");
  ok(await until(`document.getElementById("mv-wf-pass").type === "text" && !document.getElementById("mv-wf-join").disabled`, 2000, "show"),
    "the eye shows the password; Join is ready");
  await sleep(1700);   // (a poll in between keeps the field, its text and the pick)
  ok(await js(`document.getElementById("mv-wf-pass").value === "fake-pass-1" && !!document.querySelector("#mv-wf .mv-wf-net.sel")`), "a poll of the state keeps what was typed");
  await shot(join(DOCS, "move-page-phone-wifi.png"));
  await js(`(() => { window.fetch = window.__wfFetch; FM1MoveUi.wifi.pass.value = ""; })()`);
  await esc();
  ok(await until(`document.getElementById("mv-wf").hidden`, 3000, "Esc"), "Esc closes FM-1 Wi-Fi");

  const bad = [...responses].filter(([, s]) => s >= 400);
  ok(responses.size > 30 && !bad.length && !failed.length, `every file the page asked for loaded (${responses.size} responses; failed: ${bad.map(([u, s]) => `${s} ${u}`).concat(failed).join(", ") || "none"})`);
  ok(!exceptions.length, `no exceptions${exceptions.length ? ": " + exceptions.join(" | ") : ""}`);
  ok(!consoleErrors.length, `no console errors${consoleErrors.length ? ": " + consoleErrors.join(" | ") : ""}`);
} catch (e) {
  ok(false, `the test: ${e.stack || e}`);
}
console.log(`page_e2e: ${checks} checks, ${fails} failed`);
stop();
process.exit(fails ? 1 : 0);
