// SPDX-License-Identifier: GPL-3.0-only
// The Sampler window on RemoteFM1's page served by an Ableton Move (RemoteFM14Move; only when js/host.js ON_MOVE): the
// Move's sampler app (rfm1cs-abletonmove's, README "The sampler") and its takes on the page, opened from the header's
// Sampler cap (the phone: its slot menu), laid out as the Mix & FX view (js/movemix.js: a panel over the faceplate, a
// full-screen dark sheet on the phone). One state, two views: what shows is the Move's (/ws {"type":"smp"}: src/engine.c
// smp_json), every change goes to the Move's core (moveui.js OP.SMP_SET: rfm1cs rfm_move_smp_set, as the app's popup)
// and comes back.
//   Record  Source (Master, Slot 1..3 by the firmware's name, Input, Stems), Mode (Capture, Bars, Threshold, Manual),
//           Length, Threshold, Input level; the record button (OP.SMP_PRESS: Sample in the app); the state (idle,
//           armed, listening, recording, saving), the bar or the time, the level with the threshold's mark, the wave
//   Takes   the days, a day's takes newest first (the source's sign, the length, the tempo, the wave): play in the
//           browser (an <audio> on the take's WAV, GET /api/takes/wav: Range answered) or on the Move (its toggle:
//           POST /api/takes/play), download, rename, delete; Tools on the chosen take: Trim (two markers on its wave),
//           Normalize, Reverse, Fade in / out with a length; one Undo, the Move's (the take library's, src/takes.c).
//           A stems take: play, download and delete only. Nothing here starts the song.
//   Kit     (rfm1cs stage 3c, only with the kit: /ws {"type":"kit"}, src/engine.c kit_json) the 16 pads as the Move's left
//           4 x 4 (pad 1 bottom left, a row a kit track's colour, the pads sounding lit): a press plays the pad (held:
//           a momentary or a loop sounds while held) and chooses it; the chosen pad's wave with its region and every
//           setting (pitch, start, end, play mode, attack, hold, release, choke, reverse, level, pan, mute, stretch, the
//           sends), each as the Move's knobs set it (POST /api/kit, the core's rfm_move_kit_set); the kit's name, Save
//           (a new kit) and Load (the kits saved, GET /api/kit/kits). The Move's screen and pads follow.
// The takes are named by the library's ids only (src/web.c /api/takes).
import { OP, h, svg, IC, bar, segs, choice, closePop, openPop, popItem } from "./moveui.js";
import { download } from "./movefs.js";

const TABS = ["Record", "Takes", "Kit"];
// (the Kit tab) the kit tracks' colours (13 pink .. 16 violet, as the Move's pads), a pad's settings (RFM_MOVE_KIT_P_*),
// the envelope's steps (ms, as the knobs)
const KROW = ["#FF3CA0", "#FF8C28", "#78DCFF", "#AA78FF"];
const KP = { PITCH: 0, START: 1, END: 2, MODE: 3, ATTACK: 4, HOLD: 5, RELEASE: 6, CHOKE: 7, REVERSE: 8, LEVEL: 9, PAN: 10, MUTE: 11,
  STRETCH: 12, SEND1: 13, SEND2: 14, SEND3: 15 };
const KFRAC = 32767;
const ENV = [0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 40, 50, 75, 100, 150, 200, 300, 400, 500, 750, 1000, 1500, 2000, 3000, 5000, 10000];
const envIdx = (ms) => { let b = 0; ENV.forEach((v, i) => { if (Math.abs(v - ms) < Math.abs(ENV[b] - ms)) b = i; }); return b; };
const msText = (ms) => (ms >= 1000 ? `${(ms / 1000).toFixed(ms % 1000 ? 1 : 0)} s` : `${ms} ms`);
const SRC = [["M", "Master"], ["1", "Slot 1"], ["2", "Slot 2"], ["3", "Slot 3"], ["In", "Input"], ["S", "Stems"], ["L", "Looper"], ["K", "Kit"]];
const MODES = ["Capture", "Bars", "Threshold", "Manual"];
const LENS = [0, 1, 2, 4, 8, 16, 32];
const FADES = [5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000];
const K = { SOURCE: 0, MODE: 1, LENGTH: 2, THRESHOLD: 3, INPUT: 4 };
const RED = "#FF5F57", AMBER = "#F5A524", GREEN = "#3BD47A";
const SIC = {
  rec: '<circle cx="8" cy="8" r="5" fill="currentColor" stroke="none"/>',
  stop: '<rect x="4" y="4" width="8" height="8" rx="1.5" fill="currentColor" stroke="none"/>',
  play: '<path d="M5 3.2v9.6L12.8 8z" fill="currentColor"/>',
  pen: '<path d="M10.5 2.5l3 3L6 13H3v-3z"/>',
  down: '<path d="M8 2v9M4.5 7.5L8 11l3.5-3.5M3 14h10"/>',
  undo: '<path d="M5.5 3L2.5 6l3 3"/><path d="M2.5 6H10a3.5 3.5 0 010 7H6"/>',
  tools: '<path d="M2 4h7M13 4h1M2 8h2M8 8h6M2 12h8M14 12h0"/><circle cx="11" cy="4" r="1.8"/><circle cx="6" cy="8" r="1.8"/><circle cx="12" cy="12" r="1.8"/>',
  move: '<path d="M2 6h2.5L8 3v10L4.5 10H2z"/><path d="M10.5 5.5a3.5 3.5 0 010 5M12.5 3.5a6.3 6.3 0 010 9"/>',
  trim: '<path d="M3.5 2v12M12.5 2v12"/><path d="M3.5 8h9" stroke-dasharray="1.6 1.8"/>',
  norm: '<path d="M2 2.5h12"/><path d="M4.5 13.5V6M8 13.5V4.5M11.5 13.5V7"/>',
  rev: '<path d="M13 5H3.5M6 2.5L3.5 5 6 7.5M3 11h9.5M10 8.5l2.5 2.5-2.5 2.5"/>',
  fin: '<path d="M2 13.5L14 3v10.5z"/>',
  fout: '<path d="M2 3l12 10.5H2z"/>',
};
const OPS = [["trim", "Trim", SIC.trim], ["normalize", "Normalize", SIC.norm], ["reverse", "Reverse", SIC.rev], ["fade-in", "Fade in", SIC.fin], ["fade-out", "Fade out", SIC.fout]];

const dB = (lvl) => (lvl > 0 ? Math.max(-60, 20 * Math.log10(lvl / 32767)) : -60);
const clock = (s) => { s = Math.max(0, s); const m = Math.floor(s / 60); return `${m}:${(s - m * 60).toFixed(1).padStart(4, "0")}`; };
const hexPeaks = (s) => { const n = (s || "").length >> 1, a = new Uint8Array(n); for (let i = 0; i < n; i++) a[i] = parseInt(s.substr(2 * i, 2), 16); return a; };
// a take's name: its label (between the head "MMDD-HHMMSS " and ".wav"); its time from the head
const label = (n) => String(n || "").replace(/^\d{4}-\d{6} /, "").replace(/\.wav$/i, "");
const hhmm = (n) => { const m = /^\d{4}-(\d\d)(\d\d)\d\d /.exec(n || ""); return m ? `${m[1]}:${m[2]}` : ""; };
const dayName = (d) => { if (d.today) return "Today"; const t = new Date(`${d.date}T12:00:00`); return isNaN(t) ? d.date : t.toLocaleDateString(undefined, { month: "short", day: "numeric" }); };
const lenText = (t) => (t.bars ? `${t.bars} bar${t.bars === 1 ? "" : "s"}` : `${(t.frames / (t.rate || 44100)).toFixed(t.frames / (t.rate || 44100) < 10 ? 2 : 1)} s`);
let ls = null;
try { ls = window.localStorage; } catch (_) { ls = null; }
const remember = (k, v) => { try { if (ls) ls.setItem(k, v); } catch (_) {} };
const recall = (k) => { try { return ls ? ls.getItem(k) : null; } catch (_) { return null; } };

export class SmpView {
  constructor({ store, ui, player, phone }) {
    this.store = store; this.ui = ui; this.player = player; this.phoneMq = phone;
    this.isOpen = false;
    this.tab = Math.max(0, Math.min(2, +(recall("mv-smp-tab") || 0) | 0));
    this.kit = player.kit || null;                  // (the Kit tab) the kit as the Move's core has it
    this.kits = null; this.kitsVer = -1;            // the kits saved (GET /api/kit/kits), asked again after a save
    this.smp = player.smp || null;
    this.ups = []; this.key = "";
    this.list = null; this.day = 0; this.listVer = -1; this.listing = false;
    this.sel = 0;                                   // the take the Tools work on (its id; 0 none)
    this.trim = { id: 0, a: 0, b: 0 };             // Trim's markers (frames)
    this.fade = 4;                                  // the fades' length (FADES index: 100 ms)
    this.onMove = recall("mv-smp-move") === "1";   // play on the Move's output, not the browser's
    this.rev = new Map();                           // id -> edits seen (its peaks and WAV asked again)
    this.peaks = new Map(); this.pq = []; this.pbusy = false;
    this.playing = 0;                               // the take playing in the browser
    this.audio = new Audio();                       // (not in the page: the window's rebuilds never touch it)
    this.audio.preload = "none";
    this.audio.addEventListener("ended", () => { this.playing = 0; this.drawHeads(); this.refreshPlay(); });
    this.audio.addEventListener("pause", () => { this.drawHeads(); this.refreshPlay(); });
    this.audio.addEventListener("error", () => { if (this.playing) { this.playing = 0; this.refreshPlay(); this.store.toast("The take could not play here.", "bad"); } });
    this.el = h("section", { class: "mv-view mv-smp", id: "mv-smp", "aria-label": "Sampler", "aria-hidden": "true", inert: true });
    document.body.append(this.el);
    player.listen({ smp: (j) => this.onSmp(j), kit: (j) => this.onKit(j) });
    if (!this.kit) fetch("api/kit", { cache: "no-cache" }).then((r) => (r.ok ? r.json() : null)).then((j) => { if (j && !this.kit) this.onKit(j); }).catch(() => {});
    if (!this.smp) fetch("api/smp", { cache: "no-cache" }).then((r) => (r.ok ? r.json() : null)).then((j) => { if (j && !this.smp) this.onSmp(j); }).catch(() => {});
    store.on((what) => { if (what === "mix" && this.isOpen) this.render(); });
    phone.addEventListener("change", () => { this.key = ""; this.render(); });
    this.raf = 0;
  }
  get ph() { return this.phoneMq.matches; }
  up(fn) { this.ups.push(fn); }
  get on() { return !!(this.smp && this.smp.on); }
  get set() { return (this.smp && this.smp.set) || [0, 1, 0, -24, 0, 0, 0]; }
  get busy() { return !!(this.smp && this.smp.state); }

  open(tab) {
    if (tab != null) this.setTab(tab);
    if (this.isOpen) return;
    if (this.ui.view && this.ui.view.isOpen) this.ui.view.close();   // (one window over the faceplate)
    this.isOpen = true; this.key = ""; this.listVer = -1;
    this.render(); this.setOpen();
  }
  close() {
    if (!this.isOpen) return;
    this.isOpen = false; closePop(); this.setOpen();
    if (this.playing) { this.audio.pause(); this.playing = 0; }
  }
  toggle() { if (this.isOpen) this.close(); else this.open(); }
  escape() { if (closePop()) return; this.close(); }
  setOpen() {
    this.el.classList.toggle("open", this.isOpen);
    this.el.toggleAttribute("inert", !this.isOpen);
    this.el.setAttribute("aria-hidden", this.isOpen ? "false" : "true");
    if (this.ui.smpBtn) { this.ui.smpBtn.classList.toggle("lit", this.isOpen); this.ui.smpBtn.setAttribute("aria-pressed", this.isOpen ? "true" : "false"); }
    if (this.isOpen) this.el.focus({ preventScroll: true });
  }
  get kitOn() { return !!(this.kit && this.kit.on); }
  setTab(i) {
    if (i === this.tab) return;
    this.tab = i; closePop(); remember("mv-smp-tab", String(i));
    this.listVer = -1;
    this.render();
  }

  // ---- the Move's state: the controls follow it; the list again when the takes changed
  onSmp(j) {
    this.smp = j;
    if (!this.isOpen) return;
    if (this.tab === 1 && j.takes_ver !== this.listVer) this.loadList();
    this.render();
  }
  // (the Kit tab) the kit came: the tab's controls follow; the kits saved asked again after a save or a load
  onKit(j) {
    this.kit = j;
    if (!this.isOpen) return;
    if (this.tab === 2 && j.on && (j.saved | 0) + (j.kits | 0) * 1000 !== this.kitsVer) this.loadKits();
    this.render();
  }
  render() {
    if (!this.isOpen) return;
    const st = this.store, s = this.smp || {}, l = this.list, k = this.kit || {};
    if (this.tab === 2 && !this.kitOn) this.tab = 0;
    const key = JSON.stringify([this.tab, this.ph, s.on, s.takes_on, s.srcs, s.sets, st.slots.map((x) => [x.on, x.name]), this.kitOn,
      this.tab === 1 && l ? [l.day, l.days.map((d) => [d.id, d.takes]), l.takes.map((t) => [t.id, t.name, t.frames]), this.sel, this.onMove] : 0,
      this.tab === 2 && k.pads ? [k.cur, k.pads.map((p) => [p.n, p.has, p.st, p.f]), this.kits ? this.kits.map((x) => x.id) : null] : 0]);
    if (key !== this.key) { this.key = key; this.build(); }
    for (const f of this.ups) f();
    this.paint();
    this.drawHeads();
  }
  build() {
    this.ups = [];
    const scroll = this.body && this.builtTab === this.tab ? this.body.scrollTop : 0;
    this.builtTab = this.tab;
    const tabs = h("div", { class: "mv-tabs", role: "tablist" }, TABS.map((t, i) => (i === 2 && !this.kitOn ? null : h("button", { class: `mv-tab ${i === this.tab ? "on" : ""}`, role: "tab",
      "aria-selected": i === this.tab ? "true" : "false", "data-tab": i, onclick: () => this.setTab(i) }, t))));
    const live = h("span", { class: "mv-live mv-smp-live" });
    this.up(() => { const s = this.smp; live.hidden = !(s && s.shown); live.textContent = "On the Move's screen"; });
    const head = this.ph
      ? [h("div", { class: "mv-ph-top" }, h("b", { class: "mv-title" }, "Sampler"), live,
          h("button", { class: "mv-iconbtn mv-close", "aria-label": "Close the Sampler", onclick: () => this.close() }, svg(IC.close, 16))), tabs]
      : [h("div", { class: "mv-bar-top" }, tabs, live, h("span", { class: "mv-spacer" }),
          h("button", { class: "mv-pill mv-back", onclick: () => this.close() }, "Back to the faceplate", h("span", { class: "mv-val mv-mut" }, "Esc")))];
    this.body = h("div", { class: `mv-body mv-smp-body mv-smp-tab-${this.tab}` }, this.tab === 2 ? this.kitTab() : !this.on ? this.none() : this.tab ? this.takes() : this.record());
    this.el.classList.toggle("phone", this.ph);
    this.el.replaceChildren(h("div", { class: "mv-head" }, head), this.body);
    this.el.tabIndex = -1;
    this.body.scrollTop = scroll;
  }
  none() { return h("div", { class: "mv-empty-in mv-smp-none" }, svg(SIC.rec, 28), h("b", {}, "No recorder on this Move")); }

  // ================================================================ Record
  setV(k, v) { this.store.edit(OP.SMP_SET, k, v); }
  // a segmented choice over values (its own value a moment after a click, then the Move's)
  segv(o) {
    const st = { local: 0, until: 0 };
    const el = h("div", { class: "mv-segs mv-smp-segs", role: "radiogroup", "aria-label": o.label, "data-ctl": o.key });
    const btns = o.opts.map(([v, t, c]) => h("button", { class: "mv-seg", role: "radio", "data-v": v, style: { "--c": c || null },
      onclick: () => { if (this.busy) return; st.local = v; st.until = Date.now() + 800; o.set(v); refresh(); setTimeout(refresh, 850); } },
      c ? h("span", { class: "mv-dot", style: { "--c": c } }) : null, h("span", { class: "mv-seg-t" }, t)));
    el.append(...btns);
    const refresh = () => {
      const v = Date.now() < st.until ? st.local : o.get(), off = this.busy;
      btns.forEach((b) => { const on = +b.dataset.v === v; b.classList.toggle("on", on); b.setAttribute("aria-checked", on ? "true" : "false"); b.disabled = off; });
      if (o.text) btns.forEach((b, i) => { b.querySelector(".mv-seg-t").textContent = o.text(o.opts[i][0]); });
      el.classList.toggle("off", off); el.dataset.v = v;
    };
    this.up(refresh); refresh();
    return el;
  }
  record() {
    const s = this.smp, st = this.store, offered = (bit) => ((s.srcs >>> bit) & 1) === 1;
    const srcOpts = SRC.map(([sign, name], v) => {
      if (!offered(v)) return null;
      const slot = v >= 1 && v <= 3 ? st.slot(v - 1) : null;
      return [v, slot ? (slot.on ? slot.name : name) : name, slot ? st.colorOf(v - 1) : null];
    }).filter(Boolean);
    const row = (k, name, ctl) => (((s.sets >>> k) & 1) ? h("div", { class: "mv-smp-set", "data-set": k }, h("span", { class: "mv-h" }, name), ctl) : null);
    const settings = h("div", { class: "mv-smp-sets" },
      row(K.SOURCE, "Source", this.segv({ key: "source", label: "Source", opts: srcOpts, get: () => this.set[K.SOURCE], set: (v) => this.setV(K.SOURCE, v) })),
      row(K.MODE, "Mode", this.segv({ key: "mode", label: "Mode", opts: MODES.map((m, v) => [v, m]), get: () => this.set[K.MODE], set: (v) => this.setV(K.MODE, v) })),
      row(K.LENGTH, "Length", this.segv({ key: "length", label: "Length", opts: LENS.map((b) => [b, b ? String(b) : "Open"]), get: () => this.set[K.LENGTH],
        set: (v) => this.setV(K.LENGTH, v), text: (b) => (b ? String(b) : this.set[K.MODE] === 0 ? "Auto" : "Open") })),
      row(K.THRESHOLD, "Threshold", bar(this, { key: "threshold", label: "Threshold", cls: "thin", min: -60, max: 0, color: () => AMBER,
        get: () => this.set[K.THRESHOLD], set: (v) => this.setV(K.THRESHOLD, v), fmt: (v) => `${v} dB`, off: () => this.busy })),
      row(K.INPUT, "Input level", bar(this, { key: "input", label: "Input level", cls: "thin", min: -24, max: 24, color: () => GREEN,
        get: () => this.set[K.INPUT], set: (v) => this.setV(K.INPUT, v), fmt: (v) => `${v > 0 ? "+" : ""}${v} dB`, off: () => this.busy })));
    // the monitor: the state, the bar or the time, the record button; the wave; the level with the threshold's mark
    const state = h("span", { class: "mv-smp-state", id: "mv-smp-state" }, h("i"), h("b"));
    const count = h("span", { class: "mv-smp-count mv-val", id: "mv-smp-count" });
    const sign = h("span", { class: "mv-smp-sign mv-smp-srcsign" });
    const rec = h("button", { class: "mv-smp-rec", id: "mv-smp-rec", onclick: () => this.store.edit(OP.SMP_PRESS, 0, 0) }, svg(SIC.rec, 26));
    this.wave = h("canvas", { class: "mv-smp-wave", id: "mv-smp-wave", height: 120 });
    const lvI = h("i"), lvT = h("b");
    const level = h("div", { class: "mv-smp-lv", id: "mv-smp-lv", "aria-hidden": "true" }, lvI, lvT);
    const lvText = h("span", { class: "mv-val mv-mut mv-smp-lvt" });
    this.up(() => {
      const s = this.smp, mode = this.set[K.MODE], stt = s.state | 0;
      const name = stt === 1 ? "Armed" : stt === 2 ? "Recording" : stt === 3 ? "Saving" : s.listening ? "Listening" : "Idle";
      state.dataset.state = name.toLowerCase(); state.querySelector("b").textContent = name;
      state.style.setProperty("--c", stt === 2 ? RED : stt === 1 ? RED : stt === 3 ? AMBER : s.listening ? GREEN : "var(--muted)");
      const secs = (s.frames || 0) / (s.rate || 44100);
      count.textContent = stt === 1 ? (s.bars ? `0/${s.bars}` : "") : stt === 2 ? (s.bars ? `${s.bar}/${s.bars}` : clock(secs)) : "";
      const v = this.set[K.SOURCE], sg = SRC[v] || ["?", "?"];
      sign.textContent = sg[0]; sign.style.setProperty("--c", v >= 1 && v <= 3 ? this.store.colorOf(v - 1) : v === 5 ? AMBER : v === 4 ? GREEN : "var(--accent)");
      rec.dataset.state = String(stt); rec.classList.toggle("armed", stt === 1); rec.classList.toggle("on", stt === 2 || stt === 3);
      const icon = stt === 2 || stt === 1 ? "stop" : "rec";   // (only when it changes: a click's press and release on one element)
      if (rec.dataset.icon !== icon) { rec.dataset.icon = icon; rec.replaceChildren(svg(SIC[icon], icon === "stop" ? 22 : 26)); }
      rec.setAttribute("aria-label", stt === 1 ? "Disarm" : stt === 2 ? "Stop" : mode === 0 ? "Capture" : mode === 3 ? "Record" : "Arm");
      rec.disabled = stt === 3;
      const d = dB(s.level | 0), thr = this.set[K.THRESHOLD];
      lvI.style.width = `${((d + 60) / 60) * 100}%`;
      lvT.style.left = `${((thr + 60) / 60) * 100}%`;
      lvT.hidden = ((s.sets >>> K.THRESHOLD) & 1) === 0;
      lvText.textContent = `${Math.round(d)} dB`;
    });
    const mon = h("div", { class: "mv-card mv-smp-mon" },
      h("div", { class: "mv-row" }, sign, state, count, h("span", { class: "mv-spacer" }), rec),
      h("div", { class: "mv-smp-wavew" }, this.wave),
      h("div", { class: "mv-row" }, level, lvText));
    return h("div", { class: "mv-smp-rec-l" }, mon, settings);
  }
  // the running wave: a rolling history (the newest at the right), or a take with bars filling its whole length
  paint() {
    if (!this.isOpen || this.tab !== 0 || !this.wave || !this.wave.isConnected || !this.smp) return;
    const c = this.wave, dpr = devicePixelRatio || 1, w = Math.max(10, Math.round(c.clientWidth * dpr)), hh = Math.round((c.clientHeight || 120) * dpr);
    if (c.width !== w) c.width = w;
    if (c.height !== hh) c.height = hh;
    const g = c.getContext("2d"), s = this.smp, pk = hexPeaks(s.peaks), col = s.state === 2 ? RED : s.state === 1 ? AMBER : getComputedStyle(this.el).getPropertyValue("--accent") || "#3cc8dc";
    g.clearRect(0, 0, w, hh);
    const step = 3 * dpr, n = Math.floor(w / step), mid = hh / 2;
    g.fillStyle = "rgba(255,255,255,.08)"; g.fillRect(0, mid - 0.5 * dpr, w, dpr);
    g.fillStyle = col.trim();
    if (s.span) {
      for (let x = 0; x < n; x++) {
        const i = Math.floor(x * s.span / n);
        if (i >= pk.length) break;
        const a = Math.max(dpr, (pk[i] / 255) * (hh / 2 - 2));
        g.fillRect(x * step, mid - a, step - dpr, 2 * a);
      }
      if (s.bars) { g.fillStyle = "rgba(255,255,255,.18)"; for (let b = 1; b < s.bars && s.bars <= 16; b++) g.fillRect(Math.round((b / s.bars) * w), 0, dpr, hh); }
    } else {
      for (let k = 0; k < Math.min(n, pk.length); k++) {
        const v = pk[pk.length - 1 - k], a = Math.max(v ? dpr : 0, (v / 255) * (hh / 2 - 2)), x = w - (k + 1) * step;
        g.fillRect(x, mid - a, step - dpr, 2 * a);
      }
    }
    this.drawHeads();
  }

  // ================================================================ Takes
  async loadList() {
    if (this.listing) { this.listAgain = true; return; }
    this.listing = true;
    const ver = this.smp ? this.smp.takes_ver : -1;
    try {
      const r = await fetch(`api/takes${this.day ? `?day=${this.day}` : ""}`, { cache: "no-store" });
      if (r.ok) {
        const j = await r.json();
        this.list = j; this.listVer = ver;
        if (this.day && j.day !== this.day) this.day = 0;
        if (this.sel && !j.takes.some((t) => t.id === this.sel)) this.sel = 0;
      }
    } catch (_) { /* (the Move away: the next state asks again) */ }
    this.listing = false;
    if (this.listAgain) { this.listAgain = false; this.loadList(); return; }
    this.render();
  }
  take(id) { return (this.list && this.list.takes.find((t) => t.id === id)) || null; }
  pkey(t) { return `${t.id}:${t.frames}:${this.rev.get(t.id) || 0}`; }
  wantPeaks(t) {
    const k = this.pkey(t);
    if (this.peaks.has(k) || this.pq.some((x) => x.k === k)) return;
    this.pq.push({ k, id: t.id });
    this.nextPeaks();
  }
  async nextPeaks() {
    if (this.pbusy || !this.pq.length) return;
    this.pbusy = true;
    const { k, id } = this.pq.shift();
    if (!this.take(id)) { this.pbusy = false; this.nextPeaks(); return; }   // (gone from the list meanwhile)
    try {
      const r = await fetch(`api/takes/peaks?id=${id}&n=64`, { cache: "no-store" });
      if (r.ok) { const j = await r.json(); if (!j.gone) { this.peaks.set(k, Uint8Array.from(j.peaks || [])); this.drawWaves(k); } }
    } catch (_) {}
    this.pbusy = false;
    this.nextPeaks();
  }
  drawWave(c, pk, color) {
    const dpr = devicePixelRatio || 1, w = Math.max(10, Math.round((c.clientWidth || 200) * dpr)), hh = Math.max(10, Math.round((c.clientHeight || 40) * dpr));
    if (c.width !== w) c.width = w;
    if (c.height !== hh) c.height = hh;
    const g = c.getContext("2d"), mid = hh / 2;
    g.clearRect(0, 0, w, hh);
    g.fillStyle = color;
    const cw = w / pk.length;
    for (let i = 0; i < pk.length; i++) { const a = Math.max(dpr * 0.5, (pk[i] / 255) * (mid - dpr)); g.fillRect(i * cw, mid - a, Math.max(dpr, cw - dpr), 2 * a); }
    c.dataset.ok = "1";
  }
  drawWaves(k) {
    for (const c of this.el.querySelectorAll("canvas[data-pk]")) {
      if (k && c.dataset.pk !== k) continue;
      const pk = this.peaks.get(c.dataset.pk);
      if (pk) this.drawWave(c, pk, c.dataset.color || "#3cc8dc");
    }
  }
  srcColor(t) {
    const v = t.src;
    return v >= 1 && v <= 3 ? this.store.colorOf(v - 1) : v === 5 ? AMBER : v === 4 ? GREEN : v === 255 ? "#9696a0" : "var(--accent)";
  }
  resolved(c) { return c.startsWith("var(") ? (getComputedStyle(this.el).getPropertyValue(c.slice(4, -1)).trim() || "#3cc8dc") : c; }
  takes() {
    const l = this.list;
    if (!l) { if (!this.listing) this.loadList(); return h("div", { class: "mv-empty-in" }, h("span", { class: "mv-mut" }, "…")); }
    const undo = h("button", { class: "mv-pill mv-smp-undo", id: "mv-smp-undo", "aria-label": "Undo", title: "Undo (the Move's too)", onclick: () => this.undo() }, svg(SIC.undo, 14), this.ph ? null : "Undo");
    this.up(() => { undo.disabled = !(this.smp && this.smp.undo); });
    const where = h("button", { class: `mv-pill mv-smp-where ${this.onMove ? "on" : ""}`, id: "mv-smp-where", "aria-pressed": this.onMove ? "true" : "false",
      title: "Play on the Move", "aria-label": "Play on the Move",
      onclick: () => { this.stopAll(); this.onMove = !this.onMove; remember("mv-smp-move", this.onMove ? "1" : "0"); this.render(); } }, svg(SIC.move, 14), this.ph ? null : "Move");
    const days = h("div", { class: "mv-pager mv-smp-days", role: "tablist" }, l.days.map((d) => h("button", { class: `mv-pg ${d.id === l.day ? "on" : ""}`, role: "tab",
      "data-day": d.id, "aria-selected": d.id === l.day ? "true" : "false", onclick: () => { this.day = d.id; this.sel = 0; this.loadList(); } },
      dayName(d), h("span", { class: "mv-smp-n" }, d.takes))));
    const head = h("div", { class: "mv-row mv-smp-thead" }, days, h("span", { class: "mv-spacer" }), where, undo);
    if (!l.takes.length) return h("div", { class: "mv-col" }, head, h("div", { class: "mv-empty-in mv-smp-none" }, svg(SIC.rec, 28), h("span", { class: "mv-mut" }, "No takes")));
    return h("div", { class: "mv-col mv-smp-takes" }, head, h("div", { class: "mv-smp-list", role: "list" }, l.takes.map((t) => this.row(t))));
  }
  iconBtn(ic, label, onclick, cls = "") { return h("button", { class: `mv-smp-ib ${cls}`, "aria-label": label, title: label, onclick }, svg(ic, 16)); }
  row(t) {
    const stems = t.kind === "stems", col = this.srcColor(t), sel = t.id === this.sel && !stems;
    const sign = h("span", { class: "mv-smp-sign", style: { "--c": col } }, t.src === 255 ? "?" : (SRC[t.src] || ["?"])[0]);
    const k = this.pkey(t);
    const cv = h("canvas", { class: "mv-smp-tw", "data-pk": k, "data-color": this.resolved(col), height: 40 });
    const head = h("i", { class: "mv-smp-ph", "data-take": t.id });
    const play = this.iconBtn(SIC.play, "Play", () => this.play(t), "mv-smp-play");
    play.dataset.take = t.id;
    const acts = h("span", { class: "mv-smp-acts" },
      this.iconBtn(SIC.down, "Download", () => download(`api/takes/wav?id=${t.id}&dl=1`), "mv-smp-dl"),
      stems ? null : this.iconBtn(SIC.pen, "Rename", () => this.rename(t), "mv-smp-ren"),
      stems ? null : this.iconBtn(SIC.tools, "Tools", () => { this.sel = sel ? 0 : t.id; if (!sel) this.trim = { id: t.id, a: 0, b: t.frames }; this.render(); }, `mv-smp-tools ${sel ? "on" : ""}`),
      this.iconBtn(IC.trash, "Delete", () => this.edit(t, "delete"), "mv-smp-del"));
    const r = h("div", { class: `mv-smp-take ${sel ? "sel" : ""}`, role: "listitem", "data-take": t.id, "data-kind": t.kind, style: { "--c": col } },
      play, sign,
      h("span", { class: "mv-smp-tn" }, h("b", { class: "mv-ell" }, label(t.name) || t.name),
        h("span", { class: "mv-val mv-mut" }, [hhmm(t.name), lenText(t), t.bpm ? `${Math.round(t.bpm * 10) / 10} BPM` : ""].filter(Boolean).join(" · "))),
      h("span", { class: "mv-smp-tww" }, cv, head), acts);
    this.wantPeaks(t);
    requestAnimationFrame(() => { const pk = this.peaks.get(k); if (pk && cv.isConnected) this.drawWave(cv, pk, cv.dataset.color); });
    return sel ? [r, this.tools(t)] : r;
  }
  // Tools on the chosen take: its wave with Trim's two markers, the edits, the fades' length
  tools(t) {
    const k = this.pkey(t), col = this.resolved(this.srcColor(t));
    if (this.trim.id !== t.id || this.trim.b > t.frames) this.trim = { id: t.id, a: 0, b: t.frames };
    const cv = h("canvas", { class: "mv-smp-big", "data-pk": k, "data-color": col, height: 120 });
    const shadeA = h("i", { class: "mv-smp-shade a" }), shadeB = h("i", { class: "mv-smp-shade b" });
    const mA = h("i", { class: "mv-smp-mk a", role: "slider", tabindex: 0, "aria-label": "Trim start" }), mB = h("i", { class: "mv-smp-mk b", role: "slider", tabindex: 0, "aria-label": "Trim end" });
    const head = h("i", { class: "mv-smp-ph", "data-take": t.id });
    const box = h("div", { class: "mv-smp-bigw", id: "mv-smp-trim" }, cv, shadeA, shadeB, mA, mB, head);
    const info = h("span", { class: "mv-val mv-mut mv-smp-trimt" });
    const rate = t.rate || 44100, min = Math.min(t.frames, Math.round(rate * 0.01));
    const place = () => {
      const a = this.trim.a / t.frames, b = this.trim.b / t.frames;
      shadeA.style.width = `${a * 100}%`; shadeB.style.left = `${b * 100}%`;
      mA.style.left = `${a * 100}%`; mB.style.left = `${b * 100}%`;
      mA.setAttribute("aria-valuenow", this.trim.a); mB.setAttribute("aria-valuenow", this.trim.b);
      info.textContent = `${(this.trim.a / rate).toFixed(3)} – ${(this.trim.b / rate).toFixed(3)} s · ${((this.trim.b - this.trim.a) / rate).toFixed(3)} s`;
      trimBtn.disabled = this.trim.a === 0 && this.trim.b === t.frames;
    };
    const at = (x) => { const r = box.getBoundingClientRect(); return Math.round(Math.max(0, Math.min(1, (x - r.left) / r.width)) * t.frames); };
    box.addEventListener("pointerdown", (e) => {
      if (e.button > 0) return;
      e.preventDefault();
      box.setPointerCapture(e.pointerId);
      const f = at(e.clientX), which = Math.abs(f - this.trim.a) <= Math.abs(f - this.trim.b) ? "a" : "b";
      const move = (ev) => {
        const g = at(ev.clientX);
        if (which === "a") this.trim.a = Math.max(0, Math.min(g, this.trim.b - min)); else this.trim.b = Math.min(t.frames, Math.max(g, this.trim.a + min));
        place();
      };
      move(e);
      const upFn = () => { box.removeEventListener("pointermove", move); box.removeEventListener("pointerup", upFn); box.removeEventListener("pointercancel", upFn); };
      box.addEventListener("pointermove", move); box.addEventListener("pointerup", upFn); box.addEventListener("pointercancel", upFn);
    });
    for (const [m, which] of [[mA, "a"], [mB, "b"]]) m.addEventListener("keydown", (e) => {
      const d = { ArrowLeft: -1, ArrowRight: 1 }[e.key];
      if (!d) return;
      e.preventDefault(); e.stopPropagation();
      const step = Math.round(rate * (e.shiftKey ? 0.001 : 0.01)) * d;
      if (which === "a") this.trim.a = Math.max(0, Math.min(this.trim.a + step, this.trim.b - min)); else this.trim.b = Math.min(t.frames, Math.max(this.trim.b + step, this.trim.a + min));
      place();
    });
    const btn = ([op, name, ic]) => h("button", { class: `mv-pill mv-smp-op`, "data-op": op, "aria-label": name, title: name,
      onclick: () => this.edit(t, op) }, svg(ic, 14), h("span", { class: "mv-smp-opt" }, name));
    const trimBtn = btn(OPS[0]);
    const fadeLen = choice(this, { key: "fade", label: "Fade length", options: FADES.map((ms) => (ms < 1000 ? `${ms} ms` : `${ms / 1000} s`)), get: () => this.fade, set: (i) => { this.fade = i; } });
    place();
    requestAnimationFrame(() => { const pk = this.peaks.get(k); if (pk && cv.isConnected) this.drawWave(cv, pk, col); });
    return h("div", { class: "mv-smp-tools-p", "data-take": t.id }, box,
      h("div", { class: "mv-row mv-smp-ops" }, trimBtn, info, h("span", { class: "mv-spacer" })),
      h("div", { class: "mv-row mv-smp-ops" }, OPS.slice(1).map(btn), fadeLen));
  }

  // ================================================================ Kit (rfm1cs stage 3c)
  pad(i) { return (this.kit && this.kit.pads && this.kit.pads[i]) || { n: "", has: 0, v: [0, 0, KFRAC, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0] }; }
  kv(p) { const d = this.pad(this.kit ? this.kit.cur : 0); return d.v ? d.v[p] | 0 : 0; }
  async kpost(body) {
    try {
      const r = await fetch("api/kit", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
      return r.ok;
    } catch (_) { return false; }
  }
  kset(p, v) { return this.kpost({ op: "set", pad: this.kit ? this.kit.cur : 0, param: p, value: Math.round(v) }); }
  async loadKits() {
    const k = this.kit || {};
    this.kitsVer = (k.saved | 0) + (k.kits | 0) * 1000;
    try {
      const r = await fetch("api/kit/kits", { cache: "no-store" });
      if (r.ok) { const j = await r.json(); this.kits = j.kits || []; }
    } catch (_) {}
    this.render();
  }
  kitWave(c, hex, color, region) {
    const pk = hexPeaks(hex);
    if (!pk.length) { const g = c.getContext("2d"); g.clearRect(0, 0, c.width, c.height); return; }
    this.drawWave(c, pk, color);
    if (region) {   // (outside the region: shaded)
      const g = c.getContext("2d"), w = c.width, hh = c.height, a = (region[0] / KFRAC) * w, b = (region[1] / KFRAC) * w;
      g.fillStyle = "rgba(10,8,6,.66)"; g.fillRect(0, 0, a, hh); g.fillRect(b, 0, w - b, hh);
    }
  }
  kitTab() {
    const k = this.kit || {};
    if (!this.kitOn || !k.pads) return h("div", { class: "mv-empty-in mv-smp-none" }, h("b", {}, "No kit on this Move"));
    if (!this.kits) this.loadKits();
    // the pads: the Move's left 4 x 4, pad 1 bottom left
    const pads = [];
    for (let r = 3; r >= 0; r--) for (let c = 0; c < 4; c++) pads.push(this.kpad(r * 4 + c));
    const grid = h("div", { class: "mv-kit-pads", role: "group", "aria-label": "The kit's pads" }, pads);
    // the head: the kit's name, Load, Save
    const name = h("b", { class: "mv-kit-name", id: "mv-kit-name" });
    this.up(() => { const kk = this.kit || {}; name.textContent = kk.name || "Kit"; name.classList.toggle("mv-mut", !kk.name); });
    const live = h("span", { class: "mv-live" });
    this.up(() => { live.hidden = !(this.kit && this.kit.shown); live.textContent = "On the Move's screen"; });
    const load = h("button", { class: "mv-pill mv-kit-load", id: "mv-kit-load", "aria-haspopup": "menu", disabled: !(this.kits && this.kits.length),
      onclick: () => openPop(load, () => (this.kits || []).map((x) => popItem({ label: x.name, on: x.name === (this.kit || {}).name, id: `mv-kit-k-${x.id}`,
        onclick: () => { closePop(); this.kpost({ op: "open", id: x.id }); this.store.toast(`${x.name}: loading`); } })), { width: 220, cls: "mv-pop-list" }) },
      svg('<path d="M2 4.5h4l1.5 1.5H14v7H2z"/>', 14), "Load");
    const save = h("button", { class: "mv-pill mv-kit-save", id: "mv-kit-save", onclick: async () => { if (await this.kpost({ op: "save" })) this.store.toast("Saving the kit"); } },
      svg('<path d="M3 2.5h8l2 2v9H3z"/><path d="M5.5 2.5v3h4v-3M5.5 13.5v-4h5v4"/>', 14), "Save");
    const head = h("div", { class: "mv-row mv-kit-head" }, name, live, h("span", { class: "mv-spacer" }), load, save);
    return h("div", { class: "mv-col mv-kit" }, head, h("div", { class: "mv-kit-main" }, grid, this.kpanel()));
  }
  // a pad: its number, its name, its wave; pressed: heard (held while the press lasts) and chosen
  kpad(i) {
    const col = KROW[i >> 2];
    const cv = h("canvas", { class: "mv-kit-pw", height: 36 });
    const nm = h("span", { class: "mv-kit-pn mv-ell" });
    const b = h("button", { class: "mv-kit-pad", "data-pad": i, style: { "--c": col }, "aria-label": `Pad ${i + 1}` },
      h("span", { class: "mv-kit-no" }, String(i + 1)), nm, cv);
    let down = false;
    const off = () => { if (!down) return; down = false; this.kpost({ op: "play", pad: i, vel: 0 }); };
    const press = () => {
      if (this.kit && this.kit.cur !== i) this.kpost({ op: "choose", pad: i });
      if (this.pad(i).has) { down = true; this.kpost({ op: "play", pad: i, vel: 100 }); }
      if (this.kit && this.kit.cur !== i) { this.kit.cur = i; this.render(); }
    };
    b.addEventListener("pointerdown", (e) => {   // (let go anywhere: the window's, the tab may be built again meanwhile)
      if (e.button > 0) return;
      e.preventDefault();
      window.addEventListener("pointerup", off, { once: true }); window.addEventListener("pointercancel", off, { once: true });
      press();
    });
    b.addEventListener("keydown", (e) => { if ((e.key === "Enter" || e.key === " ") && !e.repeat) { e.preventDefault(); press(); } });
    b.addEventListener("keyup", (e) => { if (e.key === "Enter" || e.key === " ") off(); });
    this.up(() => {
      const d = this.pad(i), kk = this.kit || {};
      b.classList.toggle("has", !!d.has); b.classList.toggle("cur", kk.cur === i);
      b.classList.toggle("glow", !!((kk.glow >>> i) & 1)); b.classList.toggle("muted", !!(d.v && d.v[KP.MUTE]));
      nm.textContent = d.has ? d.n || "…" : "";
      b.setAttribute("aria-pressed", kk.cur === i ? "true" : "false");
      if (cv.dataset.pk !== (d.pk || "")) {
        cv.dataset.pk = d.pk || "";
        requestAnimationFrame(() => { if (cv.isConnected) this.kitWave(cv, d.pk || "", "rgba(255,255,255,.85)"); });
      }
    });
    return b;
  }
  // the chosen pad: its wave and region, its settings (as the Move's knobs and rows set them)
  kpanel() {
    const cur = this.kit ? this.kit.cur : 0, col = KROW[cur >> 2];
    const d = this.pad(cur), secs = (v) => (d.f && d.r ? `${((d.f * v) / KFRAC / d.r).toFixed(3)} s` : `${Math.round((v / KFRAC) * 1000) / 10}%`);
    const cv = h("canvas", { class: "mv-kit-big", id: "mv-kit-wave", height: 110 });
    this.up(() => {
      const dd = this.pad(cur), key = `${dd.pk}|${dd.v ? dd.v[KP.START] : 0}|${dd.v ? dd.v[KP.END] : 0}`;
      if (cv.dataset.k === key) return;
      cv.dataset.k = key;
      requestAnimationFrame(() => { if (cv.isConnected) this.kitWave(cv, dd.pk || "", col, dd.v ? [dd.v[KP.START], dd.v[KP.END]] : null); });
    });
    const row = (name, ctl, p) => h("div", { class: "mv-kit-set", "data-set": p }, h("span", { class: "mv-h" }, name), ctl);
    const B = (p, label, o) => bar(this, Object.assign({ key: `k${p}`, label, cls: "thin", color: () => col, get: () => this.kv(p), set: (v) => this.kset(p, v) }, o));
    const env = (p, label) => bar(this, { key: `k${p}`, label, cls: "thin", min: 0, max: ENV.length - 1, color: () => col,
      get: () => envIdx(this.kv(p)), set: (i) => this.kset(p, ENV[i]), fmt: (i) => msText(ENV[i]) });
    const S = (p, label, options) => segs(this, { key: `k${p}`, label, options, get: () => this.kv(p), set: (v) => this.kset(p, v) });
    const sets = h("div", { class: "mv-kit-sets" },
      row("Pitch", bar(this, { key: "k0", label: "Pitch", cls: "thin", min: -24, max: 24, color: () => col, get: () => Math.round(this.kv(KP.PITCH) / 100),
        set: (v) => this.kset(KP.PITCH, v * 100), fmt: (v) => (this.kv(KP.STRETCH) ? "Stretch" : `${v > 0 ? "+" : ""}${v} st`) }), KP.PITCH),
      row("Start", B(KP.START, "Start", { min: 0, max: KFRAC, fmt: secs }), KP.START),
      row("End", B(KP.END, "End", { min: 0, max: KFRAC, fmt: secs }), KP.END),
      row("Play", S(KP.MODE, "Play", ["One shot", "Momentary", "Loop", "Toggle"]), KP.MODE),
      row("Attack", env(KP.ATTACK, "Attack"), KP.ATTACK),
      row("Hold", env(KP.HOLD, "Hold"), KP.HOLD),
      row("Release", env(KP.RELEASE, "Release"), KP.RELEASE),
      row("Choke", S(KP.CHOKE, "Choke", ["Off", "1", "2", "3", "4"]), KP.CHOKE),
      row("Reverse", S(KP.REVERSE, "Reverse", ["Off", "On"]), KP.REVERSE),
      row("Level", B(KP.LEVEL, "Level", { min: -60, max: 6, fmt: (v) => (v <= -60 ? "-inf" : `${v > 0 ? "+" : ""}${v} dB`) }), KP.LEVEL),
      row("Pan", B(KP.PAN, "Pan", { min: -50, max: 50, fmt: (v) => (v ? `${v < 0 ? "L" : "R"}${Math.abs(v)}` : "C") }), KP.PAN),
      row("Mute", S(KP.MUTE, "Mute", ["Off", "On"]), KP.MUTE),
      row("Stretch", B(KP.STRETCH, "Stretch", { min: 0, max: 64, fmt: (v) => (v ? `${v} step${v === 1 ? "" : "s"}` : "Off") }), KP.STRETCH),
      [1, 2, 3].map((n) => row(`Send ${n}`, B(KP.SEND1 + n - 1, `Send ${n}`, { min: 0, max: 100, fmt: (v) => `${v} %` }), KP.SEND1 + n - 1)));
    const title = h("div", { class: "mv-row mv-kit-ptitle" }, h("span", { class: "mv-kit-big-no", style: { "--c": col } }, String(cur + 1)),
      h("b", { class: "mv-ell", id: "mv-kit-pname" }, d.has ? d.n || "…" : "Empty"),
      h("span", { class: "mv-val mv-mut" }, d.has && d.f && d.r ? `${(d.f / d.r).toFixed(2)} s` : ""));
    return h("div", { class: "mv-card mv-kit-panel", id: "mv-kit-panel", style: { "--c": col } }, title, h("div", { class: "mv-kit-bigw" }, cv), sets);
  }

  // ---- the take's edits, through the library (the Move's list and Tools follow; one undo, the Move's)
  async post(what, body) {
    try {
      const r = await fetch(`api/takes/${what}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
      const j = await r.json().catch(() => ({}));
      return r.ok ? j : { ok: false, error: j.error || `${r.status}` };
    } catch (_) { return { ok: false, error: "the Move did not answer" }; }
  }
  bump(id) { this.rev.set(id, (this.rev.get(id) || 0) + 1); }
  async edit(t, op) {
    const body = { id: t.id, op };
    if (op === "trim") { body.from = this.trim.a; body.count = this.trim.b - this.trim.a; }
    if (op === "fade-in" || op === "fade-out") body.ms = FADES[this.fade];
    if (op === "delete" || this.playing === t.id) this.stopAll();
    const j = await this.post("edit", body);
    if (!j.ok) { this.store.toast(`${label(t.name)}: ${j.error || "not done"}`, "bad"); return; }
    this.bump(t.id);
    if (op === "trim" && j.take) this.trim = { id: t.id, a: 0, b: j.take.frames };
    if (op === "delete" && this.sel === t.id) this.sel = 0;
    this.loadList();
  }
  async undo() {
    this.stopAll();
    const j = await this.post("undo");
    if (!j.ok) { this.store.toast(`Undo: ${j.error || "not done"}`, "bad"); return; }
    if (j.take) { this.bump(j.take.id); if (this.trim.id === j.take.id) this.trim = { id: j.take.id, a: 0, b: j.take.frames }; }
    this.loadList();
  }
  async rename(t) {
    const raw = prompt(`Rename ${label(t.name)} to:`, label(t.name));
    if (raw == null) return;
    const name = raw.trim();
    if (!name || name === label(t.name)) return;
    const j = await this.post("rename", { id: t.id, name });
    if (!j.ok) { this.store.toast(`${name}: ${j.error || "not renamed"}`, "bad"); return; }
    if (j.take && this.sel === t.id) { this.sel = j.take.id; this.trim = { id: j.take.id, a: 0, b: j.take.frames }; }
    this.loadList();
  }

  // ---- playback: in the browser (the take's WAV, its position from the <audio>), or on the Move (its position from
  // the state); one at a time
  play(t) {
    const movePlaying = this.smp && this.smp.play;
    if (this.onMove) {
      const on = movePlaying === t.id;
      this.post("play", { id: on ? 0 : t.id });
      return;
    }
    if (this.playing === t.id) { this.audio.pause(); this.playing = 0; this.refreshPlay(); return; }
    this.playing = t.id;
    this.audio.src = `api/takes/wav?id=${t.id}&v=${this.rev.get(t.id) || 0}`;
    this.audio.currentTime = 0;
    const p = this.audio.play();
    if (p && p.catch) p.catch(() => { if (this.playing === t.id) { this.playing = 0; this.refreshPlay(); } });
    this.refreshPlay();
    this.tick();
  }
  stopAll() {
    if (this.playing) { this.audio.pause(); this.playing = 0; }
    if (this.smp && this.smp.play) this.post("play", { id: 0 });
    this.refreshPlay();
  }
  refreshPlay() {
    const moveId = this.onMove && this.smp ? this.smp.play : 0, cur = this.onMove ? moveId : this.playing;
    for (const b of this.el.querySelectorAll(".mv-smp-play")) {
      const on = +b.dataset.take === cur && !!cur;
      if (b.classList.contains("on") !== on) { b.classList.toggle("on", on); b.replaceChildren(svg(on ? SIC.stop : SIC.play, 16)); b.setAttribute("aria-label", on ? "Stop" : "Play"); }
    }
  }
  tick() {
    if (this.raf) return;
    const go = () => { this.raf = 0; this.drawHeads(); if (this.playing && !this.audio.paused) this.raf = requestAnimationFrame(go); };
    this.raf = requestAnimationFrame(go);
  }
  // where the take playing is, over its waves
  drawHeads() {
    if (!this.isOpen || this.tab !== 1) return;
    let id = 0, f = 0;
    if (this.onMove && this.smp && this.smp.play) { const t = this.take(this.smp.play); id = this.smp.play; f = t && t.frames ? this.smp.pos / t.frames : 0; }
    else if (this.playing && this.audio.duration > 0) { id = this.playing; f = this.audio.currentTime / this.audio.duration; }
    for (const e of this.el.querySelectorAll(".mv-smp-ph")) {
      const on = +e.dataset.take === id && id !== 0;
      e.hidden = !on;
      if (on) e.style.left = `${Math.max(0, Math.min(1, f)) * 100}%`;
    }
    this.refreshPlay();
  }
}

export const SMP_IC = SIC.rec;
