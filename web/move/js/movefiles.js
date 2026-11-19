// SPDX-License-Identifier: GPL-3.0-only
// The settings sheet's Files section on RemoteFM1's page (RemoteFM14Move): the RemoteFM1 folder on the Move one click
// away. "Add samples" puts WAV / AIFF files into audio/ (the samples every Fantasy CFW reads: FM1VST
// docs/VIRTUAL-CARTS.md section 3); "Browse files" opens the file browser (js/movefs.js FilesView, also the header's
// Files button); "Export all projects" and "Export states" download move/projects and states/ as .zip files. Over the
// host's API (src/web.c /api/fs). Nothing here runs unless js/host.js ON_MOVE (js/moveslots.js mountMoveUi mounts it).
import { h, svg } from "./moveui.js";
import { FilesView, download, zipUrl, upload, safeName, size } from "./movefs.js";

const CSS = `
.mv-files { display: flex; flex-direction: column; gap: 8px; }
.mv-files .pills { margin-top: 2px; }
.mv-files .pill { display: inline-flex; align-items: center; justify-content: center; gap: 6px; }
.mv-files .bar { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; font-size: 13px; }
.mv-files .prog { height: 3px; background: var(--accent, #3cc8dc); width: 0; transition: width .2s; border-radius: 2px; }
`;
const IC = { up: '<path d="M8 11V2M4.5 5.5L8 2l3.5 3.5M3 14h10"/>', down: '<path d="M8 2v9M4.5 7.5L8 11l3.5-3.5M3 14h10"/>',
             files: '<path d="M2 3.5h4.5l1.3 1.5H14v8H2z"/>' };
const AUDIO = /\.(wav|aif|aiff)$/i;

export function mountMoveFiles({ sheet, toast }) {
  if (!document.getElementById("mv-files-css")) document.head.append(h("style", { id: "mv-files-css" }, CSS));
  const view = new FilesView({ toast });
  const st = { n: null, free: 0, busy: "" };
  const info = h("span", { class: "muted" });
  const prog = h("div", { class: "prog" });
  const pick = h("input", { type: "file", accept: ".wav,.aif,.aiff,audio/wav,audio/x-wav,audio/aiff", multiple: true, hidden: true,
                            onchange: () => { add([...pick.files]); pick.value = ""; } });
  const pill = (cls, ic, text, on, title) => h("button", { class: `pill ${cls}`, onclick: on, title }, svg(ic, 14), text);
  const addBtn = pill("primary mv-add-samples", IC.up, "Add samples", () => pick.click(), "WAV or AIFF files into audio/: the samples Fantasy CFWs such as Elasticat read");
  const box = h("div", { class: "mv-files" },
    h("p", { class: "note" }, "The RemoteFM1 folder on the Move: firmwares, their states, the sequencer's projects, and the samples and files of Fantasy CFWs such as Elasticat."),
    h("div", { class: "pills two" }, addBtn,
      pill("mv-browse", IC.files, "Browse files", () => { sheet.close(); view.open(""); }, "The file browser: download, upload, rename, delete")),
    h("div", { class: "pills two" },
      pill("mv-exp-proj", IC.down, "Export all projects", () => download(zipUrl("move/projects")), "move/projects as a .zip"),
      pill("mv-exp-states", IC.down, "Export states", () => download(zipUrl("states")), "states/ as a .zip: every firmware's flash and saved states")),
    prog, h("div", { class: "bar" }, info, pick));

  function render() {
    info.textContent = st.busy || (st.n == null ? "…" : `audio/: ${st.n} sample${st.n === 1 ? "" : "s"} · ${size(st.free)} free`);
    addBtn.disabled = !!st.busy;
  }
  async function refresh() {
    try {
      const j = await (await fetch("api/fs/list?path=audio", { cache: "no-store" })).json();
      st.n = (j.entries || []).filter((e) => !e.dir).length; st.free = j.free || 0;
    } catch (_) { st.n = null; }
    render();
  }
  async function add(files) {
    for (const f of files) {
      const name = safeName(f.name);
      if (!name || !AUDIO.test(name)) { toast(`${f.name}: only WAV and AIFF files.`, "bad"); continue; }
      st.busy = `Sending ${name}…`; render();
      let r = await upload("audio", f, name, (x) => { prog.style.width = `${(100 * x).toFixed(1)}%`; });
      if (r !== true && r.status === 409 && confirm(`${name} is in audio/ already. Replace it?`))
        r = await upload("audio", f, name, (x) => { prog.style.width = `${(100 * x).toFixed(1)}%`; }, true);
      prog.style.width = "0";
      if (r === true) toast(`${name} is on the Move.`); else toast(`${name}: ${r.why}`, "bad");
    }
    st.busy = "";
    await refresh();
    if (view.isOpen) view.go(view.path);
  }

  // the settings sheet's main page: "Files" before Look (the sheet is RemoteFM1's own: js/sheet.js)
  const mainPage = sheet.mainPage.bind(sheet);
  let shown = false;
  sheet.mainPage = () => {
    const out = mainPage();
    const at = out.findIndex((e) => e && e.tagName === "H3" && /^Look/.test(e.textContent));
    out.splice(at >= 0 ? at : out.length, 0, h("h3", { class: "heading" }, "Files"), box);
    if (!shown) { shown = true; refresh(); }
    return out;
  };
  const open = sheet.open.bind(sheet);
  sheet.open = (...a) => { shown = false; return open(...a); };   // (read again each time the sheet opens)
  render();
  return { refresh, view };
}
