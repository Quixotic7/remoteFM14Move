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
//   - the Mix & FX view: the page's edits (a level fader, a send dial, routing cells, Freeverb into audio FX 1, a
//     parameter) land in the core (GET /api/mix), edits from outside (POST /api/edit, as the Move's knobs) show on
//     the page; the tab follows the Move's FX page and sets it;
//   - the phone (390 x 844): the slot button, its menu, the Mix & FX sheet;
//   - screenshots: docs/move-page.png (or --shot=FILE), move-page-slots.png, move-page-mixfx.png, move-page-phone.png
//     (the slot menu open) and move-page-phone-mixfx.png.
//   node tests/page_e2e.mjs [--shot=docs/move-page.png] [--port=N]          (after make; needs Docker and Chromium)
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOT = arg("shot", join(ROOT, "docs/move-page.png"));
const DOCS = join(ROOT, "docs");
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
  "mkdir -p /tmp/s && exec build/sim/remotefm1-host --root /tmp/r --sim-dir /tmp/s --firmwares build/fw --site build/site --fw felucca --catalog-url http://127.0.0.1:1 --fx-dir build/fxmods" +
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
const shot = async (file) => { const r = await cdp("Page.captureScreenshot", { format: "png" }); writeFileSync(file, Buffer.from(r.result.data, "base64")); console.log(`  shot  ${file}`); };
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
  };
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
    hidden: ["install-link", "vst-link", "ios-link", "android-link", "fm1-btn", "rec", "ed-btn", "fw-btn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent === null; }).length,
    shown: ["mv-chips", "fw-src", "site-src", "menu-btn", "mv-mixbtn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent !== null; }).length,
    start: document.getElementById("start").hidden, asked: window.__asked.slice(),
    plate: (() => { const c = document.getElementById("device"); return [c.width, c.height]; })(),
    lit: (() => { const p = FM1Site.screen().pixels; let n = 0; for (const v of p) if (v) n++; return n; })(),
    seq: FM1Site.screen().seq, fonts: [...document.fonts].filter((f) => f.status === "loaded").map((f) => f.family) })`);
  ok(page.href === BASE && page.secure === false, `plain http on ${HOSTNAME}: not a secure context, no https redirect (${page.href})`);
  ok(page.host === "move" && page.meta === "move", "the host flag: move (index.html's meta, <html data-host>)");
  ok(page.name === "Felucca" && page.ver === "1.8.2" && /RemoteFM1 on Move/.test(page.title), `the header names what the Move runs: ${page.name} ${page.ver} (${page.title})`);
  ok(page.hidden === 8 && page.shown === 5, `the header: slot chips, Source, Site source, settings, Mix & FX shown; the firmware picker, Install, app links, FM-1, recorder, editor hidden (${page.shown}/5, ${page.hidden}/8)`);
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

  // ---- Mix & FX: ⌥M, the tab follows the Move's FX page and sets it
  await edit(22, 3, 0);
  await key("KeyM", { alt: true, keyName: "µ", vk: 77 });
  ok(await until(`document.querySelector("#mv-view.open") && document.getElementById("mv-mixbtn").classList.contains("lit")`, 3000, "the view"), "⌥M opens the Mix & FX view");
  ok(await until(`document.querySelector('.mv-tab.on[data-tab="3"]') && document.querySelector('.mv-strip[data-slot="0"]')`, 5000, "Mixer"), "the tab follows the Move's FX page: Mixer (T4)");
  await edit(22, 1, 0);
  ok(await until(`document.querySelector('.mv-tab.on[data-tab="1"]') && document.querySelector(".mv-routing")`, 5000, "Routing"), "the Move turns to Routing: the page follows");
  await click('.mv-tab[data-tab="2"]');
  ok(await untilApi(async () => ((await mixNow()) || {}).page === 2, 5000), `a tap on Audio FX sets the Move's page (mix.page ${((await mixNow()) || {}).page})`);
  // Freeverb into audio FX 1 from the page's module list, a parameter dragged
  await until(`document.querySelector('.mv-modrow[data-mod="2"]')`, 5000, "the module list");
  await click('.mv-modrow[data-mod="2"]');
  ok(await untilApi(async () => { const m = await mixNow(); return m && m.afx[0].mod === 2 && m.afx[0].name === "Freeverb"; }, 8000), "Freeverb loaded into audio FX 1 from the page (the core's afx[0])");
  ok(await until(`document.querySelector('[data-ctl="p-1-0-0"]') && /Freeverb/.test(document.querySelector(".mv-modpick").textContent)`, 5000, "Freeverb's parameters"), "its parameters show (Room Size, ...)");
  const room0 = (await mixNow()).afx[0].vals[0];
  { const b = await box('[data-ctl="p-1-0-0"] .mv-dk'); await drag(b.x + b.w / 2, b.y + b.h / 2, b.x + b.w / 2, b.y + b.h / 2 - 60); }
  ok(await untilApi(async () => (await mixNow()).afx[0].vals[0] > room0 + 20, 4000), `a dial drag sets Room Size in the core (${room0} -> ${(await mixNow()).afx[0].vals[0]})`);
  ok(await until(`/wet for a send/.test(document.querySelector(".mv-fxedit").textContent)`, 2000, "the hint"), "Wet has the amber hint (100 % wet for a send)");
  await edit(21, 1 << 5 | 0 << 3 | 1, 99);   // (the Move's knob: Damping)
  ok(await until(`document.querySelector('[data-ctl="p-1-0-1"]').dataset.v === "99"`, 5000, "Damping 99"), `an edit on the Move (Damping 99) shows on the page (${await dv('[data-ctl="p-1-0-1"]')})`);
  // Mixer: a send dial, the level fader; the Move's edits show
  await click('.mv-tab[data-tab="3"]');
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
  // Routing: a firmware's track filter, a MIDI FX's one track
  await click('.mv-tab[data-tab="0"]');
  await until(`document.querySelector('.mv-modrow[data-mod="0"]')`, 5000, "the MIDI FX modules");
  await click('.mv-modrow[data-mod="0"]');
  ok(await untilApi(async () => (await mixNow()).mfx[0].mod === 0, 5000), "the Arpeggiator into MIDI FX 1 from the page");
  await click('.mv-tab[data-tab="1"]');
  await until(`document.querySelector('.mv-fw[data-slot="1"] .mv-st[data-track="9"]')`, 5000, "the filters");
  const filt0 = (await mixNow()).slots[1].filt;
  await click('.mv-fw[data-slot="1"] .mv-st[data-track="9"]');
  ok(await untilApi(async () => (await mixNow()).slots[1].filt === (filt0 ^ 0x100), 4000), `a routing cell: track 9 toggled in ChoralRoot's filter (${filt0.toString(16)} -> ${(await mixNow()).slots[1].filt.toString(16)})`);
  await click('.mv-picker .mv-st[data-track="7"]');
  ok(await untilApi(async () => (await mixNow()).mfx[0].filt === 64, 4000), `the Arpeggiator acts on track 7 only (${(await mixNow()).mfx[0].filt})`);
  await edit(19, 0, 0x00ff);
  ok(await until(`document.querySelector('.mv-fw[data-slot="0"] .mv-st[data-track="8"].on') && !document.querySelector('.mv-fw[data-slot="0"] .mv-st[data-track="9"].on')`, 5000, "Felucca 1-8"),
    "a filter set on the Move (Felucca: tracks 1-8) shows on the page");
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
  ok(await until(`document.querySelector("#mv-view.open.phone") && document.querySelector(".mv-ph-strip") && document.querySelector('.mv-tab.on[data-tab="3"]')`, 5000, "the phone sheet"), "Mix & FX from the slot menu: the full-screen sheet, Mixer");
  await sleep(600);
  await shot(join(DOCS, "move-page-phone-mixfx.png"));
  { const b = await box('.mv-ph-strip[data-slot="1"] [data-ctl="level-1"]'); await drag(b.x + b.w * 0.5, b.y + b.h / 2, b.x + b.w * 0.25, b.y + b.h / 2); }
  const lv1 = (await mixNow()).slots[1].level;
  ok(lv1 > 20 && lv1 < 45, `the phone's Level bar dragged: ChoralRoot's level in the core ${lv1}`);
  await edit(18, 1 * 8, 100);
  await click(".mv-close");
  ok(await until(`!document.querySelector("#mv-view.open")`, 3000, "close"), "the sheet's close button goes back to the faceplate");

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
