// SPDX-License-Identifier: MPL-2.0
// The Move's page end to end, on the Mac: the host on Move Remote's simulator (the build image, its :7730 published on
// 127.0.0.1), RemoteFM1's site from it (scripts/build_site.sh), in headless Chromium (no window, a fresh profile; the
// browser and the container ended by their ids) at http://remotefm1.local:PORT/ (the name mapped to 127.0.0.1 inside
// Chromium only), so the page is NOT a secure context, as on a real Move:
//   - every file the page asks for loads (no 4xx), no exception, no console error, no AudioContext, Web MIDI, WebUSB
//     or microphone asked for; no https redirect;
//   - the panel draws, the Move's live screen shows on it, the header names the firmware the Move runs;
//   - a click on a key of the drawn keybed sounds in the simulator (its mix peak);
//   - the firmware picker lists the Move's firmwares and loads ChoralRoot into the active slot (the host's status);
//     its skin (its .rfm1's rfm1.skin sections) shows; a load from outside the page (as the Move's own projects view)
//     is followed;
//   - a screenshot: docs/move-page.png (or --shot=FILE).
//   node tests/page_e2e.mjs [--shot=docs/move-page.png] [--port=N]          (after make; needs Docker and Chromium)
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, writeFileSync, existsSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { fileURLToPath } from "node:url";

const ROOT = fileURLToPath(new URL("../", import.meta.url));
const arg = (k, d) => { const a = process.argv.find((x) => x.startsWith(`--${k}=`)); return a ? a.slice(k.length + 3) : d; };
const SHOT = arg("shot", join(ROOT, "docs/move-page.png"));
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
  "mkdir -p /tmp/s && exec build/sim/remotefm1-host --root /tmp/r --sim-dir /tmp/s --firmwares build/fw --site build/site --fw felucca"],
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
    hidden: ["install-link", "vst-link", "ios-link", "android-link", "fm1-btn", "rec", "ed-btn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent === null; }).length,
    shown: ["fw-btn", "fw-src", "site-src", "menu-btn"].filter((i) => { const e = document.getElementById(i); return e && e.offsetParent !== null; }).length,
    start: document.getElementById("start").hidden, asked: window.__asked.slice(),
    plate: (() => { const c = document.getElementById("device"); return [c.width, c.height]; })(),
    lit: (() => { const p = FM1Site.screen().pixels; let n = 0; for (const v of p) if (v) n++; return n; })(),
    seq: FM1Site.screen().seq, fonts: [...document.fonts].filter((f) => f.status === "loaded").map((f) => f.family) })`);
  ok(page.href === BASE && page.secure === false, `plain http on ${HOSTNAME}: not a secure context, no https redirect (${page.href})`);
  ok(page.host === "move" && page.meta === "move", "the host flag: move (index.html's meta, <html data-host>)");
  ok(page.name === "Felucca" && page.ver === "1.8.2" && /RemoteFM1 on Move/.test(page.title), `the header names what the Move runs: ${page.name} ${page.ver} (${page.title})`);
  ok(page.hidden === 7 && page.shown === 4, `the header: firmware picker, Source, Site source, settings shown; Install, app links, FM-1, recorder, editor hidden (${page.shown}/4, ${page.hidden}/7)`);
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

  // ---- the firmware picker: the Move's firmwares; ChoralRoot into the active slot
  await click("#fw-btn");
  await until(`document.querySelector(".sheet.open .card[data-id]")`, 5000, "the firmware page");
  await sleep(700);   // (the sheet slides in)
  const cards = await js(`[...document.querySelectorAll(".sheet .card[data-id]")].map((c) => c.dataset.id)`);
  ok(cards.join() === "choralroot,felucca", `the picker lists the Move's firmwares: ${cards.join(", ")}`);
  const fileCard = await js(`!!document.querySelector(".sheet .card.file")`);
  ok(!fileCard, "no .rfm1 file opening in the browser");
  await click('.sheet .card[data-id="choralroot"]');
  ok(await untilApi(async () => (await slotId(0)) === "choralroot", 15000), `the pick loaded ChoralRoot into the Move's slot 1 (${await slotId(0)})`);
  ok(await until(`FM1Site.current().info && FM1Site.current().info.id === "choralroot" && document.body.classList.contains("skinned")`, 10000, "ChoralRoot's info and skin"),
    "the page shows ChoralRoot from the Move, with its skin (its .rfm1's rfm1.skin sections)");
  await sleep(1500);
  const cr = await js(`({ name: document.getElementById("fw-name").textContent, body: getComputedStyle(document.documentElement).getPropertyValue("--body").trim(),
    labels: FM1Site.current().info && FM1Site.current().info.buttonNames, seq: FM1Site.screen().seq })`);
  ok(cr.name === "ChoralRoot" && /201,\s*162,\s*107|#c9a26b/i.test(cr.body), `ChoralRoot in the header, the skin's colours on the page (--body ${cr.body})`);
  ok(Array.isArray(cr.labels) && cr.labels.length === 14, `its button names from the Move (${(cr.labels || []).join(" ")})`);
  await cdp("Page.captureScreenshot", { format: "png" }).then((r) => { writeFileSync(SHOT, Buffer.from(r.result.data, "base64")); console.log(`  shot  ${SHOT}`); });

  // ---- the Move switches by itself (as its projects view does; here the same load from outside the page): the page follows
  await fetch(API + "load", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ slot: 0, id: "felucca" }) });
  ok(await until(`document.getElementById("fw-name").textContent === "Felucca" && FM1Site.current().info && FM1Site.current().info.id === "felucca" && !document.body.classList.contains("skinned")`, 15000, "the follow"),
    "the Move loaded Felucca by itself: the page follows (its name, its info, ChoralRoot's skin gone)");

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
