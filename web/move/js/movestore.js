// SPDX-License-Identifier: GPL-3.0-only
// The Move's States and projects on RemoteFM1's page (RemoteFM14Move; only when js/host.js ON_MOVE): one panel with two
// tabs, from the header's States and Projects caps (desktop), a slot's load menu ("States of slot n"), the phone's slot
// menu. Everything acts on the Move's own States and projects through the host (src/web.c /api/store, engine.h
// engine_store: the same work as the Move's pads), so the page and the Move always agree; nothing is kept in the browser.
//   - States: a slot's firmware's 32 States as the Move's pads (4 rows of 8, state 1 top left). (rfm1cs 0.14.0, the
//     owner's 2026-11-24) State 1 is the open project's own sound (white, PROJECT): Save now (the project's save),
//     Download (the project's copy as a state file), Upload (replaces it and loads it: asked first), Reset to default
//     (the firmware restarts as first installed: asked first; Undo on the Move). States 2..32 are a library shared by
//     all projects, empty / stored (the slot's colour), never changed by playing: Load into the project, Save the
//     sound here / over it (asked first), Download (a .zip: its flash image, its working memory, its .json), Upload (a
//     state file of this firmware: another firmware's is refused by the Move in plain words), Reset to default (the
//     firmware as first installed stored there), Delete (asked first). The other two slots one tap away.
//   - Projects: the sequencer's 32 projects (project 1 top left) in their colours, the open one white. A tap on the open
//     one saves it ("Saved"); an empty one: New project (rfm1cs 0.14.0's rfm_move_project_new) or Upload; Open, Save, Duplicate (then a tap on an empty slot), Download (a .zip: the project, its
//     FX state, its firmwares' copies, so it restores its sound on another Move), Upload (that .zip, or a .rfmmove
//     from RemoteFM1's web player), Delete (asked first).
//   - live: the mix's store_ver (each change on the Move) reads GET /api/store again.
//   mountMoveStore({ store, toast }) -> { view } (view.open("states" | "projects", slot?), view.close(), view.toggle(tab))
import { h, svg, NSLOT } from "./moveui.js";
import { download } from "./movefs.js";

const IC = {
  states: '<rect x="2" y="2" width="5" height="5" rx="1"/><rect x="9" y="2" width="5" height="5" rx="1"/><rect x="2" y="9" width="5" height="5" rx="1"/><rect x="9" y="9" width="5" height="5" rx="1" fill="currentColor"/>',
  projects: '<rect x="2" y="2.5" width="12" height="11" rx="2"/><path d="M2 6h12M6 6v7.5"/>',
  down: '<path d="M8 2v9M4.5 7.5L8 11l3.5-3.5M3 14h10"/>', up: '<path d="M8 11V2M4.5 5.5L8 2l3.5 3.5M3 14h10"/>',
  save: '<path d="M3 2.5h8l2 2V13.5H3z"/><path d="M5.5 2.5v3h4v-3M5 13.5v-4h6v4"/>', load: '<path d="M3 8h8M8 4.5L11.5 8 8 11.5M13.5 3v10"/>',
  copy: '<rect x="5" y="5" width="8.5" height="8.5" rx="1.5"/><path d="M3 10.5V3.5c0-.6.4-1 1-1h7"/>',
  trash: '<path d="M3 4.5h10M6 4.5V3h4v1.5M4.5 4.5l.7 8.5h5.6l.7-8.5"/>', close: '<path d="M4 4l8 8M12 4l-8 8"/>',
  open: '<path d="M2.5 4.5h4l1.3 1.5h5.7v7H2.5z"/>', check: '<path d="M3 8.5l3 3 7-7"/>',
  reset: '<path d="M3.5 8a4.5 4.5 0 1 0 1.4-3.3"/><path d="M4.5 2v3h3"/>',
};
const CSS = `
.mv-sv .mv-fs-head { gap: 8px; }
.mv-sv-tabs { display: flex; gap: 4px; background: var(--well, #15130f); border-radius: 999px; padding: 3px; }
.mv-sv-tabs button { height: 34px; padding: 0 14px; border-radius: 999px; border: 0; background: none; color: var(--muted, #aaa); font: 700 14px/1 var(--ui, inherit);
  display: inline-flex; align-items: center; gap: 7px; cursor: pointer; }
.mv-sv-tabs button[aria-selected="true"] { background: var(--raised, #333); color: var(--text, #fff); }
.mv-sv-body { flex: 1; overflow-y: auto; padding: 12px 18px 14px; display: flex; flex-direction: column; gap: 12px; scrollbar-width: thin; }
.mv-sv-slots { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; }
.mv-sv-slot { --c: #9696a0; min-width: 0; height: 46px; display: flex; align-items: center; gap: 9px; padding: 0 10px; border-radius: 12px; border: 1px solid var(--border, #444);
  background: none; color: var(--row-text, #ddd); cursor: pointer; text-align: left; font: inherit; }
.mv-sv-slot:disabled { opacity: .45; cursor: default; border-style: dashed; }
.mv-sv-slot[aria-pressed="true"] { border-color: var(--c); box-shadow: 0 0 0 1px var(--c), 0 0 14px -4px var(--c); background: var(--raised, #2a2622); color: var(--text, #fff); }
.mv-sv-slot .n { font: 700 11px/1 var(--hw, inherit); letter-spacing: .12em; color: var(--muted, #999); }
.mv-sv-slot .t { min-width: 0; display: flex; flex-direction: column; gap: 3px; }
.mv-sv-slot b { font: 700 14px/1.1 var(--ui, inherit); overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mv-sv-slot span.v { font-size: 11.5px; color: var(--muted, #999); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.mv-sv-slot i { width: 10px; height: 10px; border-radius: 50%; background: var(--c); flex: none; }
.mv-sv-grid { --c: #9696a0; display: grid; grid-template-columns: repeat(8, minmax(0, 1fr)); gap: 8px; user-select: none; }
.mv-sv-pad { position: relative; aspect-ratio: 1; min-width: 0; border-radius: 10px; border: 2px dashed var(--border, #3a3530); background: rgba(0,0,0,.18);
  color: var(--muted, #8a8278); font: 700 15px/1 var(--hw, inherit); cursor: pointer; display: grid; place-items: center; padding: 0;
  transition: transform .06s, box-shadow .15s, background-color .15s; }
.mv-sv-pad:hover { border-color: var(--muted, #777); }
.mv-sv-pad:active { transform: scale(.96); }
.mv-sv-pad.full { border: 2px solid var(--pc, var(--c)); background: color-mix(in srgb, var(--pc, var(--c)) 38%, #14110e); color: #fff; }
.mv-sv-pad.cur { border: 2px solid #fff; background: #f4f1ea; color: #17140F; box-shadow: 0 0 0 2px var(--pc, var(--c)), 0 0 18px -3px #fff; }
.mv-sv-pad .tag { position: absolute; left: 0; right: 0; bottom: 6%; font: 700 9px/1 var(--hw, inherit); letter-spacing: .1em; text-transform: uppercase; opacity: .85; }
.mv-sv-pad[aria-selected="true"] { outline: 3px solid var(--accent, #EE7A3C); outline-offset: 2px; }
.mv-sv-pad.target { border-color: var(--accent, #EE7A3C); border-style: solid; animation: mv-sv-glow 1s ease-in-out infinite; }
.mv-sv-pad.saved::after { content: "Saved"; position: absolute; inset: -9px -10px auto; margin: auto; width: max-content; padding: 3px 7px; border-radius: 7px;
  background: var(--green, #3BD47A); color: #0f1a12; font: 700 11px/1 var(--ui, inherit); animation: mv-in .15s ease-out; z-index: 1; }
.mv-sv-pad.busy { animation: mv-pulse .8s ease-in-out infinite; }
@keyframes mv-sv-glow { 50% { box-shadow: 0 0 12px -2px var(--accent, #EE7A3C); } }
.mv-sv-sel { border-radius: 16px; background: var(--surface, #211e1a); border: 1px solid var(--line, #332e28); padding: 12px 14px; display: flex; flex-direction: column; gap: 10px; }
.mv-sv-sel h4 { margin: 0; font: 700 16px/1.2 var(--ui, inherit); color: var(--text, #fff); display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.mv-sv-sel h4 .badge { font: 700 10px/1 var(--hw, inherit); letter-spacing: .1em; text-transform: uppercase; padding: 4px 7px; border-radius: 6px; background: #f4f1ea; color: #17140F; }
.mv-sv-sel .meta { color: var(--muted, #aaa); font-size: 13px; line-height: 1.45; }
.mv-sv-acts { display: flex; flex-wrap: wrap; gap: 8px; }
.mv-sv-acts .pill { gap: 6px; padding: 0 13px; }
.mv-sv-ask { display: flex; flex-wrap: wrap; align-items: center; gap: 8px; }
.mv-sv-ask .grow { flex: 1 1 200px; color: var(--row-text, #ddd); font-size: 13.5px; }
.mv-sv-note { color: var(--muted, #999); font-size: 12.5px; line-height: 1.45; margin: 0; }
.mv-sv-prog { height: 3px; background: var(--accent, #3cc8dc); width: 0; transition: width .2s; }
@media (max-width: 640px) {
  .mv-sv-body { padding: 10px 12px 14px; }
  .mv-sv-grid { gap: 5px; }
  .mv-sv-pad { border-radius: 8px; font-size: 13px; }
  .mv-sv-pad .tag { display: none; }
  .mv-sv-slot { height: 44px; padding: 0 8px; gap: 6px; }
  .mv-sv-slot .n { display: none; }
  .mv-sv-tabs button { padding: 0 11px; font-size: 13px; }
  .mv-sv .mv-fs-head .sub { display: none; }
}
`;
const enc = encodeURIComponent;
const when = (t) => { const d = new Date(t * 1000); return isNaN(d) || !t ? "" : `${d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" })} ${d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" })}`; };

// one request to the Move (POST /api/store), answered when done: { ok, message } or { ok: false, error }
async function ask(body) {
  try {
    const r = await fetch("api/store", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    const j = await r.json().catch(() => ({}));
    return r.ok && j.ok ? { ok: true, message: j.message || "" } : { ok: false, error: j.error || `error ${r.status}` };
  } catch (e) { return { ok: false, error: "the Move did not answer" }; }
}
// a file to the Move (POST /api/store/put?..), with its progress
function send(query, file, onProgress) {
  return new Promise((resolve) => {
    const x = new XMLHttpRequest();
    x.open("POST", `api/store/put?${query}`);
    x.upload.onprogress = (ev) => { if (ev.lengthComputable && onProgress) onProgress(ev.loaded / ev.total); };
    x.onload = () => { let j = {}; try { j = JSON.parse(x.responseText); } catch (_) { /* (no JSON) */ }
      resolve(x.status >= 200 && x.status < 300 && j.ok ? { ok: true, message: j.message || "" } : { ok: false, error: j.error || `error ${x.status}` }); };
    x.onerror = () => resolve({ ok: false, error: "the upload failed" });
    x.send(file);
  });
}

export class StoreView {
  constructor({ store, toast }) {
    this.store = store;
    this.toast = (m, kind) => { this.status.textContent = m; this.status.dataset.kind = kind || ""; if (toast) toast(m, kind); };
    this.tab = "states"; this.slot = null; this.data = null; this.ver = -1; this.isOpen = false;
    this.sel = { states: null, projects: null };   // the picked pad
    this.inline = null;                             // a question under the picked pad: { kind, ... }
    this.busy = "";                                 // a request under way (its words)
    this.dupFrom = null;                            // Duplicate: the project copied, waiting for an empty slot
    this.savedAt = 0;                               // the open project's "Saved" mark
    this.meta = new Map();                          // a state's .json (its firmware's version, when saved), by key
    if (!document.getElementById("mv-sv-css")) document.head.append(h("style", { id: "mv-sv-css" }, CSS));
    // (a click outside closes it, only one that also began outside: a press on a pad whose release lands elsewhere,
    // or a pad drawn again under it, does not)
    this.scrim = h("div", { class: "mv-fs-scrim", hidden: true, onpointerdown: (e) => { this.outside = e.target === this.scrim; },
      onclick: (e) => { if (this.outside && e.target === this.scrim) this.close(); this.outside = false; } });
    this.pick = h("input", { type: "file", accept: ".zip,.rfmmove,application/zip", hidden: true, onchange: () => { const f = this.pick.files[0]; this.pick.value = ""; if (f && this.onPick) this.onPick(f); } });
    this.tabBtn = {
      states: h("button", { role: "tab", id: "mv-sv-tab-states", "aria-controls": "mv-sv-body", onclick: () => this.show("states") }, svg(IC.states, 15), "States"),
      projects: h("button", { role: "tab", id: "mv-sv-tab-projects", "aria-controls": "mv-sv-body", onclick: () => this.show("projects") }, svg(IC.projects, 15), "Projects"),
    };
    this.body = h("div", { class: "mv-sv-body", id: "mv-sv-body", role: "tabpanel" });
    this.prog = h("div", { class: "mv-sv-prog" });
    this.info = h("span", {}); this.status = h("span", { role: "status" });
    this.el = h("section", { class: "mv-fs mv-sv", id: "mv-sv", role: "dialog", "aria-modal": "true", "aria-label": "States and projects on the Move", tabindex: "-1", hidden: true },
      h("div", { class: "mv-fs-head" }, h("div", { class: "mv-sv-tabs", role: "tablist" }, this.tabBtn.states, this.tabBtn.projects),
        h("span", { class: "sub" }, "on the Move"), h("span", { class: "sp" }),
        h("button", { class: "mv-fs-x", "aria-label": "Close States and projects", onclick: () => this.close() }, svg(IC.close, 16))),
      this.prog, this.body, h("div", { class: "mv-fs-foot" }, this.info, this.status), this.pick);
    document.body.append(this.scrim, this.el);
    this.el.addEventListener("keydown", (e) => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); this.escape(); } });
    // the Move changed something (a pad, a save, a project opened): read again
    store.on((what) => {
      if (what !== "mix") return;
      const v = store.mix && store.mix.store_ver;
      if (v != null && v !== this.ver && (this.isOpen || this.ver < 0)) this.refresh();
      else if (this.isOpen && this.slot != null && !(this.slotInfo(this.slot))) this.render();
    });
    window.FM1MoveStore = this;
  }

  open(tab = this.tab, slot = null) {   // (slot: whose States; none: the playing one's)
    this.tab = tab;
    if (slot !== this.slot) this.sel.states = null;
    this.slot = slot;
    this.isOpen = true; this.el.hidden = this.scrim.hidden = false; this.el.focus();
    this.render(); this.refresh();
  }
  escape() { if (this.inline || this.dupFrom != null) { this.inline = null; this.dupFrom = null; this.render(); } else this.close(); }
  close() { this.isOpen = false; this.el.hidden = this.scrim.hidden = true; this.inline = null; this.dupFrom = null; }
  toggle(tab) { if (this.isOpen && (!tab || tab === this.tab)) this.close(); else this.open(tab || this.tab); }
  show(tab) { this.tab = tab; this.inline = null; this.dupFrom = null; this.render(); }

  async refresh() {
    try {
      const r = await fetch("api/store", { cache: "no-store" });
      if (!r.ok) throw new Error(`error ${r.status}`);
      const j = await r.json();
      this.data = j; this.ver = j.ver;
      for (const k of [...this.meta.keys()]) if (!this.stored(k)) this.meta.delete(k);
    } catch (_) { /* (the next change reads it again) */ }
    this.render();
  }
  // ---- what the Move has
  slotInfo(s) { const d = this.data; return d && d.slots ? d.slots[s] || null : null; }
  stored(key) {   // a meta key "id:k" still stored?
    const [id, k] = key.split(":"), s = (this.data && this.data.slots || []).findIndex((x) => x && x.id === id);
    return s >= 0 && (this.data.slots[s].used >>> +k & 1);
  }
  curSlot() {
    if (this.slot != null && this.slotInfo(this.slot)) return this.slot;
    const a = this.store.active;
    if (this.slotInfo(a)) return a;
    for (let s = 0; s < NSLOT; s++) if (this.slotInfo(s)) return s;
    return this.slot != null ? this.slot : a;
  }
  fwName(id) { const f = this.store.firmwares().find((x) => x.id === id); return f ? f.name : id; }

  // ---- render
  render() {
    for (const t of ["states", "projects"]) this.tabBtn[t].setAttribute("aria-selected", t === this.tab ? "true" : "false");
    if (!this.isOpen) return;
    // (drawn again only when what it shows changed: a button replaced under a press would lose its click)
    const q = this.inline, key = JSON.stringify([this.tab, this.slot, this.store.active, this.data, this.sel, q && [q.tab, q.k, q.kind, q.file && q.file.name],
      this.busy, this.busyK, this.dupFrom, Date.now() - this.savedAt < 1600, [...this.meta.entries()], this.store.firmwares().length]);
    if (key === this.rkey) return;
    this.rkey = key;
    const kids = this.tab === "states" ? this.renderStates() : this.renderProjects();
    this.body.replaceChildren(...kids.filter(Boolean));
  }
  pad(k, { cls = "", color, tag, label, selected, onclick, title }) {
    return h("button", { class: `mv-sv-pad ${cls}`, "data-k": k, role: "gridcell", "aria-selected": selected ? "true" : "false",
      "aria-label": label, title, style: color ? { "--pc": color } : null, onclick }, String(k + 1), tag ? h("span", { class: "tag" }, tag) : null);
  }
  pill(text, ic, on, cls = "", attrs = {}) { return h("button", { class: `pill ${cls}`, onclick: on, disabled: !!(this.busy || attrs.disabled), title: attrs.title, "data-act": attrs.act }, ic ? svg(ic, 14) : null, text); }
  askRow(text, yes, yesText, cls = "danger-fill") {
    return h("div", { class: "mv-sv-ask" }, h("span", { class: "grow" }, text),
      h("button", { class: `pill ${cls}`, "data-act": "yes", onclick: () => { this.inline = null; yes(); } }, yesText),
      h("button", { class: "pill ghost", "data-act": "no", onclick: () => { this.inline = null; this.render(); } }, "Cancel"));
  }

  renderStates() {
    const s = this.curSlot(), si = this.slotInfo(s);
    const slots = h("div", { class: "mv-sv-slots", role: "group", "aria-label": "Firmware slot" }, [0, 1, 2].map((i) => {
      const x = this.slotInfo(i), c = this.store.colorOf(i);
      return h("button", { class: "mv-sv-slot", "data-slot": i, disabled: !x, "aria-pressed": i === s && x ? "true" : "false", style: { "--c": x ? x.color || c : null },
        onclick: () => { this.slot = i; this.sel.states = null; this.inline = null; this.render(); } },
        h("span", { class: "n" }, i + 1), x ? h("i") : null,
        h("span", { class: "t" }, h("b", {}, x ? x.name : `Slot ${i + 1}`), h("span", { class: "v" }, x ? `${x.version}${i === this.store.active ? " · playing" : ""}` : "empty")));
    }));
    if (!si) {
      this.info.textContent = "";
      return [slots, h("p", { class: "mv-sv-note" }, this.data ? "No firmware in this slot: load one from its chip, then its States are here." : "Reading the Move…")];
    }
    // (rfm1cs 0.14.0, the owner's 2026-11-24) pad 1 is the open project's own sound; pads 2..32 a library shared by
    // all projects (nothing in it changes by playing)
    const used = (si.used >>> 0) & ~1, k = this.sel.states, pj = si.proj;
    const n = [...Array(32).keys()].filter((i) => used >>> i & 1).length;
    this.info.textContent = `${si.name} ${si.version}: ${pj >= 0 ? `project ${pj + 1}'s sound and ` : ""}${n} of 31 library states stored`;
    const grid = h("div", { class: "mv-sv-grid", role: "grid", "aria-label": `${si.name}'s States`, style: { "--c": si.color || "#9696a0" } },
      [...Array(32).keys()].map((i) => {
        const full = i > 0 && used >>> i & 1;
        if (i === 0)
          return this.pad(0, { cls: `cur proj ${this.busyK === 0 && this.tab === "states" ? "busy" : ""}`, tag: "project",
            label: `State 1: ${pj >= 0 ? `project ${pj + 1}'s` : "the project's"} own sound`, selected: k === 0,
            onclick: () => { this.sel.states = 0; this.inline = null; this.render(); } });
        return this.pad(i, { cls: `${full ? "full" : ""} ${this.busyK === i && this.tab === "states" ? "busy" : ""}`,
          label: `State ${i + 1}: ${full ? "stored" : "empty"}`, selected: i === k,
          onclick: () => { this.sel.states = i; this.inline = null; this.render(); } });
      }));
    return [slots, grid, k != null ? (k === 0 ? this.projRow(s, si) : this.stateRow(s, si, k))
      : h("p", { class: "mv-sv-note" }, `Tap a state. State 1 (white) is the open project's own sound, saved as you play; states 2 to 32 are a library every project shares: an empty one takes the sound as it is now, a stored one loads into the project. As on the Move: ${["Track 1", "Track 2", "Track 3"][s]} held + pad 7.`)];
  }
  // pad 1: the open project's own sound
  projRow(s, si) {
    const pj = si.proj, name = pj >= 0 ? `Project ${pj + 1}'s sound` : "The project's sound", lname = name.charAt(0).toLowerCase() + name.slice(1);
    const title = h("h4", {}, name, h("span", { class: "badge" }, "Project"));
    const meta = h("div", { class: "meta" }, `${si.name} ${si.version} as you play it in ${pj >= 0 ? `project ${pj + 1}` : "the open project"}: its own copy, saved with the project.${si.undo ? " Undo on the Move brings back the sound before the last load, paste or reset." : ""}`);
    let acts;
    const q = this.inline;
    if (q && q.k === 0 && q.tab === "states") {
      if (q.kind === "reset") acts = this.askRow(`Reset ${lname} to default? The firmware restarts as first installed (Undo on the Move brings it back).`,
        () => this.stateOp("state-reset", s, si, 0), "Reset it");
      else if (q.kind === "upload") acts = this.askRow(q.text, () => this.stateUpload(s, si, 0, q.file, 1), "Replace and load it", "primary");
    } else {
      acts = h("div", { class: "mv-sv-acts" },
        this.pill("Save now", IC.save, () => this.projectSave(pj), "primary", { act: "save", disabled: pj < 0, title: "The project saved now, this sound with it" }),
        this.pill("Download", IC.down, () => this.stateDownload(s, si, 0), "", { act: "download", disabled: pj < 0, title: "This project's copy of the firmware as a state file" }),
        this.pill("Upload…", IC.up, () => this.pickFile((f) => { this.inline = { tab: "states", k: 0, kind: "upload", file: f,
          text: `Replace ${lname} with ${f.name} and load it?` }; this.render(); }), "", { act: "upload", disabled: pj < 0 }),
        this.pill("Reset to default", IC.reset, () => { this.inline = { tab: "states", k: 0, kind: "reset" }; this.render(); }, "", { act: "reset", disabled: pj < 0,
          title: "The firmware as first installed: it restarts fresh" }));
    }
    return h("div", { class: "mv-sv-sel", "data-k": 0 }, title, meta, acts);
  }
  // pads 2..32: the library
  stateRow(s, si, k) {
    const full = si.used >>> k & 1, key = `${si.id}:${k}`, m = this.meta.get(key);
    if (full && !m && !this.busy) this.readMeta(si.id, k);   // (not while a request is under way: its file may not be there yet)
    const title = h("h4", {}, `State ${k + 1}`);
    const meta = h("div", { class: "meta", "data-meta": key },
      !full ? "Empty." : m && !m.pending && m.id ? `${this.fwName(m.id || si.id)} ${m.version || "?"}${m.version && m.version !== si.version ? ` (this slot runs ${si.version})` : ""}${m.saved ? ` · saved ${when(m.saved)}` : ""}${m.fresh ? " · as first installed" : m.mem ? " · with its working memory" : " · its flash only"}` : `${si.name}`);
    let acts;
    const q = this.inline, pj = si.proj;
    if (q && q.k === k && q.tab === "states") {
      if (q.kind === "delete") acts = this.askRow(`Delete state ${k + 1}? It cannot be undone from the page.`, () => this.stateOp("state-delete", s, si, k), "Delete");
      else if (q.kind === "reset") acts = this.askRow(`Reset state ${k + 1} to default? It holds the firmware as first installed instead of its sound.`, () => this.stateOp("state-reset", s, si, k), "Reset it");
      else if (q.kind === "over") acts = this.askRow(`Save the sound as it is now over state ${k + 1}?`, () => this.stateOp("state-save", s, si, k, { replace: 1 }), "Save over it", "primary");
      else if (q.kind === "upload") acts = this.askRow(q.text, () => this.stateUpload(s, si, k, q.file, 1), "Replace it", "primary");
    } else if (!full) {
      acts = h("div", { class: "mv-sv-acts" },
        this.pill("Save the sound here", IC.save, () => this.stateOp("state-save", s, si, k), "primary", { act: "save" }),
        this.pill("Upload a state file…", IC.up, () => this.pickFile((f) => this.stateUpload(s, si, k, f, 0)), "", { act: "upload" }),
        this.pill("Reset to default", IC.reset, () => this.stateOp("state-reset", s, si, k), "", { act: "reset", title: "Store the firmware as first installed here" }));
    } else {
      acts = h("div", { class: "mv-sv-acts" },
        this.pill("Load into the project", IC.load, () => this.stateOp("state-load", s, si, k), "primary", { act: "load",
          title: `Replaces ${pj >= 0 ? `project ${pj + 1}'s` : "the project's"} sound (Undo on the Move brings it back); this state is not changed by playing` }),
        this.pill("Save the sound over it", IC.save, () => { this.inline = { tab: "states", k, kind: "over" }; this.render(); }, "", { act: "save" }),
        this.pill("Download", IC.down, () => this.stateDownload(s, si, k), "", { act: "download", title: "This state as a file: its flash image, its working memory, its .json" }),
        this.pill("Upload…", IC.up, () => this.pickFile((f) => { this.inline = { tab: "states", k, kind: "upload", file: f, text: `Replace state ${k + 1} with ${f.name}?` }; this.render(); }), "", { act: "upload" }),
        this.pill("Reset to default", IC.reset, () => { this.inline = { tab: "states", k, kind: "reset" }; this.render(); }, "", { act: "reset", title: "This state holds the firmware as first installed" }),
        this.pill("Delete", IC.trash, () => { this.inline = { tab: "states", k, kind: "delete" }; this.render(); }, "danger", { act: "delete" }));
    }
    return h("div", { class: "mv-sv-sel", "data-k": k }, title, meta, acts);
  }
  async readMeta(id, k) {
    const key = `${id}:${k}`;
    this.meta.set(key, { pending: true });
    try {
      const r = await fetch(`api/fs/get?path=${enc(`states/${id}/state-${String(k).padStart(2, "0")}.json`)}`, { cache: "no-store" });
      this.meta.set(key, r.ok ? await r.json() : {});
    } catch (_) { this.meta.set(key, {}); }
    this.render();
  }
  pickFile(then) { this.onPick = then; this.pick.click(); }
  async run(what, k, fn) {
    this.busy = what; this.busyK = k; this.status.textContent = what; this.status.dataset.kind = ""; this.render();
    let r;
    try { r = await fn(); } finally { this.busy = ""; this.busyK = null; this.prog.style.width = "0"; }
    if (r.ok) this.toast(r.message || "Done"); else this.toast(r.error.charAt(0).toUpperCase() + r.error.slice(1), "bad");
    await this.refresh();
    return r;
  }
  async stateOp(op, s, si, k, extra = {}) {
    const words = { "state-save": `Saving state ${k + 1}…`, "state-load": `Loading state ${k + 1}…`, "state-delete": `Deleting state ${k + 1}…`,
      "state-reset": k === 0 ? "Resetting the project's sound…" : `Resetting state ${k + 1}…` }[op];
    this.meta.delete(`${si.id}:${k}`);
    const r = await this.run(words, k, () => ask({ op, slot: s, k, id: si.id, ...extra }));
    this.meta.delete(`${si.id}:${k}`);   // (a row drawn meanwhile read the .json from before: read it again)
    this.render();
    return r;
  }
  async stateDownload(s, si, k) {
    // (pad 1: the project saved first, so its copy is the sound as it is now)
    if (k === 0) { const r = await this.projectSave(si.proj); if (!r.ok) return; }
    download(`api/store/state?slot=${s}&k=${k}`);
  }
  stateUpload(s, si, k, file, flags) {
    const full = k > 0 && si.used >>> k & 1;
    if (full && !(flags & 1)) {   // (asked first)
      this.inline = { tab: "states", k, kind: "upload", file, text: `Replace state ${k + 1} with ${file.name}?` };
      this.render(); return null;
    }
    this.meta.delete(`${si.id}:${k}`);
    return this.run(`Sending ${file.name}…`, k, () => send(`kind=state&slot=${s}&k=${k}${full ? "&replace=1" : ""}`, file,
      (x) => { this.prog.style.width = `${(100 * x).toFixed(1)}%`; }));
  }

  renderProjects() {
    const d = this.data;
    if (!d) return [h("p", { class: "mv-sv-note" }, "Reading the Move…")];
    const used = d.used >>> 0, open = d.open, k = this.sel.projects, by = new Map((d.projects || []).map((p) => [p.k, p]));
    const loading = d.pending >= 0 || d.loading;
    this.info.textContent = `${[...Array(32).keys()].filter((i) => used >>> i & 1).length} of 32 projects stored${open >= 0 ? ` · project ${open + 1} open${loading ? " (loading)" : ""}` : ""}`;
    const dup = this.dupFrom;
    const grid = h("div", { class: "mv-sv-grid", role: "grid", "aria-label": "Projects" }, [...Array(32).keys()].map((i) => {
      const p = by.get(i), full = used >>> i & 1, isOpen = i === open, free = !full && i !== open;
      return this.pad(i, { cls: `${full ? "full" : ""} ${isOpen ? "cur" : ""} ${dup != null && free ? "target" : ""} ${isOpen && Date.now() - this.savedAt < 1600 ? "saved" : ""} ${(this.busyK === i && this.tab === "projects") || (loading && i === (d.pending >= 0 ? d.pending : open)) ? "busy" : ""}`,
        color: p ? p.color : null, tag: isOpen ? "open" : "", selected: i === k && dup == null,
        label: `Project ${i + 1}: ${isOpen ? "open" : full ? "stored" : "empty"}`,
        onclick: () => this.projectTap(i) });
    }));
    const note = dup != null ? h("div", { class: "mv-sv-sel", "data-dup": dup }, h("h4", {}, `Duplicate project ${dup + 1}`),
      h("div", { class: "mv-sv-ask" }, h("span", { class: "grow" }, "Tap an empty slot for the copy (its firmwares' copies go with it)."),
        h("button", { class: "pill ghost", "data-act": "no", onclick: () => { this.dupFrom = null; this.render(); } }, "Cancel")))
      : k != null ? this.projectRow(k, by.get(k), used, open, loading)
      : h("p", { class: "mv-sv-note" }, "Tap a project. The open one (white) is the Move's; tap it to save it now. Projects keep their firmwares and each firmware's sound.");
    return [grid, note];
  }
  projectTap(i) {
    const d = this.data, used = d.used >>> 0;
    if (this.busy) return;
    if (this.dupFrom != null) {
      if (used >>> i & 1 || i === d.open) { this.toast(`Project ${i + 1} is not empty: pick an empty slot`, "bad"); return; }
      const from = this.dupFrom; this.dupFrom = null; this.sel.projects = i;
      this.run(`Copying project ${from + 1} into ${i + 1}…`, i, () => ask({ op: "project-dup", k: from, to: i }));
      return;
    }
    this.sel.projects = i; this.inline = null;
    if (i === d.open && (used >>> i & 1 || d.dirty)) { this.projectSave(i); return; }   // (the owner's: a tap on the open one saves it)
    this.render();
  }
  async projectSave(k) {
    const r = await this.run(`Saving project ${k + 1}…`, k, () => ask({ op: "project-save", k }));
    if (r.ok) { this.savedAt = Date.now(); this.render(); setTimeout(() => this.render(), 1700); }
    return r;
  }
  projectRow(k, p, used, open, loading) {
    const full = used >>> k & 1, isOpen = k === open;
    const fws = p ? p.fw.map((id, i) => (id ? `${i + 1} ${this.fwName(id)}` : "")).filter(Boolean) : [];
    const title = h("h4", {}, `Project ${k + 1}`, isOpen ? h("span", { class: "badge" }, "Open") : null);
    const meta = h("div", { class: "meta" }, !full ? (isOpen ? "Open on the Move, not saved yet." : "Empty.")
      : `${p ? `${p.bpm} BPM · ${p.clips} clip${p.clips === 1 ? "" : "s"}` : ""}${fws.length ? ` · firmwares: ${fws.join(", ")}` : p ? " · no firmwares" : ""}`);
    let acts;
    const q = this.inline;
    if (q && q.k === k && q.tab === "projects") {
      if (q.kind === "delete") acts = this.askRow(isOpen ? `Delete project ${k + 1}? It is open: the Move keeps playing an empty project in its place.` : `Delete project ${k + 1} with its firmwares' copies? It cannot be undone from the page.`,
        () => this.run(`Deleting project ${k + 1}…`, k, () => ask({ op: "project-delete", k })), "Delete");
      else if (q.kind === "upload") acts = this.askRow(`Replace project ${k + 1} with ${q.file.name}?`, () => this.projectUpload(k, q.file, 1), "Replace it", "primary");
    } else if (!full && !isOpen) {
      acts = h("div", { class: "mv-sv-acts" },
        this.pill("New project", IC.open, () => this.run(`Making project ${k + 1}…`, k, () => ask({ op: "project-new", k })), "primary",
          { act: "new", disabled: loading, title: "A new empty project here, opened on the Move (as an empty pad's click there)" }),
        this.pill("Upload a project file…", IC.up, () => this.pickFile((f) => this.projectUpload(k, f, 0)), "", { act: "upload" }));
    } else {
      acts = h("div", { class: "mv-sv-acts" },
        isOpen ? this.pill("Save", IC.save, () => this.projectSave(k), "primary", { act: "save" })
          : this.pill("Open", IC.open, () => this.run(`Opening project ${k + 1}…`, k, () => ask({ op: "project-open", k })), "primary", { act: "open", disabled: loading }),
        full ? this.pill("Duplicate", IC.copy, () => { this.dupFrom = k; this.render(); }, "", { act: "dup", title: "A copy in an empty slot, with its firmwares' copies" }) : null,
        full ? this.pill("Download", IC.down, () => this.projectDownload(k, isOpen), "", { act: "download", title: "This project as a file: the project, its FX, its firmwares' copies" }) : null,
        !isOpen ? this.pill("Upload…", IC.up, () => this.pickFile((f) => { this.inline = { tab: "projects", k, kind: "upload", file: f }; this.render(); }), "", { act: "upload" }) : null,
        full || isOpen ? this.pill("Delete", IC.trash, () => { this.inline = { tab: "projects", k, kind: "delete" }; this.render(); }, "danger", { act: "delete" }) : null);
    }
    return h("div", { class: "mv-sv-sel", "data-k": k }, title, meta, acts);
  }
  async projectDownload(k, isOpen) {
    if (isOpen) { const r = await this.projectSave(k); if (!r.ok) return; }   // (the open one as it is now, its copies too)
    download(`api/store/project?k=${k}`);
  }
  projectUpload(k, file, replace) {
    return this.run(`Sending ${file.name}…`, k, () => send(`kind=project&k=${k}${replace ? "&replace=1" : ""}`, file,
      (x) => { this.prog.style.width = `${(100 * x).toFixed(1)}%`; }));
  }
}

export function mountMoveStore({ store, toast }) {
  return { view: new StoreView({ store, toast }) };
}
export { IC as STORE_IC };
