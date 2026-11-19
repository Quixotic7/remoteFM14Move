// SPDX-License-Identifier: GPL-3.0-only
// The Move's three firmware slots on RemoteFM1's page (RemoteFM14Move; only when js/host.js ON_MOVE):
//   - desktop: slot chips in the header, where the firmware picker is on remotefm1.com (#fw-btn, hidden on the Move).
//     Each: the slot's live mini screen, its firmware's name and version, a thin level bar in the slot's colour; the
//     playing one dark with a ring in its colour; an empty one dashed, "Load into n". A tap plays that slot (the
//     host's ACTIVE op); its chevron, or a tap on an empty one, opens "Load into slot n" (the Move's firmwares, the
//     ones on another slot marked "on 2") with "Unload slot n". A "Mix & FX" cap (⌥M) opens js/movemix.js's view.
//   - phone (the iOS app's iPhone layout): the header's pill is the slot button (the playing slot's mini screen and
//     name, three slot dots); a tap drops the slot menu over the dimmed faceplate: three 64 px rows, an empty slot as
//     "Load into slot n", then "Mix & FX" and "Load into slot n" (the firmware list, with Unload).
//   - keys: ⌥1 ⌥2 ⌥3 play a slot (an empty one: its load menu), ⌥M the Mix & FX view, Esc back to the faceplate
//     (when nothing is latched: the panel's Esc lets latches go first).
// The faceplate follows the playing slot by itself (movehost.js "moved": main.js followMove).
//   mountMoveUi({ player, controls, sheet, catalog, toast }) -> the UI (window.FM1MoveUi, for the e2e test)
import { NSLOT, MoveStore, Minis, h, svg, IC, pct, openPop, closePop, popOpen, popItem } from "./moveui.js";
import { MixView } from "./movemix.js";

const PHONE = "(max-width: 640px)";

class SlotsUi {
  constructor(o) {
    this.o = o;
    this.store = new MoveStore(o);
    this.minis = new Minis(o.player);
    this.phone = matchMedia(PHONE);
    this.menuEl = null; this.menuSub = null;    // the phone's slot menu; its firmware list for a slot (or null)
    this.buildHeader();
    this.view = new MixView({ store: this.store, minis: this.minis, ui: this, phone: this.phone });
    this.store.on((what) => { if (what !== "meters") this.render(); else this.meters(); });
    this.phone.addEventListener("change", () => { this.closeMenus(); this.render(); });
    this.keys();
    this.render();
  }

  // ---- the header: chips (desktop), the slot button (phone), the Mix & FX cap
  buildHeader() {
    const top = document.getElementById("top"), fw = document.getElementById("fw-btn"), nav = top.querySelector(":scope > nav");
    this.chipsEl = h("nav", { class: "mv-chips", id: "mv-chips", "aria-label": "Firmware slots" });
    this.hints = h("span", { class: "mv-hints", "aria-hidden": "true" }, h("span", { class: "mv-kbd" }, "⌥1"), h("span", { class: "mv-kbd" }, "⌥2"), h("span", { class: "mv-kbd" }, "⌥3"));
    this.slotBtn = h("button", { class: "mv-slotbtn", id: "mv-slotbtn", "aria-haspopup": "menu", "aria-expanded": "false", onclick: () => this.toggleMenu() });
    this.mixBtn = h("button", { class: "cap mv-mixbtn", id: "mv-mixbtn", "aria-pressed": "false", title: "Mix & FX (⌥M)", onclick: () => this.view.toggle() },
      svg(IC.mix, 15), h("span", {}, "Mix & FX"), h("span", { class: "mv-kbd mv-kbd-cap" }, "⌥M"));
    fw.after(this.chipsEl, this.hints, this.slotBtn);
    nav.before(this.mixBtn);
    this.chips = [0, 1, 2].map((i) => this.chip(i));
    this.chipsEl.append(...this.chips.map((c) => c.el));
  }
  chip(i) {
    const c = { i, key: "" };
    c.mini = this.minis.canvas(i);
    c.name = h("span", { class: "mv-nm" }); c.ver = h("span", { class: "mv-vr" });
    c.lv = h("i"); c.mt = h("b");
    c.main = h("button", { class: "mv-chip-main", "data-slot": i, onclick: () => this.pick(i) },
      h("span", { class: "mv-mini-w" }, c.mini), h("span", { class: "mv-chip-t" }, c.name, c.ver), h("span", { class: "mv-lv" }, c.lv, c.mt));
    c.chev = h("button", { class: "mv-chev", "aria-haspopup": "menu", "aria-expanded": "false", "aria-label": `Load into slot ${i + 1}`, "data-slot": i,
      onclick: (e) => { e.stopPropagation(); this.loadMenu(c.chev, i); } }, svg(IC.chev));
    c.el = h("div", { class: "mv-chip", role: "group", "data-slot": i }, c.main, c.chev);
    return c;
  }
  pick(i) {
    const s = this.store.slot(i);
    if (!s.on) { this.loadMenu(this.chips[i].chev, i); return; }
    this.store.activate(i);
  }

  render() {
    const st = this.store, act = st.active;
    for (const c of this.chips) {
      const s = st.slot(c.i), p = st.pending[c.i];
      c.el.classList.toggle("on", !!s.on && c.i === act);
      c.el.classList.toggle("empty", !s.on && !p);
      c.el.classList.toggle("loading", !!p);
      c.el.style.setProperty("--c", st.colorOf(c.i));
      c.main.setAttribute("aria-pressed", s.on && c.i === act ? "true" : "false");
      c.main.setAttribute("aria-label", s.on ? `Slot ${c.i + 1}: ${s.name}${c.i === act ? ", playing" : ""}` : `Slot ${c.i + 1}: empty, load a firmware`);
      c.name.textContent = p ? p.name : s.on ? s.name : `Load into ${c.i + 1}`;
      c.ver.textContent = p ? "loading" : s.on ? s.version || "" : "";
      c.lv.style.width = s.on ? pct(s.level) : "0%";
      const icon = s.on || p ? "chev" : "plus";
      if (c.chev.dataset.icon !== icon) { c.chev.dataset.icon = icon; c.chev.replaceChildren(svg(IC[icon], icon === "plus" ? 13 : 12)); }
      if (!s.on) this.minis.clear(c.i);
    }
    this.renderSlotBtn();
    // (the menus rebuilt only when what they show changed: a rebuild under a press would lose its click)
    const key = JSON.stringify([act, st.slots.map((s) => [s.on, s.id, s.level]), st.pending.map((p) => p && p.id)]);
    if (key !== this.menuKey) {
      this.menuKey = key;
      if (this.menuEl) this.fillMenu();
      if (this.pop && this.pop.refill) this.pop.refill();
    }
    this.meters();
  }
  meters() {
    const m = this.store.meters;
    for (const c of this.chips) c.mt.style.width = pct(m[c.i] || 0, 255);
  }

  // ---- the load menu (desktop: under the chip; phone: inside the slot menu)
  loadRows(slot, done) {
    const st = this.store, fws = st.firmwares();
    const rows = fws.map((f) => {
      const k = st.where(f);
      return popItem({ label: f.name, id: f.id, dot: st.fwColor(f), sub: k >= 0 ? (k === slot ? "here" : `on ${k + 1}`) : f.version, on: k === slot,
        onclick: () => { done(); if (k !== slot) st.load(slot, f, true); else st.activate(slot); } });
    });
    if (!fws.length) rows.push(h("p", { class: "mv-pop-note" }, "No firmwares installed on this Move."));
    return rows;
  }
  loadMenu(anchor, slot) {
    this.pop = openPop(anchor, () => [
      h("div", { class: "mv-h mv-pop-h" }, `Load into slot ${slot + 1}`),
      ...this.loadRows(slot, () => closePop()),
      h("div", { class: "mv-pop-foot" },
        h("button", { class: "mv-pill", disabled: !this.store.slot(slot).on, "data-unload": slot, onclick: () => { closePop(); this.store.unload(slot); } }, `Unload slot ${slot + 1}`),
        h("button", { class: "mv-pill", onclick: () => closePop() }, "Close")),
    ], { width: 280, cls: "mv-loadmenu", onClose: () => { this.pop = null; } });
  }

  // ---- the phone's slot button and menu
  renderSlotBtn() {
    const st = this.store, act = st.active, s = st.slot(act);
    const key = `${act}|${s.on ? s.id : ""}`;
    if (this.slotBtn.dataset.key !== key) {
      this.slotBtn.dataset.key = key;
      this.slotBtn.replaceChildren(h("span", { class: "mv-mini-w" }, this.minis.canvas(act)), h("span", { class: "mv-sb-n" }, s.on ? s.name : "No firmware"),
        this.sbDots = h("span", { class: "mv-sb-dots", "aria-hidden": "true" }), svg(IC.chev));
    }
    this.sbDots.replaceChildren(...[0, 1, 2].map((i) => h("span", { class: `mv-dot ${st.slot(i).on ? "" : "hollow"} ${i === act ? "on" : ""}`, style: { "--c": st.colorOf(i) } })));
    this.slotBtn.setAttribute("aria-label", `Firmware slots: ${s.on ? s.name + " playing" : "none playing"}`);
  }
  toggleMenu(open = !this.menuEl) {
    if (!open) { this.closeMenus(); return; }
    this.menuSub = null;
    this.dim = h("div", { class: "mv-dim", onclick: () => this.closeMenus() });
    this.menuEl = h("div", { class: "mv-ipmenu", role: "menu", "aria-label": "Firmware slots" });
    document.body.append(this.dim, this.menuEl);
    this.slotBtn.setAttribute("aria-expanded", "true");
    this.fillMenu();
  }
  fillMenu() {
    const st = this.store, act = st.active, m = this.menuEl;
    if (this.menuSub != null) {
      const slot = this.menuSub;
      m.replaceChildren(
        h("div", { class: "mv-ip-head" }, h("button", { class: "mv-iconbtn", "aria-label": "Back", onclick: () => { this.menuSub = null; this.fillMenu(); } }, svg(IC.back, 16)),
          h("span", { class: "mv-h" }, `Load into slot ${slot + 1}`)),
        h("div", { class: "mv-ip-list" }, ...this.loadRows(slot, () => this.closeMenus())),
        h("div", { class: "mv-ip-foot" }, h("button", { class: "mv-pill", disabled: !st.slot(slot).on, "data-unload": slot, onclick: () => { this.closeMenus(); st.unload(slot); } }, `Unload slot ${slot + 1}`)));
      return;
    }
    const rows = [0, 1, 2].map((i) => {
      const s = st.slot(i), p = st.pending[i];
      if (!s.on && !p) return h("button", { class: "mv-iprow empty", role: "menuitem", "data-slot": i, onclick: () => { this.menuSub = i; this.fillMenu(); } },
        h("span", { class: "mv-mini-w" }, this.minis.canvas(i)), h("b", { class: "mv-iprow-n" }, `Load into slot ${i + 1}`), h("span", { class: "mv-h" }, i + 1));
      return h("button", { class: `mv-iprow ${i === act ? "on" : ""}`, role: "menuitem", "data-slot": i, style: { "--c": st.colorOf(i) },
        onclick: () => { st.activate(i); this.closeMenus(); } },
        h("span", { class: "mv-mini-w" }, this.minis.canvas(i)),
        h("span", { class: "mv-iprow-t" }, h("span", { class: "mv-iprow-l" }, h("b", {}, p ? p.name : s.name), h("span", { class: "mv-val mv-mut" }, p ? "loading" : s.version || "")),
          h("span", { class: "mv-hbar" }, h("i", { style: { width: pct(s.level || 0) } }))),
        h("span", { class: "mv-h", style: { color: i === act ? "var(--c)" : null } }, i + 1));
    });
    m.replaceChildren(...rows, h("div", { class: "mv-ip-foot" },
      h("button", { class: "mv-pill", id: "mv-ip-mix", onclick: () => { this.closeMenus(); this.view.open(); } }, svg(IC.mix, 14), "Mix & FX"),
      h("button", { class: "mv-pill", onclick: () => { this.menuSub = act; this.fillMenu(); } }, `Load into slot ${act + 1}`)));
  }
  closeMenus() {
    let any = closePop();
    if (this.menuEl) { this.menuEl.remove(); this.dim.remove(); this.menuEl = this.dim = null; this.slotBtn.setAttribute("aria-expanded", "false"); any = true; }
    return any;
  }

  // ---- keys: ⌥1-3, ⌥M, Esc (e.code: ⌥ changes e.key on a Mac)
  keys() {
    addEventListener("keydown", (e) => {
      if (this.o.sheet && this.o.sheet.isOpen) return;
      const t = e.target, typing = t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName));
      if (e.altKey && !e.metaKey && !e.ctrlKey && !typing) {
        const d = /^Digit([1-3])$/.exec(e.code);
        if (d) {
          e.preventDefault(); e.stopImmediatePropagation();
          if (e.repeat) return;
          const i = +d[1] - 1;
          if (this.store.slot(i).on) this.store.activate(i); else if (!this.phone.matches) this.loadMenu(this.chips[i].chev, i); else { this.toggleMenu(true); this.menuSub = i; this.fillMenu(); }
          return;
        }
        if (e.code === "KeyM") { e.preventDefault(); e.stopImmediatePropagation(); if (!e.repeat) this.view.toggle(); return; }
      }
      if (e.code === "Escape" && !e.metaKey && !e.ctrlKey && !e.altKey) {
        if (popOpen() || this.menuEl) { e.preventDefault(); e.stopImmediatePropagation(); this.closeMenus(); return; }
        if (this.view.isOpen && !(this.o.controls && this.o.controls.anyLatched())) { e.preventDefault(); e.stopImmediatePropagation(); this.view.close(); }
      }
    }, true);
  }
}

export function mountMoveUi(o) {
  // the stylesheet (only on the Move: the site's own index.html stays RemoteFM1's)
  if (!document.querySelector('link[href="css/move.css"]')) document.head.append(h("link", { rel: "stylesheet", href: "css/move.css" }));
  const ui = new SlotsUi(o);
  window.FM1MoveUi = ui;
  return ui;
}
export { NSLOT };
