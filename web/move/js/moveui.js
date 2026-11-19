// SPDX-License-Identifier: GPL-3.0-only
// The Move's firmware slots and Mix & FX on RemoteFM1's page (RemoteFM14Move): what js/moveslots.js (the header's slot
// chips, the phone's slot button, the keys) and js/movemix.js (the Mix & FX view) share:
//   - the store: the host's mix JSON (GET /api/mix, /ws {"type":"mix"}), the meters, the installed FX modules
//     (GET /api/fx), the Move's firmwares (the page's catalog), and the edits (movehost.js edit: engine.h ENG_OP_*).
//     The core's state is the truth: what the page shows is what comes back; a control being dragged shows its own
//     value until a moment after it is let go, then the Move's again.
//   - the slots' live mini screens (48 x 48 RGB565, /ws type 4) drawn into any number of small canvases;
//   - h() (the DOM), the popover menu, the value formats, and the controls: dials, faders, bar sliders, segments,
//     meters, all on pointer events (mouse and touch) and the arrow keys.
// Nothing here runs unless js/host.js ON_MOVE.

export const NSLOT = 3;
// engine.h ENG_OP_*: FXPAR a parameter 0..127 (b kind<<9 | slot<<7 | value), FXPG a slot's page (rfm1cs 0.5.0)
export const OP = { ACTIVE: 16, UNLOAD: 17, MIX: 18, ROUTE: 19, FXLOAD: 20, FXPARAM: 21, PAGE: 22, FXPAR: 23, FXPG: 24 };
// the Move's FX setup (rfm1cs 0.5.0: one layer): its columns (settings.fx_page 0..2) and the Mixer (3)
export const COLS = ["MIDI FX", "Firmwares", "Audio FX"];
export const MIXP = ["Level", "Pan", "Send 1", "Send 2", "Send 3"];                                     // RFM_MOVE_MIXP_*
export const UNSET = 255;                                                                               // RFM_MOVE_FX_UNSET
export const GREY = "#9696a0";
// rfm1cs fw.c CATCOL over PAL: a category's pad colour on the Move (a firmware not loaded yet: its dot in the menus)
const CAT_COLOR = { Synth: "#14dcff", Groovebox: "#ff7800", Chords: "#aa3cff", Drums: "#ff2424", Other: "#00ffaa" };
// the three FX send buses' colours (the mockups'), and the MIDI FX chain's
export const FX_COLOR = ["#3BD47A", "#F5A524", "#3CC8DC"];
export const MFX_COLOR = "#EE7A3C";

// ---- the DOM
export function h(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === "class") e.className = v;
    else if (k === "style" && typeof v === "object") { for (const [p, x] of Object.entries(v)) if (x != null) e.style.setProperty(p, String(x)); }
    else if (k === "html") e.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function") e.addEventListener(k.slice(2), v);
    else e.setAttribute(k, v === true ? "" : String(v));
  }
  for (const k of kids.flat(Infinity)) if (k != null && k !== false) e.append(k instanceof Node ? k : document.createTextNode(String(k)));
  return e;
}
export const svg = (d, w = 12) => h("span", { class: "mv-ic", "aria-hidden": "true",
  html: `<svg width="${w}" height="${w}" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">${d}</svg>` });
export const IC = {
  chev: '<path d="M4 6l4 4 4-4"/>', plus: '<path d="M8 3v10M3 8h10"/>', close: '<path d="M4 4l8 8M12 4l-8 8"/>',
  back: '<path d="M10 3L5 8l5 5"/>', mix: '<path d="M3 2v12M8 2v12M13 2v12"/><rect x="1.5" y="9" width="3" height="2.5" rx="1" fill="currentColor"/><rect x="6.5" y="4" width="3" height="2.5" rx="1" fill="currentColor"/><rect x="11.5" y="7" width="3" height="2.5" rx="1" fill="currentColor"/>',
  arrow: '<path d="M2 8h11M9 4l4 4-4 4"/>', down: '<path d="M8 2v11M4 9l4 4 4-4"/>', trash: '<path d="M3 4.5h10M6 4.5V3h4v1.5M4.5 4.5l.7 8.5h5.6l.7-8.5"/>',
  eject: '<path d="M3 10h10L8 4z"/><path d="M3 13h10"/>',
};

// ---- formats
export const pct = (v, max = 127) => `${Math.round(Math.max(0, Math.min(1, v / max)) * 1000) / 10}%`;
export const panText = (v) => (v === 64 ? "C" : v < 64 ? `L${64 - v}` : `R${v - 64}`);
// a 16-track filter, said short: "All 16", "None", "Track 6", "1-8", "1, 3, 5", "9 tracks"
export function tracksText(mask) {
  mask &= 0xffff;
  if (mask === 0xffff) return "All 16";
  if (!mask) return "None";
  const on = []; for (let i = 0; i < 16; i++) if (mask >> i & 1) on.push(i + 1);
  if (on.length === 1) return `Track ${on[0]}`;
  if (on[on.length - 1] - on[0] === on.length - 1) return `${on[0]}-${on[on.length - 1]}`;
  return on.length <= 3 ? on.join(", ") : `${on.length} tracks`;
}
// a MIDI FX's filter: one track or all
export const mfxText = (mask) => ((mask & 0xffff) === 0xffff || !mask ? "All" : tracksText(mask));
// a parameter's wire value (0..127, an enum's option index) as the module means it: as the Move reads it (the host's
// lo / hi / dec / unit: "38%", "2.40 s") when the plan has it
export function paramText(p, v) {
  if (!p) return String(v);
  if (p.type === "enum") return (p.options && p.options[v]) != null ? String(p.options[v]).replace(/_/g, "-") : String(v);
  if (p.lo != null && p.hi != null && p.lo !== p.hi) {
    const y = p.lo + (p.hi - p.lo) * v / 127, u = p.unit || "";
    return `${y.toFixed(p.dec | 0)}${u === "%" || !u ? u : ` ${u}`}`;
  }
  const x = p.min + (p.max - p.min) * v / 127;
  if (p.type === "int") return String(Math.round(x));
  return Math.abs(p.max - p.min) <= 10 ? x.toFixed(2) : x.toFixed(1);
}
export const wetHint = (p) => (p && p.wet === 1 ? "100 % wet for a send" : p && p.wet === 2 ? "0 for a send" : "");
// a module's pages (/api/fx: the host's plan), else pages of 8 in the params' order
export function modPages(m) {
  if (!m) return [];
  if (m.pages && m.pages.length) return m.pages;
  const n = (m.params || []).length, out = [];
  for (let i = 0; i < n; i += 8) out.push({ name: out.length ? `${m.name} ${out.length + 1}` : m.name, knobs: Array.from({ length: 8 }, (_, k) => (i + k < n ? i + k : -1)) });
  return out;
}
// a module's name, short (the phone's small send sliders)
export const shortName = (m) => (!m ? "" : m.name.length <= 6 ? m.name : m.abbrev || m.name.slice(0, 4));

// ---- the store
export class MoveStore {
  constructor({ player, catalog, toast }) {
    this.player = player; this.catalog = catalog; this.toast = toast || (() => {});
    this.mix = null; this.meters = new Uint8Array(7); this.lib = { midi: [], audio: [] };
    this.subs = new Set();
    this.pending = [null, null, null];   // a load on its way into a slot: { id, t }
    player.listen({
      mix: (m) => { this.mix = m; this.checkPending(); this.fire("mix"); },
      meters: (m) => { this.meters = m; this.fire("meters"); },
    });
    this.loadLib();
    if (!player.mix) fetch("api/mix", { cache: "no-cache" }).then((r) => (r.ok ? r.json() : null)).then((m) => { if (m && !this.mix) { this.mix = m; this.fire("mix"); } }).catch(() => {});
  }
  on(fn) { this.subs.add(fn); return () => this.subs.delete(fn); }
  fire(what) { for (const f of this.subs) { try { f(what); } catch (e) { console.warn("moveui:", e); } } }
  async loadLib() {
    try {
      const r = await fetch("api/fx", { cache: "no-cache" });
      if (r.ok) { const j = await r.json(); this.lib = { midi: j.midi || [], audio: j.audio || [] }; this.fire("lib"); }
    } catch (_) { /* (no FX modules: the pickers say so) */ }
  }
  edit(op, a, b) { this.player.edit(op, a, b); }

  // the slots: { on, id, name, version, color, level, pan, sends, filt, ... } (the mix's; empty ones { on: 0 })
  get slots() { return (this.mix && this.mix.slots) || [{ on: 0 }, { on: 0 }, { on: 0 }]; }
  get active() { return this.mix ? this.mix.active | 0 : 0; }
  slot(i) { return this.slots[i] || { on: 0 }; }
  colorOf(i) { const s = this.slot(i); return s.on && s.color ? s.color : GREY; }
  firmwares() { return (this.catalog && this.catalog.firmwares) || []; }
  fwColor(f) {
    const k = this.slots.findIndex((s) => s.on && s.id === f.id);
    if (k >= 0) return this.colorOf(k);
    return CAT_COLOR[(f.categories && f.categories.find((c) => CAT_COLOR[c])) || "Other"] || GREY;
  }
  where(f) { const k = this.slots.findIndex((s) => s.on && s.id === f.id); return k >= 0 ? k : -1; }
  fx(kind, i) { const l = this.mix && (kind ? this.mix.afx : this.mix.mfx); return (l && l[i]) || { mod: -1, vals: [], filt: 0xffff }; }
  module(kind, i) { const x = this.fx(kind, i), l = kind ? this.lib.audio : this.lib.midi; return x.mod >= 0 ? l.find((m) => m.i === x.mod) || l[x.mod] || null : null; }
  modName(kind, i) { const m = this.module(kind, i), x = this.fx(kind, i); return m ? m.name : x.mod >= 0 ? x.name || "Module" : ""; }

  // a firmware into a slot (POST /api/load; activate: then the playing one), followed until it runs there
  async load(slot, f, activate = true) {
    this.pending[slot] = { id: f.id, name: f.name, t: Date.now() };
    this.fire("mix");
    let ok = false;
    try {
      const r = await fetch("api/load", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ slot, id: f.id, activate: activate ? 1 : 0 }) });
      ok = r.ok;
    } catch (_) { ok = false; }
    if (!ok) { this.pending[slot] = null; this.fire("mix"); this.toast(`The Move could not load ${f.name} into slot ${slot + 1}.`, "bad"); return; }
    setTimeout(() => { const p = this.pending[slot]; if (p && p.id === f.id) { this.pending[slot] = null; this.fire("mix"); this.toast(`${f.name} did not start in slot ${slot + 1}.`, "bad"); } }, 20000);
  }
  checkPending() { this.pending.forEach((p, s) => { if (p && this.slot(s).on && this.slot(s).id === p.id) this.pending[s] = null; }); }
  unload(slot) { this.edit(OP.UNLOAD, slot, 0); }
  activate(slot) { if (this.slot(slot).on) this.edit(OP.ACTIVE, slot, 0); }
}

// ---- the slots' mini screens: 48 x 48, into every canvas made for that slot
export class Minis {
  constructor(player) {
    this.img = [null, null, null]; this.cv = [new Set(), new Set(), new Set()];
    player.listen({ mini: (s, px) => this.put(s, px) });
  }
  canvas(slot, cls = "") {
    const c = h("canvas", { class: `mv-mini ${cls}`, width: 48, height: 48, "aria-hidden": "true" });
    this.cv[slot].add(c);
    if (this.img[slot]) c.getContext("2d").putImageData(this.img[slot], 0, 0);
    return c;
  }
  put(s, px) {
    const img = this.img[s] || (this.img[s] = new ImageData(48, 48)), d = img.data;
    for (let i = 0; i < px.length; i++) {
      const v = px[i], r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
      d[4 * i] = r << 3 | r >> 2; d[4 * i + 1] = g << 2 | g >> 4; d[4 * i + 2] = b << 3 | b >> 2; d[4 * i + 3] = 255;
    }
    for (const c of this.cv[s]) { if (!c.isConnected) { this.cv[s].delete(c); continue; } c.getContext("2d").putImageData(img, 0, 0); }
  }
  clear(s) { this.img[s] = null; for (const c of this.cv[s]) c.getContext("2d").clearRect(0, 0, 48, 48); }
}

// ---- the popover menu (one at a time): under an anchor, closed by a click elsewhere, Escape or a pick
let pop = null;
export function closePop() { if (!pop) return false; const p = pop; pop = null; p.el.remove(); p.anchor && p.anchor.setAttribute("aria-expanded", "false"); if (p.onClose) p.onClose(); return true; }
export const popOpen = () => !!pop;
export function openPop(anchor, build, { width = 280, onClose, cls = "" } = {}) {
  const again = pop && pop.anchor === anchor;
  closePop();
  if (again) return null;
  if (!popWired) {
    popWired = true;
    addEventListener("pointerdown", (e) => { if (pop && !pop.el.contains(e.target) && !pop.anchor.contains(e.target)) closePop(); }, true);
    addEventListener("resize", () => closePop());
  }
  const el = h("div", { class: `mv-pop ${cls}`, role: "menu", style: { width: `${width}px` } });
  document.body.append(el);
  pop = { el, anchor, onClose };
  anchor.setAttribute("aria-expanded", "true");
  const fill = () => { el.replaceChildren(...[].concat(build(fill))); place(); };
  const place = () => {
    const r = anchor.getBoundingClientRect(), W = innerWidth, H = innerHeight, w = Math.min(width, W - 16);
    el.style.width = `${w}px`;
    el.style.left = `${Math.max(8, Math.min(W - w - 8, r.left))}px`;
    const below = r.bottom + 8, hh = el.offsetHeight;
    el.style.top = `${below + hh > H - 8 && r.top - hh - 8 > 8 ? r.top - hh - 8 : below}px`;
    el.style.maxHeight = `${H - 16}px`;
  };
  fill();
  return { el, refill: fill };
}
let popWired = false;   // (the listeners that close it: added with the first menu)
export const popItem = ({ label, sub, dot, thumb, on, onclick, disabled, cls = "", id }) =>
  h("button", { class: `mv-pop-item ${on ? "on" : ""} ${cls}`, role: "menuitem", disabled, onclick, "data-id": id },
    thumb || (dot ? h("span", { class: "mv-dot", style: { "--c": dot } }) : null), h("span", { class: "mv-pop-l" }, label), sub ? h("span", { class: "mv-val mv-mut" }, sub) : null);

// ---- the controls. ctx: { up(fn) } (re-run on every mix); o: { get() -> 0..127, set(v), max?, min?, color?, fmt?(v) }
// A control's own value while it is dragged (and 600 ms after), else the Move's.
function holder(o) {
  const st = { local: 0, until: 0, drag: false, last: 0, timer: 0, sent: -1 };
  const min = o.min ?? 0, max = o.max ?? 127;
  const clamp = (v) => Math.max(min, Math.min(max, Math.round(v)));
  st.value = () => (st.drag || Date.now() < st.until ? st.local : o.get());
  st.put = (v, now) => {
    v = clamp(v); st.local = v; st.until = Date.now() + 600;
    if (v === st.sent && !now) return;
    const go = () => { st.timer = 0; st.last = Date.now(); if (st.local !== st.sent) { st.sent = st.local; o.set(st.local); } };
    clearTimeout(st.timer);
    if (now || Date.now() - st.last > 40) go(); else st.timer = setTimeout(go, 40 - (Date.now() - st.last));
  };
  st.release = (refresh) => { st.drag = false; st.until = Date.now() + 600; setTimeout(refresh, 650); };
  return st;
}
// the drag: vertical (dial: relative), or along the track (fader / bar: absolute)
function dragger(el, st, o, { axis, absolute, track, refresh }) {
  const min = o.min ?? 0, max = o.max ?? 127;
  el.addEventListener("pointerdown", (e) => {
    if (e.button > 0 || el.classList.contains("off")) return;
    e.preventDefault(); e.stopPropagation();
    el.setPointerCapture(e.pointerId);
    el.dataset.ptr = "1";                       // (focused by the pointer: no focus ring; the arrow keys still work)
    el.focus({ preventScroll: true });
    st.local = st.value(); st.drag = true; st.sent = st.local;
    const v0 = st.local, x0 = e.clientX, y0 = e.clientY;
    const at = (ev) => {
      if (absolute) {
        const r = (track || el).getBoundingClientRect(), pad = o.pad || 0;
        const f = axis === "x" ? (ev.clientX - r.left - pad) / (r.width - 2 * pad) : 1 - (ev.clientY - r.top - pad) / (r.height - 2 * pad);
        return min + Math.max(0, Math.min(1, f)) * (max - min);
      }
      const d = axis === "x" ? ev.clientX - x0 : y0 - ev.clientY;
      return v0 + d * (max - min) / (ev.shiftKey ? 600 : 150);
    };
    if (absolute) { st.put(at(e)); refresh(); }
    const move = (ev) => { st.put(at(ev)); refresh(); };
    const up = () => { el.removeEventListener("pointermove", move); el.removeEventListener("pointerup", up); el.removeEventListener("pointercancel", up); st.put(st.local, true); st.release(refresh); el.classList.remove("drag"); };
    el.classList.add("drag");
    el.addEventListener("pointermove", move); el.addEventListener("pointerup", up); el.addEventListener("pointercancel", up);
  });
  el.addEventListener("keydown", (e) => {
    const step = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1, PageUp: 10, PageDown: -10 }[e.key];
    if (!step && e.key !== "Home" && e.key !== "End") return;
    delete el.dataset.ptr;
    e.preventDefault(); e.stopPropagation();
    const v = e.key === "Home" ? min : e.key === "End" ? max : st.value() + step * (e.shiftKey ? 10 : 1);
    st.put(v, true); st.release(refresh); refresh();
  });
  el.addEventListener("wheel", (e) => {
    if (el.classList.contains("off")) return;
    e.preventDefault();
    st.put(st.value() + (e.deltaY < 0 ? 1 : -1) * (Math.abs(e.deltaY) > 40 ? 4 : 1)); st.release(refresh); refresh();
  }, { passive: false });
  if (o.def != null) el.addEventListener("dblclick", () => { st.put(o.def, true); st.release(refresh); refresh(); });
}
const aria = (el, o, v, text) => { el.setAttribute("aria-valuenow", v); el.setAttribute("aria-valuetext", text); };

// a dial: a ring in the colour, its value under it
export function dial(ctx, o) {
  const st = holder(o), max = o.max ?? 127, min = o.min ?? 0;
  const ring = h("span", { class: "mv-dk" });
  const val = h("span", { class: "mv-val" });
  const el = h("div", { class: `mv-dial ${o.cls || ""}`, role: "slider", tabindex: 0, "aria-label": o.label, "aria-valuemin": min, "aria-valuemax": max, "data-ctl": o.key || null },
    ring, o.caption ? h("b", { class: "mv-cap" }, o.caption) : null, val, o.under || null);
  const refresh = () => {
    const v = st.value(), off = !!(o.off && o.off());
    el.classList.toggle("off", off);
    el.style.setProperty("--c", o.color ? o.color() : "var(--accent)");
    el.style.setProperty("--p", `${((v - min) / (max - min)) * 75}%`);
    const t = off ? "–" : o.fmt ? o.fmt(v) : String(v);
    val.textContent = t; aria(el, o, v, t); el.dataset.v = v;
  };
  dragger(el, st, o, { axis: "y", refresh });
  ctx.up(refresh); refresh();
  return el;
}
// a vertical fader: no handle, the level is the coloured fill inside its track
export function fader(ctx, o) {
  const st = holder(o), max = o.max ?? 127;
  const fill = h("i");
  const track = h("div", { class: "mv-fader-t" }, fill);
  const val = h("span", { class: "mv-val mv-big" });
  const el = h("div", { class: "mv-fader", role: "slider", tabindex: 0, "aria-label": o.label, "aria-valuemin": 0, "aria-valuemax": max, "aria-orientation": "vertical", "data-ctl": o.key || null }, track, val);
  const refresh = () => {
    const v = st.value();
    el.style.setProperty("--c", o.color ? o.color() : "var(--accent)");
    el.style.setProperty("--f", v / max);
    val.textContent = o.fmt ? o.fmt(v) : String(v); aria(el, o, v, val.textContent); el.dataset.v = v;
  };
  dragger(el, st, { ...o, pad: 4 }, { axis: "y", absolute: true, track, refresh });
  ctx.up(refresh); refresh();
  return el;
}
// a bar slider (the phone's): light text, vertically centred, over a dim fill with a bright edge
export function bar(ctx, o) {
  const st = holder(o), max = o.max ?? 127, min = o.min ?? 0;
  const fill = h("i"), lab = h("span", { class: "mv-bar-l" }, o.caption || ""), val = h("em");
  const el = h("div", { class: `mv-bar ${o.cls || ""}`, role: "slider", tabindex: 0, "aria-label": o.label, "aria-valuemin": min, "aria-valuemax": max, "data-ctl": o.key || null }, fill, lab, val);
  const refresh = () => {
    const v = st.value(), off = !!(o.off && o.off());
    el.classList.toggle("off", off);
    el.style.setProperty("--c", o.color ? o.color() : "var(--accent)");
    el.style.setProperty("--f", (v - min) / (max - min));
    if (o.capText) lab.textContent = o.capText();
    const t = off ? "" : o.fmt ? o.fmt(v) : String(v);
    val.textContent = t; aria(el, o, v, t); el.dataset.v = v;
  };
  dragger(el, st, o, { axis: "x", absolute: true, refresh });
  ctx.up(refresh); refresh();
  return el;
}
// a segmented control (an enum with few options)
export function segs(ctx, o) {
  const el = h("div", { class: "mv-segs", role: "radiogroup", "aria-label": o.label, "data-ctl": o.key || null });
  const btns = o.options.map((t, i) => h("button", { class: "mv-seg", role: "radio", onclick: () => { o.set(i); st.local = i; st.until = Date.now() + 600; refresh(); setTimeout(refresh, 650); } }, String(t).replace(/_/g, "-")));
  const st = { local: 0, until: 0 };
  el.append(...btns);
  const refresh = () => { const v = Date.now() < st.until ? st.local : o.get(); btns.forEach((b, i) => { b.classList.toggle("on", i === v); b.setAttribute("aria-checked", i === v ? "true" : "false"); }); el.dataset.v = v; };
  ctx.up(refresh); refresh();
  return el;
}
// an enum with many options: a pill that opens the list
export function choice(ctx, o) {
  const st = { local: 0, until: 0 };
  const txt = h("b");
  const el = h("button", { class: "mv-pill mv-choice", "aria-haspopup": "menu", "aria-label": o.label, "data-ctl": o.key || null }, txt, svg(IC.chev));
  const refresh = () => { const v = Date.now() < st.until ? st.local : o.get(); txt.textContent = String(o.options[v] ?? v).replace(/_/g, "-"); el.dataset.v = v; };
  el.addEventListener("click", () => openPop(el, () => o.options.map((t, i) => popItem({ label: String(t).replace(/_/g, "-"), on: i === o.get(),
    onclick: () => { closePop(); o.set(i); st.local = i; st.until = Date.now() + 600; refresh(); setTimeout(refresh, 650); } })), { width: 200, cls: "mv-pop-list" }));
  ctx.up(refresh); refresh();
  return el;
}
// a meter: 0..255 (square-root scale), vertical
export function meter(ctx, idx, cls = "") {
  const i = h("i");
  const el = h("span", { class: `mv-meter ${cls}`, "aria-hidden": "true", "data-meter": idx }, i);
  ctx.meter(() => { const v = ctx.store.meters[idx] | 0; i.style.setProperty("--m", v / 255); el.dataset.v = v; });
  return el;
}
