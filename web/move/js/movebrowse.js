// SPDX-License-Identifier: GPL-3.0-only
// The Move's firmware browser on RemoteFM1's page (RemoteFM14Move; only when js/host.js ON_MOVE): js/fw-browser.js
// (RemoteFM1's browser: the iOS app's firmware tiles, the same file as remotefm1.com's) over the Move's catalog
// (src/web.c GET /api/catalog: every firmware of the signed index and every installed one, each with its 64 x 64
// 1-bit thumbnail, its categories, the version installed; the Move's own favourites, in order).
//   - opened from a slot's load menu ("Browse all", desktop: under the chip; phone: the slot menu's load page) for that
//     slot; the slot can be changed in its head. A dialog: centred on a desktop, the whole screen on a phone
//   - a tile: RemoteFM1FW's catalog image when the device showing the page reaches GitHub, else the Move's own 1-bit
//     thumbnail (pixelated, the theme's colours), else the name on its colour. Marks: playing (the target slot's),
//     a Fantasy CFW, a favourite (the Move's: set with its Sample button), UPDATE, not installed, a download's ring
//   - the filters (the owner's, as on the Move): Favourites or All (All by itself when there are none), Installed
//     (with its updates counted) / Not installed, one category at a time or Fantasy CFW, a search
//   - a tap: an installed one loads into the slot (and plays it); one not installed, or with an update, is downloaded
//     by the Move (POST /api/download: the Move's own catalog download, its progress the status's dl_*) and then
//     loaded. Esc or the scrim closes it.
//   - the settings sheet's Firmware page is the same browser (sheetPage, from site.patch's sheet.js): the whole
//     catalog for the slot that plays; "Bigger view" opens the dialog
//   An update: installed, and the index has another version or another build of the same version (the host's
//   cat_updates: a rebuild's installed version reads "<version> old").
//   mountMoveBrowse({ store, toast }) -> { open(slot), close(), isOpen, sheetPage(closeSheet), thumb(id, name) }
import { h, svg, IC } from "./moveui.js";
import { moveCatalog } from "./movehost.js";
import { FwBrowser, catalogImages, thumbBits, bitsUrl, standIn } from "./fw-browser.js";

const CATS = ["All", "Synth", "Groovebox", "Chords", "Drums", "Other"];
const DL_MS = 5 * 60 * 1000;

class MoveBrowse {
  constructor({ store, toast }) {
    this.store = store; this.toast = toast || (() => {});
    this.cat = { firmwares: [], favorites: [] };
    this.slot = 0; this.isOpen = false;
    this.dl = null;                     // { id, name, slot, pct }
    this.images = new Map();
    if (!document.querySelector('link[href="css/fw-browser.css"]')) document.head.append(h("link", { rel: "stylesheet", href: "css/fw-browser.css" }));
    this.views = [];
    this.fb = this.view(() => this.slot, () => this.close(), "move.fwbrowser");
    this.slotSeg = h("div", { class: "mv-br-slots", role: "radiogroup", "aria-label": "Into slot" });
    this.title = h("b", {}, "Firmware");
    this.foot = h("div", { class: "mv-br-foot" });
    this.scrim = h("div", { class: "mv-br-scrim", hidden: true, onclick: () => this.close() });
    this.el = h("section", { class: "mv-br", id: "mv-browse", role: "dialog", "aria-modal": "true", "aria-label": "Firmware on the Move", tabindex: "-1", hidden: true },
      h("div", { class: "mv-br-head" }, this.title, h("span", { class: "mv-br-into" }, "into"), this.slotSeg, h("span", { class: "mv-spacer" }),
        h("button", { class: "mv-br-x", "aria-label": "Close the firmware browser", onclick: () => this.close() }, svg(IC.close, 16))),
      h("div", { class: "mv-br-body" }, this.fb.el), this.foot);
    this.el.addEventListener("keydown", (e) => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); this.close(); } });
    document.body.append(this.scrim, this.el);
    store.on((what) => { if (what === "mix") { const k = this.mixKey(); if (k !== this.lastMix) { this.lastMix = k; this.refresh(); if (this.isOpen) this.head(); } } });
    catalogImages().then((m) => { this.images = m; });
    this.load();
  }
  // a browser over the catalog: slot() the slot a tap loads into, done() closes what it is in
  view(slot, done, key) {
    const fb = new FwBrowser({
      key, favourites: true, install: true, fantasy: true, categories: CATS,
      items: () => this.items(slot()), onPick: (f) => this.pick(f, slot(), done),
      label: (f) => (f.installed === "" ? `A tap downloads it to the Move, then loads it into slot ${slot() + 1}`
        : f.update ? `A tap updates it on the Move to ${f.version}, then loads it into slot ${slot() + 1}` : `A tap loads it into slot ${slot() + 1}`),
      empty: "No firmware matches", none: "The Move's catalog is empty: no index, and nothing installed.",
    });
    this.views.push(fb);
    return fb;
  }
  refresh() { for (const v of this.views) if (v.el.isConnected) v.refresh(); }
  // the settings sheet's Firmware page (site.patch's sheet.js moveFirmwarePage): the catalog for the slot that plays
  sheetPage(closeSheet) {
    this.sheetClose = closeSheet;
    if (!this.sheet) {
      const fb = this.view(() => this.store.active, () => this.sheetClose && this.sheetClose(), "move.fwbrowser.sheet");
      const into = h("span", { class: "mv-sh-into" });
      const big = h("button", { class: "mv-pill mv-browse-btn", id: "mv-sh-big", title: "The browser as a big window, for any slot",
        onclick: () => { const s = this.store.active; if (this.sheetClose) this.sheetClose(); this.open(s); } }, "Bigger view");
      const note = h("p", { class: "note mv-sh-note" });
      this.sheet = { fb, into, note, el: [h("div", { class: "mv-sh-head" }, into, big), fb.el, note] };
    }
    const sh = this.sheet, st = this.store, s = st.slot(st.active);
    sh.into.replaceChildren(h("i", { style: { background: st.colorOf(st.active) } }), `A tap loads into slot ${st.active + 1}`,
      h("span", { class: "mv-mut" }, s.on ? ` (${s.name} plays there)` : " (empty)"));
    sh.note.textContent = `${this.footText()}. A firmware not on the Move, or with an update, is downloaded by the Move first.`;
    sh.fb.refresh();
    if (!(Date.now() - (this.loadedAt || 0) < 3000)) { this.loadedAt = Date.now(); this.load(); }   // (the sheet renders often)
    return sh.el;
  }
  mixKey() { const st = this.store; return JSON.stringify([st.active, st.slots.map((s) => [s.on, s.id]), st.pending.map((p) => p && p.id)]); }

  // the Move's catalog (GET /api/catalog; a host without it: the installed firmwares only)
  async load() {
    try {
      const r = await fetch("api/catalog", { cache: "no-cache" });
      if (r.ok) { this.cat = await r.json(); this.old = false; }
      else throw new Error(String(r.status));
    } catch (_) {
      this.old = true;
      this.cat = { firmwares: this.store.firmwares().map((f) => ({ id: f.id, name: f.name, version: f.version, installed: f.version, categories: f.categories,
        description: "", virtual: !!(f.rfm1 && f.rfm1.virtual), indexed: false, thumb: "" })), favorites: [] };
    }
    const key = JSON.stringify(this.cat);
    if (key !== this.catKey) { this.catKey = key; this.refresh(); if (this.isOpen) this.head(); }
    return this.cat;
  }
  entry(id) { return (this.cat.firmwares || []).find((f) => f.id === id) || null; }
  items(slot = this.slot) {
    const st = this.store, favs = new Set(this.cat.favorites || []), tgt = st.slot(slot);
    return (this.cat.firmwares || []).map((f) => {
      const k = st.slots.findIndex((s) => s.on && s.id === f.id), busy = this.dl && this.dl.id === f.id ? this.dl.pct : null;
      return { id: f.id, name: f.name || f.id, version: f.version, installed: f.installed || "", categories: f.categories || [], desc: f.description,
        fantasy: !!f.virtual, fav: favs.has(f.id), thumb: f.thumb || null, busy,
        update: !!(f.installed && f.indexed && f.installed !== f.version),
        current: !!(tgt.on && tgt.id === f.id), note: k >= 0 && k !== slot ? `on ${k + 1}` : null };
    });
  }
  head() {
    this.title.textContent = "Firmware";
    this.slotSeg.replaceChildren(...[0, 1, 2].map((i) => {
      const s = this.store.slot(i);
      return h("button", { class: `mv-br-slot${i === this.slot ? " on" : ""}`, role: "radio", "aria-checked": String(i === this.slot), "data-slot": i,
        style: { "--c": this.store.colorOf(i) }, title: s.on ? `Slot ${i + 1}: ${s.name}` : `Slot ${i + 1}: empty`,
        onclick: () => { this.slot = i; this.head(); this.fb.refresh(); } }, h("i"), `Slot ${i + 1}`);
    }));
    this.foot.textContent = this.footText();
  }
  footText() {
    const n = (this.cat.firmwares || []).length, inst = (this.cat.firmwares || []).filter((f) => f.installed).length;
    const gen = this.cat.generated ? this.cat.generated.slice(0, 10) : "";
    return this.old ? `${n} firmwares on this Move (this host has no catalog for the page)`
      : `${n} firmwares, ${inst} on this Move · the Move's catalog${gen ? ` of ${gen}` : ""}${this.cat.from === 0 ? " (no index: only what is installed)" : ""} · thumbnails from RemoteFM1FW's catalog, else the Move's own`;
  }

  open(slot = this.store.active) {
    this.slot = slot; this.isOpen = true;
    this.el.hidden = this.scrim.hidden = false;
    this.lastMix = this.mixKey();
    this.head(); this.fb.refresh();
    this.el.focus({ preventScroll: true });
    this.fb.focus();
    this.load();
  }
  close() { if (!this.isOpen) return false; this.isOpen = false; this.el.hidden = this.scrim.hidden = true; return true; }

  // ---- a tap
  async pick(f, slot = this.slot, done = () => this.close()) {
    const st = this.store;
    if (this.dl) { this.toast(`${this.dl.name} is still downloading`, ""); return; }
    if (f.installed && !f.update) {
      const here = st.slot(slot);
      done();
      if (here.on && here.id === f.id) st.activate(slot);
      else st.load(slot, { id: f.id, name: f.name }, true);
      return;
    }
    await this.download(f, slot, done);
  }
  // the Move downloads it (its own catalog's download: signed index, SHA-256s), then it loads into the slot
  async download(f, slot, done = () => this.close()) {
    const st0 = await getStatus();
    const before = st0 ? st0.dl_failed | 0 : 0;
    this.dl = { id: f.id, name: f.name, slot, pct: 0 };
    this.refresh();
    this.toast(`${f.update ? "Updating" : "Downloading"} ${f.name} ${f.version} on the Move…`, "");
    let ok = false, why = "";
    try {
      const r = await fetch("api/download", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ id: f.id }) });
      if (!r.ok) throw new Error((await r.json().catch(() => ({}))).error || `HTTP ${r.status}`);
      const t0 = Date.now();
      while (Date.now() - t0 < DL_MS) {
        await sleep(400);
        const s = await getStatus();
        if (s && s.dl_id === f.id && s.dl_state === 1 && this.dl) { const p = s.dl_pct | 0; if (p !== this.dl.pct) { this.dl.pct = p; this.refresh(); } }
        if (s && (s.dl_failed | 0) > before) { why = s.dl_err || "it failed"; break; }
        if (s && s.dl_id === f.id && s.dl_state === 2) {
          await this.load();
          const e = this.entry(f.id);
          if (e && e.installed === f.version) { ok = true; break; }
        }
      }
      if (!ok && !why) why = "it took too long";
    } catch (e) { why = e.message || String(e); }
    this.dl = null;
    if (!ok) { await this.load(); this.refresh(); this.toast(`The Move could not download ${f.name}: ${why}`, "bad"); return; }
    // the page's list of the Move's firmwares (the slot menus, the settings sheet) takes it in, then it loads
    try {
      const c = await moveCatalog();
      const list = this.store.catalog.firmwares;
      list.splice(0, list.length, ...c.firmwares);
      this.store.catalog.categories = c.categories;
    } catch (_) { /* (the load below still goes by id) */ }
    this.refresh();
    this.toast(`${f.name} ${f.version} is on the Move: loading it into slot ${slot + 1}`, "");
    done();
    await sleep(300);
    this.store.load(slot, { id: f.id, name: f.name }, true);
  }

  // a small thumbnail for the load menus' rows (the catalog image, else the Move's 1-bit one, else the colour)
  thumb(id, name) {
    const f = this.entry(id), url = this.images.get(id), bits = thumbBits(f && f.thumb);
    const w = h("span", { class: "mv-th", style: { "--fb-stand": standIn(id || name) }, "aria-hidden": "true" });
    const cs = getComputedStyle(document.documentElement);
    const put = (src, px) => { const i = h("img", { alt: "", class: px ? "px" : "", decoding: "async" }); i.onerror = () => { i.remove(); if (!px && bits) put(bitsUrl(bits, cs.getPropertyValue("--text").trim() || "#F3EBDC", "#141210"), true); }; i.src = src; w.append(i); };
    if (url) put(url, false); else if (bits) put(bitsUrl(bits, cs.getPropertyValue("--text").trim() || "#F3EBDC", "#141210"), true);
    return w;
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function getStatus() { try { const r = await fetch("api/status", { cache: "no-cache" }); return r.ok ? await r.json() : null; } catch (_) { return null; } }

export function mountMoveBrowse(o) { return new MoveBrowse(o); }
