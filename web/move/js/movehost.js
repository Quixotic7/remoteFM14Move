// SPDX-License-Identifier: GPL-3.0-only
// The "movehost" back end: RemoteFM1's page served by an Ableton Move running RemoteFM14Move. The engine is not in the
// browser: it is the firmware running on the Move, reached over the host's WebSocket (/ws, remoteFM14Move src/web.h).
// It stands in for js/audio.js's Player (the same calls and events, main.js makePlayer), so the rest of the page (the
// panel, the themes, the skins, the labels, the sheet) is RemoteFM1's own, unchanged. No AudioWorklet, Web MIDI,
// WebUSB or IndexedDB here: the page works on plain http (http://remotefm1.local:7730/, not a secure context).
//
//   const catalog = await moveCatalog()      the firmwares installed on the Move (GET /api/firmwares), as a catalog:
//                                            each an entry with engines ["move"]; .active: the id the Move's active
//                                            slot runs now (GET /api/info), or null
//   const p = new MoveHostPlayer(catalog, events)
//   await p.start()                          the WebSocket (no gesture needed: there is no audio here)
//   await p.load(entry, "move")             that firmware into the Move's active slot (POST /api/load: the engine does
//                                            what the Move's own FW_LOAD does); nothing when it is there already
//   p.panel(p)                               API.md section 2's panel; the changes go out as the host's ops (KEY, LEAN,
//                                            BTN, ENC, MASTER; the first MASTER is not sent: the Move's volume knob's
//                                            level stands until the page's MASTER moves, as the rfm1 engine does)
//   events: frame(pixels, seq), leds(levels, playGreen), info(info), labels({ seq, list }) as the worklet's;
//           moved(id) the Move's active slot runs another firmware (its own projects view, another track; null: none);
//           moveLink(up) the WebSocket went away / came back; error(text) a pick the Move could not start
//   Mix & FX (js/moveslots.js, js/movemix.js): p.listen({ mix(m), meters(u8[7]), mini(slot, px565[48*48]) }) adds a
//           listener (it gets the last ones at once); p.mix / p.meters / p.minis the last ones; p.edit(op, a, b) one
//           of the host's Mix & FX ops (engine.h ENG_OP_*: 16 ACTIVE .. 24 FXPG) over /ws (POST /api/edit while /ws is
//           down)
// Everything else Player has (MIDI, flash, the recorder's taps, the output device, live input) is a no-op: the
// firmware's flash and states stay on the Move, and its sound comes out of the Move.
import { CATEGORIES, HUB, rfm1Source, shortAuthor } from "./live.js";

const OP = { KEY: 1, LEAN: 2, BTN: 3, ENC: 4, MASTER: 5 };   // remoteFM14Move src/fwrt.h FWI_*
const NKEY = 27, NBTN = 14, NENC = 7, W = 240, MINI = 48;
const LOAD_MS = 20000;                                        // a pick that hasn't started by then: said so

// the credits the sheet's About page names (catalog.json's, as RemoteFM1's site has them)
const CREDITS = {
  fm1emu: { name: "fm1-emu", author: "Joakim Eriksson", license: "MIT", source: "https://github.com/joakimeriksson/fm1-emu" },
  fm1vst: { name: "FM1VST", license: "GPL-3.0-only", source: "https://github.com/Quixotic7/FM1VST" },
  hub: { name: "The FM-1 Firmware Hub", source: HUB },
};

// one firmware of /api/firmwares as a catalog entry (live.js's shape, with an .rfm1 from the Move)
export function moveEntry(f) {
  const authors = Array.isArray(f.authors) ? f.authors.map(String).filter(Boolean) : [];
  const cats = Array.isArray(f.categories) && f.categories.length ? f.categories.filter((c) => CATEGORIES.includes(c)) : [];
  if (!cats.length) cats.push("Other");
  const src = rfm1Source(f.source_url);
  return {
    id: f.id, name: f.name || f.id, version: f.version, author: shortAuthor(authors[0]), authorFull: authors.join(", ") || "unknown",
    category: cats[0], categories: cats, tag: "On this Move",
    what: `${f.name || f.id} ${f.version}, installed on this Move: its .rfm1, compiled for the Move.`,
    license: f.license || null, source: src ? src.url : null, beta: false,
    engines: ["move"], defaultEngine: "move", native: null, emu: null,
    rfm1: { version: f.version, sourceUrl: f.source_url || null, sha256: f.sha256 || null, fileName: f.file || `${f.base}.rfm1`,
            builtBy: f.built_by || null, from: "move" },
    move: { base: f.base, skin: Array.isArray(f.skin) ? f.skin : [], licenses: !!f.licenses },
  };
}

async function getJson(url, fetchFn) {
  const r = await fetchFn(url, { cache: "no-cache" });
  if (!r.ok) throw new Error(`${url}: HTTP ${r.status}`);
  return r.json();
}

export async function moveCatalog(fetchFn = (u, o) => fetch(u, o)) {
  const [list, info] = await Promise.all([getJson("api/firmwares", fetchFn), getJson("api/info", fetchFn).catch(() => null)]);
  const firmwares = (list.firmwares || []).map(moveEntry);
  const used = new Set(firmwares.flatMap((f) => f.categories));
  const slot = info && Array.isArray(info.slots) ? info.slots[info.active] : null;
  return {
    about: "The firmwares installed on this Ableton Move (RemoteFM14Move: GET /api/firmwares)",
    move: true, live: false, hub: HUB, hubUpdated: null,
    categories: CATEGORIES.filter((c) => c === "All" || used.has(c)),
    paths: { engines: {}, installer: "https://remotefm1.com/installer/" },
    credits: CREDITS,
    firmwares,
    active: slot && firmwares.some((f) => f.id === slot.id) ? slot.id : null,
  };
}

const SKIN_SECTIONS = { "rfm1skin": "rfm1.skin", "rfm1skin.bg": "rfm1.skin.background", "rfm1skin.bgl": "rfm1.skin.background.landscape" };

export class MoveHostPlayer {
  constructor(catalog, events = {}) {
    this.catalog = catalog;
    this.ev = events;
    this.ws = null; this.up = null;              // the WebSocket; whether it is open (null: not yet)
    this.info = null;                            // the host's last info JSON
    this.cur = undefined;                        // the firmware id the Move's active slot runs (null: none)
    this.want = null; this.wantTimer = 0;        // a pick on its way to the Move
    this.failSeq = null;
    this.lseq = -1;
    this.lcd = new Uint16Array(W * W); this.seq = 0; this.framePending = false;
    this.last = { keys: 0, buttons: 0, lean: new Uint8Array(NKEY), master: undefined };
    this.extras = new Map();                     // firmware base -> Promise<{ credits, skin }>
    this.infoP = Promise.resolve();
    this.ready = new Promise((r) => { this.readyNow = r; });
    this.subs = [];                              // Mix & FX listeners (listen)
    this.mix = null; this.meters = new Uint8Array(7); this.minis = [null, null, null];
    // (Player's surface that main.js touches; nothing of it applies here)
    this.ctx = null; this.node = null; this.current = null; this.volume = 1; this.muted = false; this.tapOn = false;
  }
  emit(name, ...a) {
    if (this.ev[name]) this.ev[name](...a);
    for (const l of this.subs) if (l[name]) { try { l[name](...a); } catch (e) { console.warn("movehost:", name, e); } }
  }
  // a Mix & FX listener: { mix(m), meters(levels), mini(slot, px) }; what came already, at once
  listen(l) {
    this.subs.push(l);
    if (this.mix && l.mix) l.mix(this.mix);
    this.minis.forEach((px, s) => { if (px && l.mini) l.mini(s, px); });
    return () => { this.subs = this.subs.filter((x) => x !== l); };
  }
  // one of the host's Mix & FX ops (engine.h ENG_OP_*; b 16 bits: -1, 0..127, or a 16-track mask up to 0xFFFF, which the
  // host reads as an int16): over /ws, else POST /api/edit
  edit(op, a, b = 0) {
    b = (b << 16) >> 16;
    const ws = this.ws;
    if (ws && ws.readyState === 1) { ws.send(new Uint8Array([op, a & 255, b & 255, (b >> 8) & 255])); return; }
    fetch("api/edit", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ op, a, b }) }).catch(() => {});
  }
  get running() { return true; }
  get audioState() { return "running"; }
  get sampleRate() { return 0; }

  unlock() {}
  async tryAutostart() { return this.start(); }
  async start() { this.connect(); return true; }
  async applySink() {}
  async setSink() {}

  connect() {
    if (this.ws) return;
    const ws = new WebSocket(`${location.protocol === "https:" ? "wss:" : "ws:"}//${location.host}/ws`);
    ws.binaryType = "arraybuffer";
    this.ws = ws;
    ws.onopen = () => {
      this.last = { keys: 0, buttons: 0, lean: new Uint8Array(NKEY), master: this.last.master };   // (the host's view: nothing held)
      if (this.up === false) this.emit("moveLink", true);
      this.up = true;
    };
    ws.onmessage = (m) => {
      if (typeof m.data === "string") {
        let j;
        try { j = JSON.parse(m.data); } catch (e) { console.warn("movehost: info", e); return; }
        if (j && j.type === "mix") { this.mix = j; this.emit("mix", j); } else this.onInfo(j);
        return;
      }
      const d = new Uint8Array(m.data);
      if (d[0] === 1 && d.length >= 3) this.onRows(d[1], d[2], d.subarray(3));
      else if (d[0] === 2 && d.length >= 46) this.emit("leds", d.slice(1, 45), d[45]);
      else if (d[0] === 3 && d.length >= 8) { this.meters = d.slice(1, 8); this.emit("meters", this.meters); }
      else if (d[0] === 4 && d.length >= 3 + MINI * MINI * 2 && d[1] < 3 && d[2] === MINI) {
        const px = new Uint16Array(MINI * MINI), s = d.subarray(3);
        for (let i = 0; i < px.length; i++) px[i] = s[2 * i] << 8 | s[2 * i + 1];
        this.minis[d[1]] = px;
        this.emit("mini", d[1], px);
      }
    };
    ws.onclose = () => {
      this.ws = null;
      if (this.up !== false) this.emit("moveLink", false);
      this.up = false;
      setTimeout(() => this.connect(), 1000);
    };
  }

  // the screen's changed rows (RGB565 big-endian) into the frame; one frame per animation frame
  onRows(y0, n, d) {
    const px = this.lcd;
    for (let y = 0; y < n && y0 + y < W; y++) {
      const o = (y0 + y) * W, s = y * W * 2;
      for (let x = 0; x < W; x++) px[o + x] = d[s + 2 * x] << 8 | d[s + 2 * x + 1];
    }
    if (this.framePending) return;
    this.framePending = true;
    const go = () => { this.framePending = false; this.emit("frame", this.lcd.slice(), ++this.seq); };
    if (typeof requestAnimationFrame === "function" && !document.hidden) requestAnimationFrame(go); else setTimeout(go, 33);
  }

  onInfo(j) {
    const first = !this.info;
    this.info = j;
    const slot = Array.isArray(j.slots) ? j.slots[j.active] : null, id = slot ? slot.id : null;
    // a pick the Move could not start: said, and the page back on what the Move runs
    if (this.failSeq !== null && j.fail_seq !== this.failSeq && this.want && j.failed === this.want) {
      const e = this.entryOf(this.want);
      this.want = null; clearTimeout(this.wantTimer);
      this.emit("error", `${e ? e.name : j.failed} could not start on the Move.`);
      if (id === this.cur) this.revert();
    }
    this.failSeq = j.fail_seq;
    if (id !== this.cur) {
      this.cur = id;
      if (this.want === id) { this.want = null; clearTimeout(this.wantTimer); }
      this.lseq = -1;
      this.emit("moved", id);
      if (id) this.infoP = this.emitInfo(j);
    }
    if (id && (j.lseq | 0) !== this.lseq) this.emitLabels(j);
    if (first) this.readyNow();
  }
  emitLabels(j) {
    const id = this.cur;
    this.lseq = j.lseq | 0;
    const list = this.lseq && Array.isArray(j.labels) ? j.labels.map((x) => (typeof x === "string" ? x : null)) : null;
    this.infoP.then(() => { if (this.cur === id) this.emit("labels", { seq: list ? this.lseq : 0, list }); });
  }
  // the page back on what the Move runs (a pick that did not start)
  revert() {
    this.emit("moved", this.cur);
    if (!this.cur) return;
    this.infoP = this.emitInfo(this.info);
    this.emitLabels(this.info);
  }
  entryOf(id) { return (this.catalog.firmwares || []).find((f) => f.id === id) || null; }

  // a firmware's licences (the About page's credits) and skin (its .rfm1's sections), fetched once
  extrasOf(e) {
    const base = e && e.move && e.move.base;
    if (!base) return Promise.resolve({ credits: null, skin: null });
    if (!this.extras.has(base)) {
      const bytes = (suf) => fetch(`api/fw/${base}.${suf}`).then((r) => (r.ok ? r.arrayBuffer() : null)).then((b) => (b ? new Uint8Array(b) : null)).catch(() => null);
      this.extras.set(base, (async () => {
        const credits = e.move.licenses ? await fetch(`api/fw/${base}.licenses.txt`).then((r) => (r.ok ? r.text() : null)).catch(() => null) : null;
        let skin = null;
        for (const suf of e.move.skin) {
          const b = SKIN_SECTIONS[suf] ? await bytes(suf) : null;
          if (b) (skin = skin || {})[SKIN_SECTIONS[suf]] = b;
        }
        return { credits, skin };
      })());
    }
    return this.extras.get(base);
  }

  // the worklet's "info" (API.md section 2) for the active slot's firmware, from the host's info
  async emitInfo(j) {
    const slot = j.slots[j.active], e = this.entryOf(slot.id), x = await this.extrasOf(e);
    if (this.cur !== slot.id) return;
    const km = Array.isArray(j.keymap) && j.keymap.some((v) => v >= 0) ? Array.from({ length: 6 }, (_, r) => j.keymap.slice(r * 11, r * 11 + 11)) : null;
    this.emit("info", {
      type: "info", id: slot.id, name: slot.name, version: slot.version, license: slot.license || (e && e.license) || null,
      source: e ? e.source : null, family: e ? e.category : null, latencyFrames: 0, keymap: km,
      buttonNames: Array.isArray(j.buttons) ? j.buttons.map((b) => (b && b[0]) || "") : null,
      credits: x.credits, skin: x.skin, slot: j.active,
    });
  }

  async load(entry, engine) {
    this.current = { entry, engine };
    if (engine !== "move") return;
    this.connect();
    await this.ready;
    if (this.cur === entry.id) {        // there already (the page's first look, or a pick of what runs)
      this.want = null;
      await this.infoP;
      this.infoP = this.emitInfo(this.info);
      this.emitLabels(this.info);
      return;
    }
    this.want = entry.id;
    const r = await fetch("api/load", { method: "POST", headers: { "Content-Type": "application/json" },
                                        body: JSON.stringify({ slot: this.info.active | 0, id: entry.id }) });
    if (!r.ok) {
      if (this.want === entry.id) this.want = null;
      this.revert();
      throw new Error(`The Move could not load ${entry.name} (HTTP ${r.status}).`);
    }
    clearTimeout(this.wantTimer);
    this.wantTimer = setTimeout(() => {
      if (this.want !== entry.id) return;
      this.want = null;
      this.emit("error", `${entry.name} did not start on the Move.`);
      this.revert();
    }, LOAD_MS);
  }

  // the panel (API.md section 2): what changed since the last one, as the host's 4-byte ops
  panel(p) {
    const ws = this.ws;
    if (!ws || ws.readyState !== 1) return;
    const L = this.last, ops = [];
    const op = (o, a, b) => ops.push(o, a & 255, b & 255, (b >> 8) & 255);
    const keys = (p.keys >>> 0) & ((1 << NKEY) - 1), btns = (p.buttons >>> 0) & ((1 << NBTN) - 1);
    const tk = (p.taps && p.taps.keys) >>> 0, tb = (p.taps && p.taps.buttons) >>> 0;
    for (let k = 0; k < NKEY; k++) {
      const bit = 1 << k, down = !!(keys & bit), was = !!(L.keys & bit);
      if (down && !was) op(OP.KEY, k, (p.keyVel && p.keyVel[k]) || 100);
      else if (!down && was) op(OP.KEY, k, 0);
      else if (!down && (tk & bit)) { op(OP.KEY, k, (p.keyVel && p.keyVel[k]) || 100); op(OP.KEY, k, 0); }
      const lean = down && p.keyLean ? p.keyLean[k] | 0 : 0;
      if (down && lean !== L.lean[k]) op(OP.LEAN, k, lean);
      L.lean[k] = lean;
    }
    for (let b = 0; b < NBTN; b++) {
      const bit = 1 << b, down = !!(btns & bit), was = !!(L.buttons & bit);
      if (down !== was) op(OP.BTN, b, down ? 1 : 0);
      else if (!down && (tb & bit)) { op(OP.BTN, b, 1); op(OP.BTN, b, 0); }
    }
    if (p.enc) for (let i = 0; i < NENC; i++) { const n = p.enc[i] | 0; if (n) op(OP.ENC, i, Math.max(-32768, Math.min(32767, n))); }
    if (typeof p.master === "number") {
      const m = Math.max(0, Math.min(1023, p.master | 0));
      if (L.master !== undefined && m !== L.master) op(OP.MASTER, 0, m);
      L.master = m;
    }
    L.keys = keys; L.buttons = btns;
    if (ops.length) ws.send(new Uint8Array(ops));
  }

  // (nothing of these on the Move: no MIDI path to the firmware, its flash stays there, no sound in the browser)
  midi() {}
  midiCut() {}
  flashReq() {}
  audioNow() { return null; }
  click() {}
  setVolume(v) { this.volume = v; }
  setMuted(on) { this.muted = !!on; }
  tap(on) { this.tapOn = !!on; }
  tapBuf() {}
  recStart() {}
  recStop() {}
  recStopAt() {}
  recBuf() {}
  fsReply() {}
  fsChanged() {}
  unload() { this.current = null; }
  async ainStart() { throw new Error("no audio input on the Move's page"); }
  ainStop() {}
  glitches() { return null; }
}
