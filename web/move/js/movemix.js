// SPDX-License-Identifier: GPL-3.0-only
// The Mix & FX view on RemoteFM1's page served by an Ableton Move (RemoteFM14Move; only when js/host.js ON_MOVE): the
// Move's FX setup (rfm1cs-abletonmove 0.5.0: one layer, then the Mixer) over the faceplate (the header and its slot
// chips stay):
//   T1-3 FX setup  the chain as the Move's pads show it: column 1 the MIDI FX slots (in series), column 3 the firmwares
//                  (each with its 16-track filter), column 5 the audio FX slots (send buses). The selected slot below:
//                  an FX slot's module and its own parameter pages (Schwung's ui_hierarchy, as the host planned them:
//                  8 knob cells a page, gaps as dashes, the pages as tabs), the track a MIDI FX acts on or the sends
//                  into an audio FX; a firmware's filter and its knobs (level, pan, sends 1..3)
//   T4 Mixer       a strip per firmware (tracks, 3 sends, pan, level, meter), the 3 FX returns, the master
// The selection follows the Move's (mix.page: the column, mix.focus: the slot; mix.afx[n].pg: a slot's page) and sets
// it (the PAGE op with the row, the FXPG op). Every edit is one of the host's ops (moveui.js OP: the same core path as
// the Move's own knobs; parameters by index, FXPAR); what shows is the core's state as /ws reports it.
// Desktop: a panel that slides over the faceplate, dials and faders. Phone: a full-screen dark sheet, bar sliders.
import { NSLOT, OP, COLS, MIXP, UNSET, FX_COLOR, MFX_COLOR, h, svg, IC, panText, levelText, tracksText, mfxText, paramText, wetHint,
  shortName, modPages, openPop, closePop, popItem, dial, fader, bar, segs, choice, meter } from "./moveui.js";

const RUN = "#3BD47A", BUSY = "#F5A524", ERR = "#FF5F57";
const TABS = [["FX setup", "T1-3"], ["Mixer", "T4"]];

export class MixView {
  constructor({ store, minis, ui, phone }) {
    this.store = store; this.minis = minis; this.ui = ui; this.phoneMq = phone;
    this.isOpen = false;
    this.tab = 1;                            // 0 FX setup, 1 the Mixer
    this.col = 2; this.row = 0;              // the slot selected in FX setup: column (0 MIDI FX, 1 firmwares, 2 audio FX), row
    this.lastMove = null;                    // the Move's selection last followed
    this.pg = [[0, 0, 0], [0, 0, 0]];        // each FX slot's page shown, and the Move's last
    this.lastPg = [[-1, -1, -1], [-1, -1, -1]];
    this.ups = []; this.meterUps = []; this.key = "";
    this.el = h("section", { class: "mv-view", id: "mv-view", "aria-label": "Mix and FX", "aria-hidden": "true", inert: true });
    document.body.append(this.el);
    store.on((what) => { if (what === "meters") { if (this.isOpen) this.meters(); } else this.render(); });
    phone.addEventListener("change", () => { this.key = ""; this.render(); });
  }
  get ph() { return this.phoneMq.matches; }
  up(fn) { this.ups.push(fn); }
  meter(fn) { this.meterUps.push(fn); fn(); }
  meters() { for (const f of this.meterUps) f(); }

  open() { if (this.isOpen) return; this.isOpen = true; this.key = ""; this.render(); this.setOpen(); }
  close() { if (!this.isOpen) return; this.isOpen = false; closePop(); this.setOpen(); }
  toggle() { if (this.isOpen) this.close(); else this.open(); }
  setOpen() {
    this.el.classList.toggle("open", this.isOpen);
    this.el.toggleAttribute("inert", !this.isOpen);
    this.el.setAttribute("aria-hidden", this.isOpen ? "false" : "true");
    document.body.classList.toggle("mv-mix-open", this.isOpen);
    this.ui.mixBtn.classList.toggle("lit", this.isOpen);
    this.ui.mixBtn.setAttribute("aria-pressed", this.isOpen ? "true" : "false");
    if (this.isOpen) this.el.focus({ preventScroll: true });
  }
  // the Move's FX setup on the same column and slot (PAGE: a | (row + 1) << 2), or its Mixer
  sendSel() { this.store.edit(OP.PAGE, this.tab ? 3 : this.col | (this.row + 1) << 2, 0); }
  setTab(i) {
    if (i === this.tab) return;
    this.tab = i; closePop();
    this.sendSel();
    this.render();
  }
  select(col, row) {
    const same = !this.tab && col === this.col && row === this.row;
    this.tab = 0; this.col = col; this.row = row; closePop();
    if (!same) this.sendSel();
    this.render();
  }
  setPg(kind, j, i) {
    this.pg[kind][j] = i;
    this.store.edit(OP.FXPG, kind * 4 + j, i);
    this.render();
  }

  // a rebuild when what is laid out changed; else the values only
  render() {
    const st = this.store, m = st.mix;
    if (m) {
      const sig = `${m.page}/${m.focus}`;
      if (sig !== this.lastMove) {
        this.lastMove = sig;
        if (m.page === 3) this.tab = 1;
        else if (m.page >= 0 && m.page < 3) { this.tab = 0; this.col = m.page; this.row = Math.min(2, m.focus | 0); }
      }
      for (let k = 0; k < 2; k++) for (let j = 0; j < 3; j++) {
        const p = st.fx(k, j).pg | 0;
        if (p !== this.lastPg[k][j]) { this.lastPg[k][j] = p; this.pg[k][j] = p; }
      }
    }
    if (!this.isOpen) return;
    const key = JSON.stringify([this.tab, this.col, this.row, this.pg, this.ph, st.active, st.slots.map((s) => [s.on, s.id]), st.pending.map((p) => p && p.id),
      m && [m.mfx.map((x) => x.mod), m.afx.map((x) => x.mod)], st.lib.midi.length, st.lib.audio.length]);
    if (key !== this.key) { this.key = key; this.build(); }
    for (const f of this.ups) f();
    this.meters();
  }
  build() {
    this.ups = []; this.meterUps = [];
    const scroll = this.body && this.builtTab === this.tab ? this.body.scrollTop : 0;   // (a new tab: from its top)
    this.builtTab = this.tab;
    const tabs = h("div", { class: "mv-tabs", role: "tablist" }, TABS.map(([t, n], i) => h("button", { class: `mv-tab ${i === this.tab ? "on" : ""}`, role: "tab",
      "aria-selected": i === this.tab ? "true" : "false", "data-tab": i, onclick: () => this.setTab(i) }, t, this.ph ? null : h("span", { class: "mv-t" }, n))));
    const live = h("span", { class: "mv-live" });
    this.up(() => {
      const m = this.store.mix;
      live.hidden = !(m && m.fxopen);
      if (!m || !m.fxopen) return;
      live.textContent = m.page === 3 ? `The Move's Mixer: knobs 1-3 set ${MIXP[m.mixpar | 0] || "Level"}`
        : `The Move shows ${COLS[m.page] || "FX setup"} ${(m.focus | 0) + 1}${m.fxpages ? " · its pages" : ""}`;
    });
    const head = this.ph
      ? [h("div", { class: "mv-ph-top" }, h("b", { class: "mv-title" }, "Mix & FX"), live,
          h("button", { class: "mv-iconbtn mv-close", "aria-label": "Close Mix & FX", onclick: () => this.close() }, svg(IC.close, 16))), tabs]
      : [h("div", { class: "mv-bar-top" }, tabs, h("span", { class: "mv-mut mv-small" }, "The Move's FX setup and Mixer; either side follows the other."), live,
          h("span", { class: "mv-spacer" }), h("button", { class: "mv-pill mv-back", onclick: () => this.close() }, "Back to the faceplate", h("span", { class: "mv-val mv-mut" }, "Esc")))];
    this.body = h("div", { class: `mv-body mv-tab-${this.tab}` }, this.tab ? this.mixer() : this.chain());
    this.el.classList.toggle("phone", this.ph);
    this.el.replaceChildren(h("div", { class: "mv-head" }, head), this.body);
    this.el.tabIndex = -1;
    this.body.scrollTop = scroll;
  }

  // ---- bits
  mini(i, cls = "") { return h("span", { class: `mv-mini-w ${cls}` }, this.minis.canvas(i)); }
  text(fn, cls = "", tag = "span") { const e = h(tag, { class: cls }); this.up(() => { e.textContent = fn(); }); return e; }
  sendCtl(i, k, small) {
    const st = this.store, mod = () => st.module(1, k), off = () => st.fx(1, k).mod < 0;
    const o = { key: `send-${i}-${k}`, label: `${st.slot(i).name} send to FX ${k + 1}`, get: () => st.slot(i).sends[k] | 0, set: (v) => st.edit(OP.MIX, i * 8 + 2 + k, v),
      color: () => FX_COLOR[k], off };
    if (small) return bar(this, { ...o, cls: "thin", capText: () => st.slot(i).name, color: () => st.colorOf(i) });
    if (this.ph) return bar(this, { ...o, cls: "small", capText: () => (off() ? `FX${k + 1}` : shortName(mod())) });
    return dial(this, { ...o, under: this.text(() => (off() ? "empty" : st.modName(1, k)), "mv-mut mv-tiny mv-ell") });
  }
  panCtl(i) {
    const st = this.store, o = { key: `pan-${i}`, label: `${st.slot(i).name} pan`, get: () => st.slot(i).pan ?? 64, set: (v) => st.edit(OP.MIX, i * 8 + 1, v), fmt: panText, def: 64, color: () => "#A69C8A" };
    return this.ph ? bar(this, { ...o, cls: "small", caption: "Pan" }) : dial(this, { ...o, caption: "Pan" });
  }
  levelCtl(i) {
    const st = this.store, o = { key: `level-${i}`, label: `${st.slot(i).name} level`, get: () => st.slot(i).level | 0, set: (v) => st.edit(OP.MIX, i * 8, v), def: 100, fmt: levelText, color: () => st.colorOf(i) };
    return this.ph ? bar(this, { ...o, cls: "big", caption: "Level" }) : fader(this, o);
  }
  tracksPill(i) {
    const p = h("button", { class: "mv-pill mv-pill-s", title: "Its track filter (FX setup)", onclick: () => this.select(1, i) });
    this.up(() => { p.textContent = tracksText(this.store.slot(i).filt); });
    return p;
  }
  fxStatus(kind, j) {
    const e = h("span", { class: "mv-fxst" });
    this.up(() => {
      const x = this.store.fx(kind, j);
      e.textContent = x.err ? x.err : x.busy ? "Loading..." : x.mod >= 0 && x.cpu ? `${x.cpu.toFixed(1)} % CPU` : "";
      e.className = `mv-fxst ${x.err ? "err" : x.busy ? "busy" : ""}`;
    });
    return e;
  }
  fxDot(kind, j) {
    const d = h("span", { class: "mv-dot" });
    this.up(() => { const x = this.store.fx(kind, j); d.style.setProperty("--c", x.err ? ERR : x.busy ? BUSY : x.mod >= 0 && x.run ? RUN : x.mod >= 0 ? (kind ? FX_COLOR[j] : MFX_COLOR) : "#3A352D"); d.classList.toggle("pulse", !!x.busy); });
    return d;
  }
  // the 16 tracks as toggles: fw (a firmware's filter: any set) or a MIDI FX (one, or All)
  trackCells(get, onCell, color, cols = 16) {
    const cells = Array.from({ length: 16 }, (_, t) => h("button", { class: "mv-st", "data-track": t + 1, "aria-label": `Track ${t + 1}`, onclick: () => onCell(t) }, t + 1));
    this.up(() => { const m = get() & 0xffff; cells.forEach((c, t) => { const on = !!(m >> t & 1); c.classList.toggle("on", on); c.setAttribute("aria-pressed", on ? "true" : "false"); }); });
    return h("div", { class: `mv-steps c${cols}`, style: { "--c": color } }, cells);
  }
  fwCells(i, cols) {
    const st = this.store;
    return this.trackCells(() => st.slot(i).filt, (t) => st.edit(OP.ROUTE, i, (st.slot(i).filt ^ (1 << t)) & 0xffff), st.colorOf(i), cols);
  }
  mfxCells(j, cols) {
    const st = this.store;
    return this.trackCells(() => (mfxText(st.fx(0, j).filt) === "All" ? 0 : st.fx(0, j).filt),
      (t) => st.edit(OP.ROUTE, 3 + j, (st.fx(0, j).filt & 0xffff) === 1 << t ? 0xffff : 1 << t), MFX_COLOR, cols);
  }
  allBtn(j) {
    const b = h("button", { class: "mv-pill mv-all", onclick: () => this.store.edit(OP.ROUTE, 3 + j, 0xffff) }, "All");
    this.up(() => b.classList.toggle("on", mfxText(this.store.fx(0, j).filt) === "All"));
    return b;
  }
  modMenu(anchor, kind, j) {
    const st = this.store, list = kind ? st.lib.audio : st.lib.midi;
    openPop(anchor, () => [
      h("div", { class: "mv-h mv-pop-h" }, `${kind ? "FX" : "MIDI FX"} ${j + 1}: pick a module`),
      ...list.map((m) => popItem({ label: m.name, id: m.id, sub: `${m.builtin ? "built-in" : "installed"} ${m.version || ""}`, on: st.fx(kind, j).mod === m.i,
        onclick: () => { closePop(); st.edit(OP.FXLOAD, kind * 4 + j, m.i); } })),
      list.length ? null : h("p", { class: "mv-pop-note" }, "No modules installed."),
      h("div", { class: "mv-pop-foot" }, h("button", { class: "mv-pill", disabled: st.fx(kind, j).mod < 0, onclick: () => { closePop(); st.edit(OP.FXLOAD, kind * 4 + j, -1); } }, "Empty the slot")),
    ], { width: 280, cls: "mv-modmenu" });
  }
  modList(kind, j) {
    const st = this.store, list = kind ? st.lib.audio : st.lib.midi;
    const rows = list.map((m) => {
      const b = h("button", { class: "mv-modrow", "data-mod": m.i, onclick: () => st.edit(OP.FXLOAD, kind * 4 + j, m.i) }, h("b", {}, m.name), h("span", { class: "mv-val mv-mut" }, m.builtin ? "built-in" : "installed"));
      this.up(() => b.classList.toggle("on", st.fx(kind, j).mod === m.i));
      return b;
    });
    return h("div", { class: "mv-card mv-modlist" }, rows.length ? rows : h("p", { class: "mv-mut mv-small" }, "No modules installed on this Move."));
  }
  sendsFrom(k) {
    const st = this.store, l = [];
    for (let i = 0; i < NSLOT; i++) { const s = st.slot(i); if (s.on && s.sends[k]) l.push(`${s.name} ${s.sends[k]}`); }
    return l.length ? `From ${l.join(", ")}` : "No sends yet";
  }

  // ---- T4 Mixer (the owner's on the Move: each firmware's level, pan and sends)
  mixer() {
    const st = this.store, out = [];
    if (this.ph) {
      for (let i = 0; i < NSLOT; i++) {
        const s = st.slot(i);
        if (!s.on) { out.push(h("button", { class: "mv-card mv-empty mv-ph-emptyslot", onclick: () => { this.close(); this.ui.toggleMenu(true); this.ui.menuSub = i; this.ui.fillMenu(); } }, svg(IC.plus, 14), `Load into slot ${i + 1}`)); continue; }
        out.push(h("div", { class: `mv-card mv-ph-strip ${i === st.active ? "sel" : ""}`, style: { "--c": st.colorOf(i) }, "data-slot": i },
          h("div", { class: "mv-row" }, this.mini(i, "s28"), h("b", { class: "mv-name" }, s.name), this.tracksPill(i)),
          this.levelCtl(i),
          h("div", { class: "mv-grid4" }, this.panCtl(i), [0, 1, 2].map((k) => this.sendCtl(i, k)))));
      }
      out.push(h("div", { class: "mv-h mv-pad" }, "FX returns"));
      out.push(h("div", { class: "mv-grid3" }, [0, 1, 2].map((k) => {
        const n = this.text(() => st.modName(1, k) || "+ Load", "mv-ret-n", "b");
        const c = h("button", { class: "mv-card mv-ph-ret", style: { "--c": FX_COLOR[k] }, onclick: () => this.select(2, k) }, h("span", { class: "mv-h" }, `FX ${k + 1}`), n, meter(this, 3 + k, "h"));
        this.up(() => { c.classList.toggle("mv-empty", st.fx(1, k).mod < 0); });
        return c;
      })));
      out.push(h("div", { class: "mv-row mv-master-ph" }, h("span", { class: "mv-h" }, "Master"), meter(this, 6, "h grow"), h("span", { class: "mv-mut mv-small" }, "the Move's volume knob")));
      return h("div", { class: "mv-col" }, out);
    }
    for (let i = 0; i < NSLOT; i++) {
      const s = st.slot(i);
      if (!s.on) {
        const b = h("button", { class: "mv-pill", "aria-haspopup": "menu", onclick: () => this.ui.loadMenu(b, i) }, svg(IC.plus, 13), "Load a firmware");
        out.push(h("div", { class: "mv-card mv-strip mv-empty", "data-slot": i }, h("div", { class: "mv-h" }, `Slot ${i + 1}`),
          h("div", { class: "mv-empty-in" }, this.mini(i, "s40 none"), h("b", {}, st.pending[i] ? `${st.pending[i].name}...` : "Empty"), b)));
        continue;
      }
      out.push(h("div", { class: `mv-card mv-strip ${i === st.active ? "sel" : ""}`, style: { "--c": st.colorOf(i) }, "data-slot": i },
        h("span", { class: "mv-topbar" }),
        h("div", { class: "mv-row" }, this.mini(i, "s40"), h("div", { class: "mv-grow mv-min0" },
          h("div", { class: "mv-h mv-c" }, `Slot ${i + 1}${i === st.active ? " · playing" : ""}`), h("div", { class: "mv-name mv-ell" }, s.name))),
        h("div", { class: "mv-row mv-between" }, h("span", { class: "mv-h" }, "Tracks"), this.tracksPill(i)),
        h("div", { class: "mv-h" }, "Sends"),
        h("div", { class: "mv-grid3 mv-sends" }, [0, 1, 2].map((k) => this.sendCtl(i, k))),
        h("div", { class: "mv-strip-bot" }, this.panCtl(i), this.levelCtl(i), meter(this, i))));
    }
    out.push(h("div", { class: "mv-sep" }));
    for (let k = 0; k < 3; k++) {
      const n = this.text(() => st.modName(1, k) || "Empty", "mv-ret-n", "div");
      const sub = this.text(() => {
        const x = st.fx(1, k), m = st.module(1, k);
        if (x.err) return x.err;
        if (x.mod < 0) return "Load a module to use its send.";
        const from = [0, 1, 2].filter((i) => st.slot(i).on && st.slot(i).sends[k]).map((i) => i + 1);
        return `${m && m.builtin ? "Built-in" : "Installed module"}. ${from.length ? `Sends from ${from.join(" and ")}.` : "No sends yet."}`;
      }, "mv-mut mv-small");
      this.up(() => sub.classList.toggle("mv-errt", !!st.fx(1, k).err));
      const act = h("button", { class: "mv-pill mv-center", onclick: () => this.select(2, k) });
      const c = h("div", { class: "mv-card mv-ret", style: { "--c": FX_COLOR[k] }, "data-fx": k }, h("div", { class: "mv-h" }, `FX ${k + 1} return`), n, sub, h("span", { class: "mv-spacer" }), meter(this, 3 + k), act);
      this.up(() => { const e = st.fx(1, k).mod < 0; c.classList.toggle("mv-empty", e); act.textContent = e ? "Load module" : "Edit"; });
      out.push(c);
    }
    out.push(h("div", { class: "mv-card mv-master" }, h("div", { class: "mv-h" }, "Master"), h("span", { class: "mv-spacer" }), meter(this, 6, "wide"),
      h("div", { class: "mv-mut mv-small mv-center-t" }, "The Move's", h("br"), "volume knob")));
    return h("div", { class: "mv-mixer" }, out);
  }

  // ---- T1-3 FX setup: the chain (the Move's pad columns 1, 3, 5), the selected slot under it
  chain() {
    const ph = this.ph;
    const column = (c) => h("div", { class: "mv-col mv-chaincol", "data-col": c },
      h("div", { class: "mv-h mv-pad" }, ph ? COLS[c] : `${COLS[c]}${c === 0 ? " · in series" : c === 2 ? " · sends" : ""}`),
      [0, 1, 2].map((r) => (c === 1 ? this.fwCard(r) : this.fxCard(c ? 1 : 0, r))));
    const arrow = () => h("div", { class: "mv-arrow" }, svg(IC.arrow, 20));
    const chain = h("div", { class: "mv-chain" }, column(0), arrow(), column(1), arrow(), column(2));
    return h("div", { class: "mv-fxsetup" }, chain, this.col === 1 ? this.fwDetail(this.row) : this.fxDetail(this.col ? 1 : 0, this.row));
  }
  fxCard(kind, k) {
    const st = this.store, col = kind ? 2 : 0, on = !this.tab && this.col === col && this.row === k;
    const c = h("button", { class: `mv-card mv-fxslot ${on ? "sel" : ""}`, style: { "--c": kind ? FX_COLOR[k] : MFX_COLOR }, "data-kind": kind, "data-fxslot": k,
      onclick: () => this.select(col, k) },
      h("span", { class: "mv-row" }, this.fxDot(kind, k), h("span", { class: "mv-h" }, `${kind ? "FX" : ""} ${k + 1}`),
        this.text(() => st.modName(kind, k) || (this.ph ? "+" : "Empty"), "mv-name mv-grow mv-ell", "b")),
      this.ph ? null : this.text(() => { const y = st.fx(kind, k); return y.err ? "refused" : y.mod < 0 ? (kind ? "Pick a module" : "passes") : kind ? this.sendsFrom(k) : mfxText(y.filt); }, "mv-mut mv-small mv-ell"));
    this.up(() => c.classList.toggle("mv-empty", st.fx(kind, k).mod < 0));
    return c;
  }
  fwCard(i) {
    const st = this.store, s = st.slot(i), on = !this.tab && this.col === 1 && this.row === i;
    const c = h("button", { class: `mv-card mv-chfw ${on ? "sel" : ""} ${s.on ? "" : "mv-empty"}`, style: { "--c": st.colorOf(i) }, "data-slot": i, onclick: () => this.select(1, i) },
      h("span", { class: "mv-row" }, this.mini(i, `s28 ${s.on ? "" : "none"}`), h("b", { class: "mv-name mv-grow mv-ell" }, s.on ? s.name : st.pending[i] ? `${st.pending[i].name}...` : `Slot ${i + 1}`)),
      this.ph ? null : this.text(() => (st.slot(i).on ? `${tracksText(st.slot(i).filt)}${i === st.active ? " · playing" : ""}` : "Empty"), "mv-mut mv-small mv-ell"));
    return c;
  }
  // a firmware selected: its 16-track filter and its knobs on the Move (1 level, 2 pan, 3..5 sends)
  fwDetail(i) {
    const st = this.store, s = st.slot(i), ph = this.ph;
    if (!s.on) {
      const b = h("button", { class: "mv-pill", "aria-haspopup": "menu", onclick: () => this.ui.loadMenu(b, i) }, svg(IC.plus, 13), "Load a firmware");
      return h("div", { class: "mv-fxdetail" }, h("div", { class: "mv-card mv-fxedit mv-empty" }, h("div", { class: "mv-fx-none" }, h("b", {}, `Slot ${i + 1} is empty`), b)));
    }
    const lvl = { key: `level-${i}`, label: `${s.name} level`, get: () => st.slot(i).level | 0, set: (v) => st.edit(OP.MIX, i * 8, v), def: 100, fmt: levelText, color: () => st.colorOf(i) };
    const editor = h("div", { class: "mv-card mv-fxedit mv-fwedit", style: { "--c": st.colorOf(i) }, "data-slot": i },
      h("div", { class: "mv-row" }, this.mini(i, "s40"), h("div", { class: "mv-grow mv-min0" }, h("div", { class: "mv-h mv-c" }, `Firmware ${i + 1}${i === st.active ? " · playing" : ""}`),
        h("div", { class: "mv-name mv-ell" }, s.name))),
      h("div", { class: "mv-row" }, h("span", { class: "mv-h mv-grow" }, "Tracks it plays"), this.text(() => tracksText(st.slot(i).filt), "mv-val mv-mut")),
      this.fwCells(i, ph ? 8 : 16),
      h("div", { class: "mv-h" }, "Its knobs"),
      h("div", { class: `mv-knobs ${ph ? "ph" : ""}` },
        ph ? bar(this, { ...lvl, caption: "Level" }) : h("div", { class: "mv-param tile" }, dial(this, { ...lvl, cls: "big", caption: "Level" })),
        ph ? this.panCtl(i) : h("div", { class: "mv-param tile" }, this.panCtl(i)),
        [0, 1, 2].map((k) => (ph ? this.sendCtl(i, k) : h("div", { class: "mv-param tile" }, this.sendCtl(i, k))))),
      ph ? null : h("span", { class: "mv-spacer" }),
      ph ? null : h("div", { class: "mv-mut mv-small" }, "On the Move: FX setup, its pad in column 3; the steps are its tracks, knobs 1 to 5 its level, pan and sends."));
    const side = h("div", { class: "mv-col" }, h("div", { class: "mv-h mv-pad" }, "Mixer"),
      h("div", { class: "mv-card dim mv-col" }, h("span", { class: "mv-mut mv-small" }, "Every firmware's level, pan and sends side by side, with the FX returns and the meters."),
        h("button", { class: "mv-pill mv-center", onclick: () => this.setTab(1) }, "Open the Mixer")));
    return h("div", { class: "mv-fxdetail" }, editor, side);
  }
  // one knob cell of a page: its parameter (index idx) or a gap
  cell(kind, j, m, idx, n) {
    const st = this.store, ph = this.ph, p = idx >= 0 && m.params ? m.params[idx] : null, col = kind ? FX_COLOR[j] : MFX_COLOR;
    if (!p) return h("div", { class: "mv-param tile gap", "data-knob": n, "aria-hidden": "true" }, h("b", { class: "mv-gap" }, "–"), ph ? null : h("span", { class: "mv-mut mv-tiny" }, `knob ${n + 1}`));
    const label = ph ? p.short || p.name || p.key : p.name || p.key;
    const o = { key: `p-${kind}-${j}-${idx}`, label: p.name || p.key,
      get: () => { const v = (st.fx(kind, j).vals || [])[idx]; return v == null || v === UNSET ? p.def | 0 : v; },
      set: (v) => st.edit(OP.FXPAR, idx, kind << 9 | j << 7 | v) };
    const hint = kind ? wetHint(p) : "";
    if (p.type === "enum") {
      const opts = p.options && p.options.length ? p.options : Array.from({ length: p.max - p.min + 1 }, (_, i) => String(p.min + i));
      const ctl = opts.length <= 3 ? segs(this, { ...o, options: opts }) : choice(this, { ...o, options: opts });
      return h("div", { class: "mv-param tile enum", "data-knob": n, title: p.name }, h("b", { class: "mv-pname mv-ell" }, label), ctl);
    }
    const fmt = (v) => paramText(p, v), def = p.def != null ? p.def : null;
    return h("div", { class: `mv-param tile ${hint ? "hinted" : ""}`, "data-knob": n, title: p.name },
      ph ? bar(this, { ...o, cls: "small", caption: label, fmt, color: () => col, def }) : dial(this, { ...o, cls: "big", fmt, color: () => col, caption: label, def }),
      hint ? h("span", { class: "mv-hint" }, hint) : null);
  }
  // an FX slot selected: its module, its pages (8 knob cells each), and the track it acts on / the sends into it
  fxDetail(kind, j) {
    const st = this.store, ph = this.ph, x = st.fx(kind, j), m = st.module(kind, j), col = kind ? FX_COLOR[j] : MFX_COLOR;
    const pick = h("button", { class: "mv-pill mv-modpick", "aria-haspopup": "menu", onclick: () => this.modMenu(pick, kind, j) },
      this.fxDot(kind, j), h("b", {}, m ? m.name : "Pick a module"), m ? h("span", { class: "mv-val mv-mut" }, m.builtin ? "built-in" : "installed") : null, svg(IC.chev));
    const empty = h("button", { class: `mv-pill ${ph ? "mv-iconpill" : ""}`, disabled: x.mod < 0, "aria-label": `Empty ${kind ? "FX" : "MIDI FX"} ${j + 1}`, onclick: () => st.edit(OP.FXLOAD, kind * 4 + j, -1) },
      ph ? svg(IC.trash, 15) : "Empty the slot");
    const pages = modPages(m), pi = Math.max(0, Math.min(this.pg[kind][j], pages.length - 1)), page = pages[pi];
    let params;
    if (m && page) {
      const pager = pages.length > 1 ? h("div", { class: "mv-pager", role: "tablist", "aria-label": `${m.name}'s pages` },
        pages.map((g, i) => h("button", { class: `mv-pg ${i === pi ? "on" : ""}`, role: "tab", "aria-selected": i === pi ? "true" : "false", "data-pg": i, onclick: () => this.setPg(kind, j, i) }, g.name || `${i + 1}`)),
        h("span", { class: "mv-val mv-mut mv-pgn" }, `${pi + 1}/${pages.length}`)) : null;
      params = [h("div", { class: "mv-row" }, h("span", { class: "mv-h mv-grow" }, pages.length > 1 ? page.name || "Parameters" : "Parameters"), pager ? null : h("span", { class: "mv-val mv-mut" }, "1/1")),
        pager, h("div", { class: `mv-knobs ${ph ? "ph" : ""}`, "data-page": pi }, page.knobs.map((idx, n) => this.cell(kind, j, m, idx, n)))];
    } else if (m) params = h("div", { class: "mv-h" }, "No parameters");
    else params = h("div", { class: "mv-fx-none" }, h("b", {}, x.mod >= 0 ? x.name || "A module" : "Empty"), h("span", { class: "mv-mut" }, kind ? "A send bus with nothing on it: its sends go nowhere. Pick a module." : "This slot passes the notes through. Pick a module."));
    const editor = h("div", { class: "mv-card mv-fxedit", style: { "--c": col } },
      h("div", { class: "mv-row" }, ph ? null : h("span", { class: "mv-h" }, `${kind ? "FX" : "MIDI FX"} ${j + 1}`), pick, h("span", { class: "mv-spacer" }), empty), this.fxStatus(kind, j),
      params,
      ph ? null : h("span", { class: "mv-spacer" }),
      ph ? null : h("div", { class: "mv-mut mv-small" }, `On the Move: FX setup, its pad in column ${kind ? 5 : 1} (held + the jog: a module); the jog's click opens these pages, the jog turns them.`));
    const side = [];
    if (kind === 0) {
      side.push(h("div", { class: "mv-card mv-picker", style: { "--c": MFX_COLOR } }, h("div", { class: "mv-row" }, h("span", { class: "mv-h mv-grow" }, "Acts on"), this.allBtn(j)), this.mfxCells(j, 8),
        h("div", { class: "mv-mut mv-small" }, "One track or All: tap the lit one again for All. The others pass through.")));
    } else {
      side.push(h("div", { class: "mv-h mv-pad" }, `Sends into FX ${j + 1}`));
      for (let i = 0; i < NSLOT; i++) {
        if (!st.slot(i).on) continue;
        const c = h("div", { class: "mv-card mv-sendin", style: { "--c": st.colorOf(i) } }, this.sendCtl(i, j, true));
        this.up(() => c.classList.toggle("dimmed", !st.slot(i).sends[j]));
        side.push(c);
      }
      if (side.length === 1) side.push(h("p", { class: "mv-mut mv-small" }, "No firmware loaded."));
    }
    side.push(h("div", { class: "mv-h mv-pad" }, "Pick a module"), this.modList(kind, j));
    return h("div", { class: "mv-fxdetail" }, editor, h("div", { class: "mv-col" }, side));
  }
}
