// SPDX-License-Identifier: GPL-3.0-only
// The Mix & FX view on RemoteFM1's page served by an Ableton Move (RemoteFM14Move; only when js/host.js ON_MOVE): the
// Move's FX setup pages, in the Move's order, over the faceplate (the header and its slot chips stay):
//   T1 MIDI FX   the three MIDI FX slots in series: the selected one's module, its parameters and the track it acts on
//   T2 Routing   sequencer (16 tracks) -> MIDI FX 1 -> 2 -> 3 -> the three firmwares, each with its 16-track filter
//   T3 Audio FX  the three send buses: the selected one's module, its parameters (wet / dry hints), the sends into it
//   T4 Mixer     a strip per firmware (tracks, 3 sends, pan, level, meter), the 3 FX returns, the master
// The tab follows the Move's FX page (mix.page) and sets it (the PAGE op). Every edit is one of the host's ops
// (moveui.js OP: the same core path as the Move's own knobs); what shows is the core's state as /ws reports it.
// Desktop: a panel that slides over the faceplate, dials and faders. Phone: a full-screen dark sheet, bar sliders.
import { NSLOT, OP, PAGES, FX_COLOR, MFX_COLOR, h, svg, IC, pct, panText, tracksText, mfxText, paramText, wetHint, shortName,
  openPop, closePop, popItem, dial, fader, bar, segs, choice, meter } from "./moveui.js";

const RUN = "#3BD47A", BUSY = "#F5A524", ERR = "#FF5F57";

export class MixView {
  constructor({ store, minis, ui, phone }) {
    this.store = store; this.minis = minis; this.ui = ui; this.phoneMq = phone;
    this.isOpen = false;
    this.tab = 3; this.lastPage = null;
    this.sel = [0, 0];                       // the selected MIDI FX and audio FX slot
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
  setTab(i) {
    if (i === this.tab) return;
    this.tab = i; closePop();
    this.store.edit(OP.PAGE, i, 0);
    this.render();
  }

  // a rebuild when what is laid out changed; else the values only
  render() {
    const st = this.store, m = st.mix;
    if (m && m.page !== this.lastPage) { this.lastPage = m.page; if (m.page >= 0 && m.page < 4) this.tab = m.page; }
    if (!this.isOpen) return;
    const key = JSON.stringify([this.tab, this.ph, this.sel, st.active, st.slots.map((s) => [s.on, s.id]), st.pending.map((p) => p && p.id),
      m && [m.mfx.map((x) => x.mod), m.afx.map((x) => x.mod)], st.lib.midi.length, st.lib.audio.length]);
    if (key !== this.key) { this.key = key; this.build(); }
    for (const f of this.ups) f();
    this.meters();
  }
  build() {
    this.ups = []; this.meterUps = [];
    const scroll = this.body ? this.body.scrollTop : 0;
    const tabs = h("div", { class: "mv-tabs", role: "tablist" }, PAGES.map((t, i) => h("button", { class: `mv-tab ${i === this.tab ? "on" : ""}`, role: "tab",
      "aria-selected": i === this.tab ? "true" : "false", "data-tab": i, onclick: () => this.setTab(i) }, t, this.ph ? null : h("span", { class: "mv-t" }, `T${i + 1}`))));
    const live = h("span", { class: "mv-live" });
    this.up(() => { const m = this.store.mix; live.textContent = m && m.fxopen ? `The Move shows ${PAGES[m.page] || "FX setup"}` : ""; live.hidden = !(m && m.fxopen); });
    const head = this.ph
      ? [h("div", { class: "mv-ph-top" }, h("b", { class: "mv-title" }, "Mix & FX"), live,
          h("button", { class: "mv-iconbtn mv-close", "aria-label": "Close Mix & FX", onclick: () => this.close() }, svg(IC.close, 16))), tabs]
      : [h("div", { class: "mv-bar-top" }, tabs, h("span", { class: "mv-mut mv-small" }, "The Move's four FX setup pages; either side follows the other."), live,
          h("span", { class: "mv-spacer" }), h("button", { class: "mv-pill mv-back", onclick: () => this.close() }, "Back to the faceplate", h("span", { class: "mv-val mv-mut" }, "Esc")))];
    this.body = h("div", { class: `mv-body mv-tab-${this.tab}` }, [this.midiPage, this.routing, this.audioPage, this.mixer][this.tab].call(this));
    this.el.classList.toggle("phone", this.ph);
    this.el.replaceChildren(h("div", { class: "mv-head" }, head), this.body);
    this.el.tabIndex = -1;
    this.body.scrollTop = scroll;
  }
  midiPage() { return this.fxPage(0); }
  audioPage() { return this.fxPage(1); }

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
    const st = this.store, o = { key: `level-${i}`, label: `${st.slot(i).name} level`, get: () => st.slot(i).level | 0, set: (v) => st.edit(OP.MIX, i * 8, v), def: 100, color: () => st.colorOf(i) };
    return this.ph ? bar(this, { ...o, cls: "big", caption: "Level" }) : fader(this, o);
  }
  tracksPill(i) {
    const p = h("button", { class: "mv-pill mv-pill-s", title: "Its track filter (Routing)", onclick: () => this.setTab(1) });
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

  // ---- T4 Mixer
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
        const c = h("button", { class: "mv-card mv-ph-ret", style: { "--c": FX_COLOR[k] }, onclick: () => { this.sel[1] = k; this.setTab(2); } }, h("span", { class: "mv-h" }, `FX ${k + 1}`), n, meter(this, 3 + k, "h"));
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
      const act = h("button", { class: "mv-pill mv-center", onclick: () => { this.sel[1] = k; this.setTab(2); } });
      const c = h("div", { class: "mv-card mv-ret", style: { "--c": FX_COLOR[k] }, "data-fx": k }, h("div", { class: "mv-h" }, `FX ${k + 1} return`), n, sub, h("span", { class: "mv-spacer" }), meter(this, 3 + k), act);
      this.up(() => { const e = st.fx(1, k).mod < 0; c.classList.toggle("mv-empty", e); act.textContent = e ? "Load module" : "Edit"; });
      out.push(c);
    }
    out.push(h("div", { class: "mv-card mv-master" }, h("div", { class: "mv-h" }, "Master"), h("span", { class: "mv-spacer" }), meter(this, 6, "wide"),
      h("div", { class: "mv-mut mv-small mv-center-t" }, "The Move's", h("br"), "volume knob")));
    return h("div", { class: "mv-mixer" }, out);
  }

  // ---- T2 Routing
  routing() {
    const st = this.store, ph = this.ph;
    const tile = (t) => {
      const dots = h("span", { class: "mv-tdots" });
      this.up(() => dots.replaceChildren(...[0, 1, 2].filter((i) => st.slot(i).on && st.slot(i).filt >> t & 1).map((i) => h("i", { style: { background: st.colorOf(i) } }))));
      return h("div", { class: "mv-trk" }, h("b", {}, t + 1), dots);
    };
    const seq = h("div", { class: "mv-card mv-seq dim" }, h("div", { class: "mv-h" }, "Sequencer · 16 tracks"),
      h("div", { class: `mv-trks ${ph ? "c8" : ""}` }, Array.from({ length: 16 }, (_, t) => tile(t))),
      ph ? null : h("span", { class: "mv-spacer" }),
      h("div", { class: "mv-mut mv-small" }, "Every track goes through the MIDI FX in turn, then to each firmware whose track filter lets it in. The dots: who gets it."),
      h("div", { class: "mv-note-b" }, h("b", {}, "Live pads and the faceplate"), h("br"), h("span", { class: "mv-mut" }, "go straight to the playing slot.")));
    const sel = this.sel[0];
    const chain = [h("div", { class: "mv-h mv-pad" }, "MIDI FX · in series")];
    for (let j = 0; j < 3; j++) {
      const name = this.text(() => st.modName(0, j) || "Empty", "mv-name mv-grow mv-ell", "b");
      const pill = this.text(() => (st.fx(0, j).mod < 0 ? "passes" : mfxText(st.fx(0, j).filt)), "mv-pill mv-pill-s");
      const sub = this.text(() => { const x = st.fx(0, j); return x.err ? x.err : x.mod < 0 ? "Passes everything through" : mfxText(x.filt) === "All" ? "Acts on every track" : `Acts on ${tracksText(x.filt).toLowerCase()}; the others pass through`; }, "mv-mut mv-small");
      const c = h("button", { class: `mv-card mv-mfx ${j === sel ? "sel" : ""}`, style: { "--c": MFX_COLOR }, "data-mfx": j, onclick: () => { this.sel[0] = j; this.render(); } },
        h("span", { class: "mv-row" }, this.fxDot(0, j), h("span", { class: "mv-h" }, j + 1), name, pill), sub);
      this.up(() => c.classList.toggle("mv-empty", st.fx(0, j).mod < 0));
      chain.push(c);
      if (j < 2) chain.push(h("div", { class: "mv-arrow-s" }, svg(IC.down, 14)));
    }
    chain.push(h("div", { class: "mv-card mv-picker dim" }, h("div", { class: "mv-row" }, this.text(() => `${st.modName(0, sel) || `MIDI FX ${sel + 1}`} · acts on`, "mv-h mv-grow"), this.allBtn(sel)),
      this.mfxCells(sel, 8), h("div", { class: "mv-mut mv-small" }, "One track or All: tap the lit one again for All. The others pass through.")));
    const fws = [h("div", { class: "mv-h mv-pad" }, "Firmware track filters")];
    for (let i = 0; i < NSLOT; i++) {
      const s = st.slot(i);
      if (!s.on) { fws.push(h("div", { class: "mv-card mv-empty mv-fwempty" }, this.mini(i, "s28 none"), h("span", { class: "mv-mut" }, `Slot ${i + 1} is empty`))); continue; }
      fws.push(h("div", { class: "mv-card mv-fw", style: { "--c": st.colorOf(i) }, "data-slot": i },
        h("div", { class: "mv-row" }, this.mini(i, "s28"), h("b", { class: "mv-name mv-grow mv-ell" }, s.name), this.text(() => tracksText(st.slot(i).filt), "mv-val mv-mut"),
          ph ? null : h("button", { class: "mv-pill mv-pill-s", onclick: () => this.setTab(3) }, "to Mixer")),
        this.fwCells(i, ph ? 8 : 16)));
    }
    fws.push(h("div", { class: "mv-mut mv-small mv-pad" }, "Why: a firmware that listens on channel 1 only gets just track 6, with track 6 set to channel 1."));
    if (ph) return h("div", { class: "mv-col" }, seq, ...chain, ...fws);
    const arrow = () => h("div", { class: "mv-arrow" }, svg(IC.arrow, 24));
    return h("div", { class: "mv-routing" }, seq, arrow(), h("div", { class: "mv-col" }, chain), arrow(), h("div", { class: "mv-col" }, fws));
  }

  // ---- T1 MIDI FX, T3 Audio FX: the three slots, the selected one's module and parameters
  fxPage(kind) {
    const st = this.store, ph = this.ph, j = this.sel[kind], x = st.fx(kind, j), m = st.module(kind, j), col = kind ? FX_COLOR[j] : MFX_COLOR;
    const slotCard = (k) => {
      const c = h("button", { class: `mv-card mv-fxslot ${k === j ? "sel" : ""}`, style: { "--c": kind ? FX_COLOR[k] : MFX_COLOR }, "data-fxslot": k, onclick: () => { this.sel[kind] = k; this.render(); } },
        h("span", { class: "mv-row" }, this.fxDot(kind, k), h("span", { class: "mv-h" }, `${kind ? "FX" : ""} ${k + 1}`), this.text(() => st.modName(kind, k) || (ph ? "+ Load" : "Empty"), "mv-name mv-grow mv-ell", "b")),
        this.text(() => { const y = st.fx(kind, k); return y.err ? "refused" : y.mod < 0 ? (kind ? "Pick a module" : "passes") : kind ? (ph ? "" : this.sendsFrom(k)) : mfxText(y.filt); }, "mv-mut mv-small mv-ell"));
      this.up(() => c.classList.toggle("mv-empty", st.fx(kind, k).mod < 0));
      return c;
    };
    const slots = [0, 1, 2].map(slotCard);
    // the selected slot
    const pick = h("button", { class: "mv-pill mv-modpick", "aria-haspopup": "menu", onclick: () => this.modMenu(pick, kind, j) },
      this.fxDot(kind, j), h("b", {}, m ? m.name : "Pick a module"), m ? h("span", { class: "mv-val mv-mut" }, m.builtin ? "built-in" : "installed") : null, svg(IC.chev));
    const empty = h("button", { class: `mv-pill ${ph ? "mv-iconpill" : ""}`, disabled: x.mod < 0, "aria-label": `Empty ${kind ? "FX" : "MIDI FX"} ${j + 1}`, onclick: () => st.edit(OP.FXLOAD, kind * 4 + j, -1) },
      ph ? svg(IC.trash, 15) : "Empty the slot");
    const params = [];
    if (m) (m.params || []).slice(0, 8).forEach((p, n) => {
      const o = { key: `p-${kind}-${j}-${n}`, label: p.name || p.key, get: () => (st.fx(kind, j).vals || [])[n] | 0, set: (v) => st.edit(OP.FXPARAM, kind << 5 | j << 3 | n, v) };
      const hint = kind ? wetHint(p) : "";
      if (p.type === "enum") {
        const opts = p.options && p.options.length ? p.options : Array.from({ length: p.max - p.min + 1 }, (_, i) => String(p.min + i));
        const ctl = opts.length <= (ph ? 4 : 5) ? segs(this, { ...o, options: opts }) : choice(this, { ...o, options: opts });
        params.push(h("div", { class: `mv-param enum ${ph ? "" : "tile"} ${opts.length > 2 && opts.length <= 5 ? "wide" : ""}` }, ph ? h("div", { class: "mv-h" }, p.name || p.key) : null, ctl, ph ? null : h("b", { class: "mv-pname" }, p.name || p.key), ph ? null : h("span", { class: "mv-val mv-mut" }, `${opts.length} choices`)));
      } else if (ph) {
        params.push(h("div", { class: `mv-param ${hint ? "hinted" : ""}` }, bar(this, { ...o, caption: p.name || p.key, fmt: (v) => paramText(p, v), color: () => col, def: p.def != null ? Math.round((p.def - p.min) / ((p.max - p.min) || 1) * 127) : null }),
          hint ? h("span", { class: "mv-hint" }, hint) : null));
      } else {
        params.push(h("div", { class: `mv-param tile ${hint ? "hinted" : ""}` }, dial(this, { ...o, cls: "big", fmt: (v) => paramText(p, v), color: () => col, caption: p.name || p.key }),
          hint ? h("span", { class: "mv-hint" }, hint) : null));
      }
    });
    const err = this.fxStatus(kind, j);
    const where = kind ? `Session, then Track ${j + 1}` : "Session, then the MIDI FX page";
    const editor = h("div", { class: "mv-card mv-fxedit", style: { "--c": col } },
      h("div", { class: "mv-row" }, ph ? null : h("span", { class: "mv-h" }, `${kind ? "FX" : "MIDI FX"} ${j + 1}`), pick, h("span", { class: "mv-spacer" }), empty), err,
      m ? [h("div", { class: "mv-h" }, m.params && m.params.length ? "Parameters" : "No parameters"), h("div", { class: `mv-params ${ph ? "" : "grid"}` }, params)]
        : h("div", { class: "mv-fx-none" }, h("b", {}, x.mod >= 0 ? x.name || "A module" : "Empty"), h("span", { class: "mv-mut" }, kind ? "A send bus with nothing on it: its sends go nowhere. Pick a module." : "This slot passes the notes through. Pick a module.")),
      ph ? null : h("span", { class: "mv-spacer" }),
      ph ? null : h("div", { class: "mv-mut mv-small" }, `On the Move: ${where}; its knobs edit the same parameters, and either side follows the other.`));
    // MIDI FX: the track it acts on; audio FX: the sends into it
    let side;
    if (kind === 0) {
      side = h("div", { class: "mv-card mv-picker", style: { "--c": MFX_COLOR } }, h("div", { class: "mv-row" }, h("span", { class: "mv-h mv-grow" }, "Acts on"), this.allBtn(j)), this.mfxCells(j, 8));
    } else {
      side = [h("div", { class: "mv-h mv-pad" }, `Sends into FX ${j + 1}`)];
      for (let i = 0; i < NSLOT; i++) {
        if (!st.slot(i).on) continue;
        const c = h("div", { class: "mv-card mv-sendin", style: { "--c": st.colorOf(i) } }, this.sendCtl(i, j, true));
        this.up(() => c.classList.toggle("dimmed", !st.slot(i).sends[j]));
        side.push(c);
      }
      if (side.length === 1) side.push(h("p", { class: "mv-mut mv-small" }, "No firmware loaded."));
    }
    if (ph) return h("div", { class: "mv-col" }, h("div", { class: "mv-grid3" }, slots), h("div", { class: "mv-chainline" }, kind ? "each firmware → its sends → FX 1 · 2 · 3 → the mix" : "sequencer → 1 → 2 → 3 → firmwares"), editor, side);
    return h("div", { class: "mv-fxpage" },
      h("div", { class: "mv-col" }, h("div", { class: "mv-h mv-pad" }, kind ? "Send slots" : "MIDI FX slots, in series"), slots, h("span", { class: "mv-spacer" }),
        h("div", { class: "mv-mut mv-small" }, kind ? "Each slot runs on a bus: every firmware sends to it, after its level and pan. Its return joins the mix."
          : "The sequencer's notes go through 1, then 2, then 3, then to the firmwares. Each acts on one track or all.")),
      editor,
      h("div", { class: "mv-col" }, side, h("div", { class: "mv-h mv-pad" }, "Pick a module"), this.modList(kind, j)));
  }
}
