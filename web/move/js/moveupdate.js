// SPDX-License-Identifier: GPL-3.0-only
// The settings sheet's Update section on RemoteFM1's page (RemoteFM14Move): a signed .rfm1u (tools/make_update.py)
// sent to the Move, checked and installed there (src/update.h: the signature, a backup of the RemoteFM1 folder's
// move/, the module's folder swapped with the old one kept), then "relaunch RemoteFM1 on the Move". Undo puts the
// previous build back. Over the host's API: GET /api/update (the state), POST /api/update (the file as the body),
// POST /api/update/undo. The picker takes any file (Safari on iPadOS greys out an extension it does not know when
// accept names it): the name and the file's own header are checked here first. Nothing here runs unless js/host.js
// ON_MOVE (js/moveslots.js mountMoveUi mounts it).
import { h, svg } from "./moveui.js";

const API = "api/update";
const CSS = `
.mv-upd { display: flex; flex-direction: column; gap: 8px; }
.mv-upd .pills { margin-top: 2px; }
.mv-upd .pill { display: inline-flex; align-items: center; justify-content: center; gap: 6px; }
.mv-upd .pill[hidden] { display: none; }
.mv-upd .ver { font-size: 13px; display: flex; gap: 8px; align-items: baseline; flex-wrap: wrap; }
.mv-upd .ver b { font-weight: 650; }
.mv-upd .ver code { font: 12px/1 var(--mono, ui-monospace, Menlo, monospace); color: var(--muted, #999); }
.mv-upd .prog { height: 3px; background: var(--accent, #3cc8dc); width: 0; transition: width .2s; border-radius: 2px; }
.mv-upd .st { font-size: 13px; min-height: 18px; }
.mv-upd .st.bad { color: #ff6b5c; }
.mv-upd .st.good { color: var(--accent, #3cc8dc); font-weight: 600; }
`;
const IC = { up: '<path d="M8 11V2M4.5 5.5L8 2l3.5 3.5M3 14h10"/>', undo: '<path d="M5 3L2 6l3 3M2 6h7.5a4 4 0 010 8H6"/>' };
const BUSY = new Set(["receiving", "checking", "installing", "undoing"]);
const short = (c) => String(c || "").slice(0, 12);
const appOf = (id) => (!id || id === "remotefm1" ? "RemoteFM1" : id.charAt(0).toUpperCase() + id.slice(1));

// the file's own header (src/update.h): "RFM1UPD\0", u32 format, u32 manifest bytes, u64 payload bytes, the manifest
async function readManifest(file) {
  const buf = await file.slice(0, 24).arrayBuffer();
  if (buf.byteLength < 24 || String.fromCharCode(...new Uint8Array(buf, 0, 8)) !== "RFM1UPD\0") return null;
  const head = new DataView(buf);
  const mlen = head.getUint32(12, true);
  if (head.getUint32(8, true) !== 1 || mlen < 2 || mlen > (1 << 20)) return null;
  try { return JSON.parse(await file.slice(24, 24 + mlen).text()); } catch (_) { return null; }
}

export function mountMoveUpdate({ sheet, toast }) {
  if (!document.getElementById("mv-upd-css")) document.head.append(h("style", { id: "mv-upd-css" }, CSS));
  let st = null, sending = 0, timer = 0, note = null;   // note: this page's own word (a refusal before sending)
  const ver = h("div", { class: "ver" });
  const prog = h("div", { class: "prog" });
  const line = h("div", { class: "st" });
  const pick = h("input", { type: "file", hidden: true, onchange: () => { const f = pick.files[0]; pick.value = ""; if (f) send(f); } });
  const pill = (cls, ic, text, on) => h("button", { class: `pill ${cls}`, onclick: on }, svg(ic, 14), text);
  const updBtn = pill("primary mv-upd-pick", IC.up, "Update", () => pick.click());
  const undoBtn = pill("mv-upd-undo", IC.undo, "Undo last update", () => undo());
  const box = h("div", { class: "mv-upd" }, ver, h("div", { class: "pills two" }, updBtn, undoBtn), prog, line, pick);

  function render() {
    const busy = sending > 0 || (st && BUSY.has(st.state));
    ver.replaceChildren();
    if (st && st.running) {
      ver.append(h("b", {}, st.running.version), h("code", {}, short(st.running.commit)));
      const inst = st.installed;
      if (inst && inst.version && (inst.version !== st.running.version || (inst.commit && short(inst.commit) !== short(st.running.commit))))
        ver.append(h("span", { class: "muted" }, "→"), h("b", {}, inst.version), h("code", {}, short(inst.commit)));
    }
    updBtn.disabled = busy || !st || !st.can;
    undoBtn.hidden = !(st && st.undo);
    undoBtn.disabled = busy;
    if (st && st.undo && st.undo_to) undoBtn.title = `${appOf(st.undo_to.app)} back to ${st.undo_to.version || "its previous build"}`;
    line.className = "st";
    if (note) { line.textContent = note; line.classList.add("bad"); }
    else if (sending > 0) line.textContent = `Sending ${Math.round(sending * 100)} %`;
    else if (!st) line.textContent = "";
    else if (!st.can) { line.textContent = st.why; line.classList.add("bad"); }
    else if (st.state === "failed") { line.textContent = st.msg; line.classList.add("bad"); }
    else if (st.state === "done") { line.textContent = st.msg; line.classList.add("good"); }
    else line.textContent = BUSY.has(st.state) ? `${st.msg}…` : "";
    if (!sending) prog.style.width = st && BUSY.has(st.state) && st.state !== "receiving" ? "100%" : "0";
  }
  async function refresh() {
    try { st = await (await fetch(API, { cache: "no-store" })).json(); } catch (_) { /* (the Move gone: as it was) */ }
    render();
    clearTimeout(timer);
    if (st && BUSY.has(st.state)) timer = setTimeout(refresh, 600);
  }
  async function send(file) {
    note = null;
    if (!/\.rfm1u$/i.test(file.name)) { note = `${file.name}: not an update file (.rfm1u)`; render(); return; }
    const man = await readManifest(file);
    if (!man || !man.version) { note = `${file.name}: not an update file (.rfm1u)`; render(); return; }
    if (!confirm(`Install ${man.name || "RemoteFM1"} ${man.version}${man.host_commit && man.host_commit !== "unknown" ? ` (${short(man.host_commit)})` : ""} on the Move?`)) return;
    sending = 0.001; render();
    const r = await new Promise((resolve) => {
      const x = new XMLHttpRequest();
      x.open("POST", API);
      x.upload.onprogress = (ev) => { if (ev.lengthComputable) { sending = Math.max(0.001, ev.loaded / ev.total); prog.style.width = `${(100 * sending).toFixed(1)}%`; render(); } };
      x.onload = () => { let m = ""; try { m = JSON.parse(x.responseText).error || ""; } catch (_) { /* (no JSON) */ }
        resolve(x.status >= 200 && x.status < 300 ? true : m || `error ${x.status}`); };
      x.onerror = () => resolve("the upload failed");
      x.send(file);
    });
    sending = 0;
    prog.style.width = "0";
    if (r !== true) { note = r; toast(`Update: ${r}`, "bad"); }
    await refresh();
  }
  async function undo() {
    const to = st && st.undo_to && st.undo_to.version ? st.undo_to.version : "the previous build";
    if (!confirm(`Put ${appOf(st && st.undo_to && st.undo_to.app)} back to ${to}?`)) return;
    note = null;
    try {
      const x = await fetch(`${API}/undo`, { method: "POST" });
      if (!x.ok) { let m = ""; try { m = (await x.json()).error || ""; } catch (_) { /* (no JSON) */ } note = m || `error ${x.status}`; }
    } catch (_) { note = "the Move did not answer"; }
    await refresh();
  }

  // the settings sheet's main page: "Update" after Files, before Look (the sheet is RemoteFM1's own: js/sheet.js)
  const mainPage = sheet.mainPage.bind(sheet);
  sheet.mainPage = () => {
    const out = mainPage();
    const at = out.findIndex((e) => e && e.tagName === "H3" && /^Look/.test(e.textContent));
    out.splice(at >= 0 ? at : out.length, 0, h("h3", { class: "heading" }, "Update"), box);
    return out;
  };
  const open = sheet.open.bind(sheet);
  sheet.open = (...a) => { note = null; refresh(); return open(...a); };   // (read again each time the sheet opens)
  refresh();
  return { refresh };
}
