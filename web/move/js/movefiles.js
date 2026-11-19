// SPDX-License-Identifier: GPL-3.0-only
// The Move's audio folder on RemoteFM1's page (RemoteFM14Move): the WAVs a Fantasy CFW reads (Elasticat's samples:
// FM1VST docs/VIRTUAL-CARTS.md section 3, the RemoteFM1 folder's audio/ on the Move). A list in the settings sheet with
// an add and a remove, over the host's API (src/web.c): GET /api/files/audio, POST /api/files/audio/<name> (the file
// as the body), DELETE /api/files/audio/<name>. The full file browser comes later. Nothing here runs unless
// js/host.js ON_MOVE (js/moveslots.js mountMoveUi mounts it).
import { h, svg, IC } from "./moveui.js";

const API = "api/files/audio";
const CSS = `
.mv-files { display: flex; flex-direction: column; gap: 6px; }
.mv-files ul { list-style: none; margin: 0; padding: 0; display: flex; flex-direction: column; gap: 2px; max-height: 240px; overflow-y: auto; }
.mv-files li { display: flex; align-items: center; gap: 8px; min-height: 30px; padding: 0 2px; }
.mv-files li .nm { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mv-files li .sz { font-variant-numeric: tabular-nums; opacity: .7; font-size: 13px; }
.mv-files li button { background: none; border: 0; color: inherit; opacity: .7; cursor: pointer; padding: 6px; }
.mv-files li button:hover { opacity: 1; }
.mv-files .bar { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.mv-files .bar .muted { font-size: 13px; opacity: .7; }
.mv-files .prog { height: 3px; background: var(--accent, #3cc8dc); width: 0; transition: width .2s; border-radius: 2px; }
`;

const mb = (n) => (n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : n >= 1e6 ? `${(n / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(n / 1e3))} KB`);
// a name the Move takes (src/web.c name_ok): letters, digits, space _ - . , ( ) +, ending .wav, not hidden
export function wavName(name) {
  let n = String(name || "").normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[^A-Za-z0-9 _\-.,()+]/g, "_");
  n = n.replace(/\.\.+/g, ".").replace(/^[. ]+/, "");
  if (!/\.wav$/i.test(n)) return null;
  if (n.length > 96) n = n.slice(0, 92) + ".wav";
  return n.length >= 5 ? n : null;
}

export function mountMoveFiles({ sheet, toast }) {
  if (!document.getElementById("mv-files-css")) document.head.append(h("style", { id: "mv-files-css" }, CSS));
  const st = { files: null, free: 0, max: 0, busy: "", err: "" };
  const list = h("ul", { "aria-label": "Audio files on the Move" });
  const info = h("span", { class: "muted" });
  const prog = h("div", { class: "prog" });
  const pick = h("input", { type: "file", accept: ".wav,audio/wav,audio/x-wav", multiple: true, hidden: true, onchange: () => { add([...pick.files]); pick.value = ""; } });
  const addBtn = h("button", { class: "pill", onclick: () => pick.click() }, "Add WAV files");
  const box = h("div", { class: "mv-files" },
    h("p", { class: "note" }, "Samples for Fantasy CFWs such as Elasticat: the RemoteFM1 folder's audio/ on the Move."),
    list, prog, h("div", { class: "bar" }, addBtn, info, pick));

  function render() {
    list.replaceChildren(...(st.files || []).map((f) => h("li", {},
      h("span", { class: "nm", title: f.name }, f.dir ? `${f.name}/` : f.name),
      h("span", { class: "sz" }, f.dir ? "folder" : mb(f.size)),
      f.dir ? null : h("button", { "aria-label": `Delete ${f.name}`, title: "Delete", onclick: () => del(f.name) }, svg(IC.trash, 14)))));
    if (st.files && !st.files.length) list.append(h("li", { class: "muted" }, "No samples yet."));
    info.textContent = st.busy || st.err || (st.files ? `${st.files.length} item${st.files.length === 1 ? "" : "s"} · ${mb(st.free)} free` : "…");
    addBtn.disabled = !!st.busy;
  }
  async function refresh() {
    try {
      const r = await fetch(API, { cache: "no-store" });
      const j = await r.json();
      Object.assign(st, { files: j.files || [], free: j.free || 0, max: j.max || 0, err: "" });
    } catch (e) { st.err = "Can't read the Move's audio folder."; }
    render();
  }
  async function add(files) {
    for (const f of files) {
      const name = wavName(f.name);
      if (!name) { toast(`${f.name}: only WAV files.`, "bad"); continue; }
      if (st.max && f.size > st.max) { toast(`${f.name}: at most ${mb(st.max)} a file.`, "bad"); continue; }
      st.busy = `Sending ${name}…`; render();
      try {
        const ok = await new Promise((resolve) => {   // (XHR: its upload progress)
          const x = new XMLHttpRequest();
          x.open("POST", `${API}/${encodeURIComponent(name)}`);
          x.upload.onprogress = (ev) => { if (ev.lengthComputable) prog.style.width = `${(100 * ev.loaded / ev.total).toFixed(1)}%`; };
          x.onload = () => { let m = ""; try { m = JSON.parse(x.responseText).error || ""; } catch (_) { /* (no JSON) */ }
            resolve(x.status >= 200 && x.status < 300 ? true : m || `error ${x.status}`); };
          x.onerror = () => resolve("the upload failed");
          x.send(f);
        });
        if (ok !== true) toast(`${name}: ${ok}`, "bad"); else toast(`${name} is on the Move.`);
      } finally { prog.style.width = "0"; }
    }
    st.busy = "";
    await refresh();
  }
  async function del(name) {
    if (!confirm(`Delete ${name} from the Move?`)) return;
    const r = await fetch(`${API}/${encodeURIComponent(name)}`, { method: "DELETE" }).catch(() => null);
    if (!r || !r.ok) toast(`${name}: can't delete it.`, "bad");
    await refresh();
  }

  // the settings sheet's main page: "Audio files" before Look (the sheet is RemoteFM1's own: js/sheet.js)
  const mainPage = sheet.mainPage.bind(sheet);
  let shown = false;
  sheet.mainPage = () => {
    const out = mainPage();
    const at = out.findIndex((e) => e && e.tagName === "H3" && /^Look/.test(e.textContent));
    const part = [h("h3", { class: "heading" }, "Audio files"), box];
    out.splice(at >= 0 ? at : out.length, 0, ...part);
    if (!shown || !st.files) { shown = true; refresh(); }
    return out;
  };
  const open = sheet.open.bind(sheet);
  sheet.open = (...a) => { shown = false; return open(...a); };   // (the list read again each time the sheet opens)
  render();
  return { refresh };
}
