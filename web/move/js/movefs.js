// SPDX-License-Identifier: GPL-3.0-only
// The Move's file browser on RemoteFM1's page (RemoteFM14Move): the RemoteFM1 folder on the Move (firmwares/, states/,
// move/, audio/, code/, data/, catalog/) over the host's API (src/web.c /api/fs: list, get, zip, put, mkdir, rename,
// delete; the rules are the host's, src/files.h). A panel like the settings sheet, from the header's Files button or
// the sheet's Files section: breadcrumbs, each entry's size and date, a download (a folder as a .zip), upload (a
// file picker, or files dropped on the panel), rename, delete (asked first; a folder with things in it asked twice),
// a new folder; the exports (all the sequencer projects, the firmwares' states) one click away. What the Move keeps
// to itself (firmwares/, catalog/, what a running firmware or the open project needs) shows read-only with the
// host's reason. Nothing here runs unless js/host.js ON_MOVE (js/moveslots.js mountMoveUi mounts it).
import { h, svg } from "./moveui.js";

const API = "api/fs/";
const IC = {
  folder: '<path d="M1.8 4.2c0-.8.6-1.4 1.4-1.4h3l1.4 1.6h5.2c.8 0 1.4.6 1.4 1.4v6.5c0 .8-.6 1.4-1.4 1.4H3.2c-.8 0-1.4-.6-1.4-1.4z"/>',
  file: '<path d="M4 1.8h5.2L12.5 5v9.2H4z"/><path d="M9 1.8V5h3.5"/>',
  down: '<path d="M8 2v9M4.5 7.5L8 11l3.5-3.5M3 14h10"/>',
  up: '<path d="M8 11V2M4.5 5.5L8 2l3.5 3.5M3 14h10"/>',
  zip: '<path d="M3.5 2h9v12h-9z"/><path d="M8 2v2M7 4h2M8 6v2M7 8h2M7 10h2v2H7z"/>',
  pen: '<path d="M10.5 2.5l3 3L6 13H3v-3z"/>',
  trash: '<path d="M3 4.5h10M6 4.5V3h4v1.5M4.5 4.5l.7 8.5h5.6l.7-8.5"/>',
  plus: '<path d="M8 3v10M3 8h10"/>', close: '<path d="M4 4l8 8M12 4l-8 8"/>', files: '<path d="M2 3.5h4.5l1.3 1.5H14v8H2z"/>',
  lock: '<rect x="3.5" y="7" width="9" height="6.5" rx="1.2"/><path d="M5.5 7V5a2.5 2.5 0 015 0v2"/>',
};
const CSS = `
.mv-fs-scrim { position: fixed; inset: 0; background: rgba(0,0,0,.45); z-index: 30; }
.mv-fs { position: fixed; top: 8px; right: 8px; bottom: 8px; width: min(760px, calc(100vw - 16px)); z-index: 31;
  background: var(--window, #1c1a17); color: var(--text, #eee); border: 1px solid var(--border, #444); border-radius: 24px;
  box-shadow: 0 18px 44px rgba(0,0,0,.5); display: flex; flex-direction: column; font: 14px/1.35 var(--ui, system-ui); outline: none; }
.mv-fs[hidden], .mv-fs-scrim[hidden], .mv-fs-ro[hidden] { display: none; }
.mv-fs-head { height: 60px; flex: none; display: flex; align-items: center; gap: 10px; padding: 0 12px 0 18px; border-bottom: 1px solid var(--divider, #333); }
.mv-fs-head b { font: 700 17px/1 var(--title, inherit); }
.mv-fs-head .sub { color: var(--muted, #999); font-size: 12px; }
.mv-fs-head .sp { flex: 1; }
.mv-fs-x { width: 36px; height: 36px; display: grid; place-items: center; border: 0; background: var(--raised, #333); color: var(--knob-off, #bbb); border-radius: 999px; cursor: pointer; }
.mv-fs-x:hover { color: var(--text, #fff); }
.mv-fs-bar { display: flex; flex-wrap: wrap; gap: 8px; padding: 12px 18px 6px; align-items: center; }
.mv-fs-bar .pill { height: 34px; padding: 0 12px; display: inline-flex; align-items: center; gap: 6px; }
.mv-fs-crumbs { display: flex; flex-wrap: wrap; align-items: center; gap: 2px; padding: 4px 18px 8px; font-size: 13px; }
.mv-fs-crumbs button { background: none; border: 0; color: var(--accent, #3cc8dc); font: 600 13px var(--ui, inherit); cursor: pointer; padding: 4px 6px; border-radius: 8px; }
.mv-fs-crumbs button:hover { background: var(--raised, #333); }
.mv-fs-crumbs button[aria-current] { color: var(--text, #fff); cursor: default; background: none; }
.mv-fs-crumbs .sep { color: var(--muted, #888); }
.mv-fs-ro { margin: 0 18px 8px; padding: 8px 12px; border-radius: 12px; background: var(--surface, #222); border: 1px solid var(--line, #333); color: var(--muted, #aaa); font-size: 12.5px; display: flex; gap: 8px; align-items: flex-start; }
.mv-fs-list { flex: 1; overflow-y: auto; padding: 0 10px 12px; scrollbar-width: thin; }
.mv-fs-row { display: grid; grid-template-columns: 22px minmax(0, 1fr) 84px 128px auto; align-items: center; gap: 10px; min-height: 42px; padding: 0 8px; border-radius: 12px; }
.mv-fs-row:hover { background: var(--raised-hover, rgba(255,255,255,.05)); }
.mv-fs-row .ic { color: var(--knob-off, #aaa); display: grid; }
.mv-fs-row .ic.dir { color: var(--accent, #3cc8dc); }
.mv-fs-row .nm { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; background: none; border: 0; color: inherit; font: inherit; text-align: left; padding: 0; }
.mv-fs-row .nm.dir { cursor: pointer; font-weight: 600; }
.mv-fs-row .nm.dir:hover { text-decoration: underline; }
.mv-fs-row .sz, .mv-fs-row .dt { color: var(--muted, #999); font-size: 12.5px; font-variant-numeric: tabular-nums; white-space: nowrap; }
.mv-fs-row .sz { text-align: right; }
.mv-fs-row .acts { display: flex; gap: 2px; }
.mv-fs-row .acts button { width: 32px; height: 32px; display: grid; place-items: center; background: none; border: 0; color: var(--knob-off, #aaa); border-radius: 999px; cursor: pointer; }
.mv-fs-row .acts button:hover:not(:disabled) { background: var(--raised, #333); color: var(--text, #fff); }
.mv-fs-row .acts button:disabled { opacity: .3; cursor: default; }
.mv-fs-empty { color: var(--muted, #999); padding: 18px 8px; }
.mv-fs-foot { flex: none; display: flex; justify-content: space-between; gap: 10px; padding: 10px 18px; border-top: 1px solid var(--divider, #333); color: var(--muted, #999); font-size: 12px; }
.mv-fs-foot [data-kind="bad"] { color: #FF7A7A; }
.mv-fs-prog { height: 3px; background: var(--accent, #3cc8dc); width: 0; transition: width .2s; }
.mv-fs.drop .mv-fs-list { outline: 2px dashed var(--accent, #3cc8dc); outline-offset: -6px; border-radius: 16px; }
@media (max-width: 640px) {
  .mv-fs { top: 0; right: 0; bottom: 0; width: 100vw; border-radius: 0; }
  .mv-fs-row { grid-template-columns: 22px minmax(0, 1fr) auto; }
  .mv-fs-row .dt { display: none; }
  .mv-fs-row .sz { grid-column: 2; grid-row: 2; text-align: left; margin-top: -8px; padding-bottom: 6px; }
}
`;
const enc = (p) => encodeURIComponent(p);
export const size = (n) => (n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : n >= 1e6 ? `${(n / 1e6).toFixed(1)} MB` : n >= 1e3 ? `${Math.round(n / 1e3)} KB` : `${n} B`);
const date = (t) => { const d = new Date(t * 1000); return isNaN(d) ? "" : `${d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" })} ${d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" })}`; };
const join = (dir, name) => (dir ? `${dir}/${name}` : name);
// the name a file takes on the Move: what the host refuses in a new name (src/files.h) replaced
export function safeName(name) {
  let n = String(name || "").replace(/[\u0000-\u001f\u007f\\/:*?"<>|]/g, "_").replace(/^[.\s]+/, "").replace(/[\s.]+$/, "");
  return n.slice(0, 200) || null;
}
// a download by the browser (the host answers with an attachment)
export function download(url) {
  const a = h("a", { href: url, download: "", hidden: true });
  document.body.append(a); a.click(); a.remove();
}
export const zipUrl = (path) => `${API}zip?path=${enc(path)}`;

// one upload (XHR: its progress), into dir: true, or the host's reason
export function upload(dir, file, name, onProgress, replace = false) {
  return new Promise((resolve) => {
    const x = new XMLHttpRequest();
    x.open("POST", `${API}put?path=${enc(dir)}&name=${enc(name)}${replace ? "&replace=1" : ""}`);
    x.upload.onprogress = (ev) => { if (ev.lengthComputable && onProgress) onProgress(ev.loaded / ev.total); };
    x.onload = () => { let m = ""; try { m = JSON.parse(x.responseText).error || ""; } catch (_) { /* (no JSON) */ }
      resolve(x.status >= 200 && x.status < 300 ? true : { status: x.status, why: m || `error ${x.status}` }); };
    x.onerror = () => resolve({ status: 0, why: "the upload failed" });
    x.send(file);
  });
}

export class FilesView {
  constructor({ toast }) {
    // (the page's toast is under this panel: what is said also shows in the panel's foot)
    this.toast = (m, kind) => { this.msg = m; this.msgKind = kind || ""; this.status.textContent = m; this.status.dataset.kind = this.msgKind; if (toast) toast(m, kind); };
    this.path = ""; this.data = null; this.busy = ""; this.msg = ""; this.isOpen = false;
    if (!document.getElementById("mv-fs-css")) document.head.append(h("style", { id: "mv-fs-css" }, CSS));
    this.scrim = h("div", { class: "mv-fs-scrim", hidden: true, onclick: () => this.close() });
    this.pick = h("input", { type: "file", multiple: true, hidden: true, onchange: () => { this.put([...this.pick.files]); this.pick.value = ""; } });
    this.crumbs = h("nav", { class: "mv-fs-crumbs", "aria-label": "Folder" });
    this.ro = h("div", { class: "mv-fs-ro", hidden: true });
    this.list = h("div", { class: "mv-fs-list", role: "list", "aria-label": "Files on the Move" });
    this.prog = h("div", { class: "mv-fs-prog" });
    this.info = h("span", {}); this.status = h("span", { role: "status" });
    const pill = (cls, ic, text, on, title) => h("button", { class: `pill ${cls}`, onclick: on, title }, svg(ic, 14), text);
    this.upBtn = pill("primary mv-fs-up", IC.up, "Upload", () => this.pick.click(), "Upload files into this folder (or drop them here)");
    this.dirBtn = pill("mv-fs-new", IC.plus, "New folder", () => this.mkdir());
    this.zipBtn = pill("mv-fs-zip", IC.zip, "Download folder", () => download(zipUrl(this.path)), "This folder and everything in it as a .zip");
    this.el = h("section", { class: "mv-fs", id: "mv-fs", role: "dialog", "aria-modal": "true", "aria-label": "Files on the Move", tabindex: "-1", hidden: true },
      h("div", { class: "mv-fs-head" }, svg(IC.files, 18), h("b", {}, "Files"), h("span", { class: "sub" }, "the RemoteFM1 folder on the Move"),
        h("span", { class: "sp" }), h("button", { class: "mv-fs-x", "aria-label": "Close Files", onclick: () => this.close() }, svg(IC.close, 16))),
      h("div", { class: "mv-fs-bar" }, this.upBtn, this.dirBtn, this.zipBtn,
        pill("mv-fs-exp-proj", IC.down, "Export all projects", () => download(zipUrl("move/projects")), "move/projects as a .zip: every sequencer project"),
        pill("mv-fs-exp-states", IC.down, "Export states", () => download(zipUrl("states")), "states/ as a .zip: every firmware's flash and saved states"),
        this.pick),
      this.crumbs, this.ro, this.prog, this.list, h("div", { class: "mv-fs-foot" }, this.info, this.status));
    document.body.append(this.scrim, this.el);
    // drag and drop (desktop): files dropped on the panel go into the folder shown
    let depth = 0;
    this.el.addEventListener("dragenter", (e) => { if (e.dataTransfer && [...e.dataTransfer.types].includes("Files")) { e.preventDefault(); depth++; this.el.classList.add("drop"); } });
    this.el.addEventListener("dragover", (e) => { if (e.dataTransfer && [...e.dataTransfer.types].includes("Files")) e.preventDefault(); });
    this.el.addEventListener("dragleave", () => { if (--depth <= 0) { depth = 0; this.el.classList.remove("drop"); } });
    this.el.addEventListener("drop", (e) => { e.preventDefault(); depth = 0; this.el.classList.remove("drop"); if (e.dataTransfer) this.put([...e.dataTransfer.files]); });
    this.el.addEventListener("keydown", (e) => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); this.close(); } });
  }

  open(path = this.path) { this.isOpen = true; this.el.hidden = this.scrim.hidden = false; this.el.focus(); this.go(path); }
  close() { this.isOpen = false; this.el.hidden = this.scrim.hidden = true; }
  toggle() { if (this.isOpen) this.close(); else this.open(); }

  async go(path) {
    if (path !== this.path) this.msg = "";
    this.path = path;
    try {
      const r = await fetch(`${API}list?path=${enc(path)}`, { cache: "no-store" });
      const j = await r.json();
      if (!r.ok) throw new Error(j.error || `error ${r.status}`);
      if (this.path !== path) return;
      this.data = j;
    } catch (e) {
      this.toast(`Files: ${e.message || e}`, "bad");
      if (path) { this.go(path.includes("/") ? path.slice(0, path.lastIndexOf("/")) : ""); return; }
      this.data = { entries: [], writable: false, why: "", free: 0 };
    }
    this.render();
  }

  render() {
    const d = this.data || { entries: [] }, path = this.path;
    const parts = path ? path.split("/") : [];
    const crumbs = [h("button", { "aria-current": parts.length ? null : "location", onclick: () => this.go("") }, "RemoteFM1")];
    parts.forEach((p, i) => {
      const to = parts.slice(0, i + 1).join("/");
      crumbs.push(h("span", { class: "sep" }, "/"), h("button", { "aria-current": i === parts.length - 1 ? "location" : null, onclick: () => this.go(to) }, p));
    });
    this.crumbs.replaceChildren(...crumbs);
    this.ro.hidden = !!d.writable || !d.why;
    this.ro.replaceChildren(svg(IC.lock, 14), h("span", {}, `Read-only: ${d.why || ""}`));
    this.upBtn.disabled = this.dirBtn.disabled = !d.writable || !!this.busy;
    const busy = Array.isArray(d.busy) ? d.busy.filter(Boolean) : [];
    const lockedHere = (name) => {   // (what the host would refuse: told before trying; the host decides)
      const rel = join(path, name), m = /^(states|data|code)\/([^/]+)/.exec(rel);
      if (!d.writable) return d.why || "read-only";
            if (["audio", "code", "data", "states", "move"].includes(rel) || rel === "move/projects") return "a folder the Move needs";
      if (m && busy.includes(m[2])) return `${m[2]} runs in a slot now: unload it first`;
      const pm = /^move\/projects\/move-project-(\d\d)\./.exec(rel);
      if (pm && +pm[1] === d.open_project + 1) return "the open project on the Move";
      return null;
    };
    const rows = d.entries.map((e) => {
      const rel = join(path, e.name), lock = lockedHere(e.name);
      return h("div", { class: "mv-fs-row", role: "listitem", "data-name": e.name },
        h("span", { class: `ic${e.dir ? " dir" : ""}` }, svg(e.dir ? IC.folder : IC.file, 16)),
        e.dir ? h("button", { class: "nm dir", title: e.name, onclick: () => this.go(rel) }, e.name) : h("span", { class: "nm", title: e.name }, e.name),
        h("span", { class: "sz" }, e.dir ? "folder" : size(e.size)),
        h("span", { class: "dt" }, date(e.mtime)),
        h("span", { class: "acts" },
          h("button", { "aria-label": `${e.dir ? "Download as .zip" : "Download"} ${e.name}`, title: e.dir ? "Download as .zip" : "Download",
            onclick: () => download(e.dir ? zipUrl(rel) : `${API}get?path=${enc(rel)}`) }, svg(e.dir ? IC.zip : IC.down, 15)),
          h("button", { "aria-label": `Rename ${e.name}`, title: lock || "Rename", disabled: !!lock, onclick: () => this.rename(e) }, svg(IC.pen, 15)),
          h("button", { "aria-label": `Delete ${e.name}`, title: lock || "Delete", disabled: !!lock, onclick: () => this.del(e) }, svg(IC.trash, 15))));
    });
    this.list.replaceChildren(...(rows.length ? rows : [h("div", { class: "mv-fs-empty" }, d.writable ? "Nothing here yet: upload files, or drop them here." : "Nothing here.")]));
    const n = d.entries.length;
    this.info.textContent = `${n} item${n === 1 ? "" : "s"}${d.free ? ` · ${size(d.free)} free on the Move` : ""}`;
    this.status.textContent = this.busy || this.msg;
  }

  async call(what, q, ok) {
    const r = await fetch(`${API}${what}?${q}`, { method: "POST" }).catch(() => null);
    let j = {};
    try { j = r ? await r.json() : {}; } catch (_) { /* (no JSON) */ }
    if (r && r.ok) { if (ok) this.toast(ok); return { ok: true }; }
    return { ok: false, status: r ? r.status : 0, why: j.error || "the Move did not answer" };
  }
  async mkdir() {
    const raw = prompt("New folder's name:", "New folder");
    if (raw == null) return;
    const name = safeName(raw);
    if (!name) { this.toast("That name won't do.", "bad"); return; }
    const r = await this.call("mkdir", `path=${enc(join(this.path, name))}`);
    if (!r.ok) this.toast(`${name}: ${r.why}`, "bad");
    await this.go(this.path);
  }
  async rename(e) {
    const raw = prompt(`Rename ${e.name} to:`, e.name);
    if (raw == null || raw === e.name) return;
    const name = safeName(raw);
    if (!name) { this.toast("That name won't do.", "bad"); return; }
    const r = await this.call("rename", `path=${enc(join(this.path, e.name))}&to=${enc(join(this.path, name))}`);
    if (!r.ok) this.toast(`${e.name}: ${r.why}`, "bad");
    await this.go(this.path);
  }
  async del(e) {
    const rel = join(this.path, e.name);
    let n = 0;
    if (e.dir) {   // (a folder with things in it: asked twice, then all of it)
      try { n = ((await (await fetch(`${API}list?path=${enc(rel)}`, { cache: "no-store" })).json()).entries || []).length; } catch (_) { n = 0; }
    }
    if (!confirm(`Delete ${e.dir ? "the folder " : ""}${e.name} from the Move?`)) return;
    const all = () => confirm(`${e.name} has ${n ? `${n} item${n === 1 ? "" : "s"}` : "files"} in it. Delete it with everything in it? This can't be undone.`);
    if (n && !all()) return;
    let r = await this.call("delete", `path=${enc(rel)}${n ? `&recursive=1&confirm=${enc(e.name)}` : ""}`, `${e.name} deleted.`);
    if (!r.ok && r.status === 409 && e.dir && !n && all())   // (only hidden files in it)
      r = await this.call("delete", `path=${enc(rel)}&recursive=1&confirm=${enc(e.name)}`, `${e.name} deleted.`);
    if (!r.ok) this.toast(`${e.name}: ${r.why}`, "bad");
    await this.go(this.path);
  }
  async put(files, dir = this.path) {
    for (const f of files) {
      const name = safeName(f.name);
      if (!name) { this.toast(`${f.name}: that name won't do.`, "bad"); continue; }
      this.busy = `Sending ${name}…`; this.render();
      let r = await upload(dir, f, name, (x) => { this.prog.style.width = `${(100 * x).toFixed(1)}%`; });
      if (r !== true && r.status === 409 && confirm(`${name} is there already. Replace it?`))
        r = await upload(dir, f, name, (x) => { this.prog.style.width = `${(100 * x).toFixed(1)}%`; }, true);
      this.prog.style.width = "0";
      if (r === true) this.toast(`${name} is on the Move.`); else this.toast(`${name}: ${r.why}`, "bad");
    }
    this.busy = "";
    await this.go(this.path);
  }
}
