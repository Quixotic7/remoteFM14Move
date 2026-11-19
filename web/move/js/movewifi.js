// SPDX-License-Identifier: GPL-3.0-only
// The FM-1's Wi-Fi, set up from the Move's page (RemoteFM14Move): an FM-1 running RemoteFM1 Wi-Fi, plugged into the
// Move's USB-A, joins a network without a computer. A panel like the settings sheet ("FM-1 Wi-Fi", from the sheet's
// FM-1 Wi-Fi section, or the page's ?wifi): the FM-1's state; the networks it hears (its own scan, 2.4 GHz: strength
// bars, a lock for a password); a password (shown or not) and Join; the FM-1 followed until it has an address, then
// "unplug it from USB-A" (the Move finds it over Wi-Fi by itself); the networks it keeps, each with Forget.
// Over the host's API (src/web.c /api/fm1/wifi: fm1link.c sends RemoteFM1's WIFI over USB-A). The password goes once,
// in a POST body, and is cleared from the field once sent. Nothing here runs unless js/host.js ON_MOVE
// (js/moveslots.js mountMoveUi mounts it).
import { h, svg } from "./moveui.js";

const API = "api/fm1/wifi";
const IC = {
  wifi: '<path d="M1.5 6.2a9.5 9.5 0 0113 0M3.8 8.6a6.2 6.2 0 018.4 0M6 11a3 3 0 014 0"/><circle cx="8" cy="13.2" r=".6" fill="currentColor"/>',
  lock: '<rect x="3.5" y="7" width="9" height="6.5" rx="1.2"/><path d="M5.5 7V5a2.5 2.5 0 015 0v2"/>',
  eye: '<path d="M1.5 8s2.4-4.5 6.5-4.5S14.5 8 14.5 8 12.1 12.5 8 12.5 1.5 8 1.5 8z"/><circle cx="8" cy="8" r="2"/>',
  eyeOff: '<path d="M2 2l12 12M6.6 3.7A6.8 6.8 0 018 3.5c4.1 0 6.5 4.5 6.5 4.5a11 11 0 01-1.9 2.4M4.3 4.8A11 11 0 001.5 8s2.4 4.5 6.5 4.5c1 0 1.9-.3 2.7-.7"/>',
  close: '<path d="M4 4l8 8M12 4l-8 8"/>', scan: '<path d="M13.5 8a5.5 5.5 0 11-1.6-3.9M13.5 2.5v3h-3"/>',
  usb: '<path d="M8 1.5v10M5.5 4L8 1.5 10.5 4M4 7v2l4 2.5M12 6v2.5L8 11"/><circle cx="8" cy="13.2" r="1.3"/>',
  check: '<path d="M3.5 8.5l3 3 6-7"/>',
};
const CSS = `
.mv-wf-scrim { position: fixed; inset: 0; background: rgba(0,0,0,.45); z-index: 30; }
.mv-wf { position: fixed; top: 8px; right: 8px; bottom: 8px; width: min(420px, calc(100vw - 16px)); z-index: 31;
  background: var(--window, #1c1a17); color: var(--text, #eee); border: 1px solid var(--border, #444); border-radius: 24px;
  box-shadow: 0 18px 44px rgba(0,0,0,.5); display: flex; flex-direction: column; font: 14px/1.35 var(--ui, system-ui); outline: none; }
.mv-wf[hidden], .mv-wf-scrim[hidden] { display: none; }
.mv-wf-head { height: 60px; flex: none; display: flex; align-items: center; gap: 10px; padding: 0 12px 0 18px; border-bottom: 1px solid var(--divider, #333); }
.mv-wf-head b { font: 700 17px/1 var(--title, inherit); }
.mv-wf-head .sp { flex: 1; }
.mv-wf-head .mv-ic { color: var(--accent, #3cc8dc); }
.mv-wf-x { width: 36px; height: 36px; display: grid; place-items: center; border: 0; background: var(--raised, #333); color: var(--knob-off, #bbb); border-radius: 999px; cursor: pointer; }
.mv-wf-x:hover { color: var(--text, #fff); }
.mv-wf-body { flex: 1; overflow-y: auto; padding: 4px 18px 18px; scrollbar-width: thin; }
.mv-wf .heading { display: flex; align-items: center; justify-content: space-between; }
.mv-wf-card { border-radius: 16px; background: var(--surface, rgba(255,255,255,.04)); border: 1px solid var(--line, var(--border, #333)); padding: 12px 14px; display: grid; gap: 6px; }
.mv-wf-line { display: flex; align-items: center; gap: 8px; min-width: 0; }
.mv-wf-line .t { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mv-wf-line b { font-weight: 650; }
.mv-wf-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--muted, #888); flex: none; }
.mv-wf-dot.ok { background: var(--green, #3BD47A); box-shadow: 0 0 8px var(--green, #3BD47A); }
.mv-wf-dot.busy { background: var(--accent, #3cc8dc); animation: mv-wf-pulse 1s ease-in-out infinite; }
.mv-wf-dot.bad { background: var(--amber, #F5A524); }
@keyframes mv-wf-pulse { 50% { opacity: .3; } }
.mv-wf-need { display: flex; gap: 10px; align-items: center; padding: 12px 14px; border-radius: 16px; border: 1px dashed var(--border, #555); color: var(--row-text, #ddd); font-size: 13.5px; }
.mv-wf-need .mv-ic { color: var(--accent, #3cc8dc); }
.mv-wf-list { display: grid; gap: 2px; }
.mv-wf-net { border-radius: 14px; }
.mv-wf-net.sel { background: var(--surface, rgba(255,255,255,.05)); box-shadow: inset 0 0 0 1px var(--accent, #3cc8dc); }
.mv-wf-row { width: 100%; display: grid; grid-template-columns: 22px minmax(0, 1fr) auto 16px; align-items: center; gap: 10px; min-height: 48px; padding: 0 10px; background: none; border: 0; color: inherit; font: inherit; text-align: left; cursor: pointer; border-radius: 14px; }
.mv-wf-row:hover:not(:disabled) { background: var(--raised-hover, rgba(255,255,255,.05)); }
.mv-wf-row:disabled { cursor: default; opacity: .5; }
.mv-wf-row .nm { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 15px; }
.mv-wf-row .tag { font-size: 11px; font-weight: 600; letter-spacing: .04em; color: var(--muted, #999); white-space: nowrap; }
.mv-wf-row .tag.on { color: var(--green, #3BD47A); }
.mv-wf-row .lk { color: var(--muted, #999); display: grid; }
.mv-wf-bars { display: grid; grid-auto-flow: column; align-items: end; gap: 2px; height: 14px; width: 20px; }
.mv-wf-bars i { display: block; width: 3px; border-radius: 1px; background: var(--border, #555); }
.mv-wf-bars i.on { background: var(--accent, #3cc8dc); }
.mv-wf-form { display: grid; gap: 10px; padding: 2px 10px 12px; }
.mv-wf-pass { display: flex; align-items: center; gap: 6px; height: 44px; border-radius: 12px; border: 1px solid var(--border, #555); background: var(--raised, #2a2a2a); padding: 0 6px 0 12px; }
.mv-wf-pass:focus-within { border-color: var(--accent, #3cc8dc); }
.mv-wf-pass input { flex: 1; min-width: 0; background: none; border: 0; outline: none; color: var(--text, #fff); font: 500 16px var(--ui, inherit); }
.mv-wf-pass button { width: 34px; height: 34px; display: grid; place-items: center; border: 0; background: none; color: var(--knob-off, #aaa); border-radius: 999px; cursor: pointer; }
.mv-wf-pass button:hover { color: var(--text, #fff); }
.mv-wf-form .pills { margin-top: 0; }
.mv-wf-prog { height: 3px; border-radius: 2px; background: var(--border, #444); overflow: hidden; }
.mv-wf-prog i { display: block; height: 100%; width: 40%; background: var(--accent, #3cc8dc); animation: mv-wf-slide 1.2s ease-in-out infinite; }
@keyframes mv-wf-slide { from { transform: translateX(-100%); } to { transform: translateX(250%); } }
.mv-wf-done { color: var(--green, #3BD47A); }
.mv-wf-bad { color: var(--amber, #F5A524); }
.mv-wf-kept { display: grid; grid-template-columns: minmax(0, 1fr) auto auto; align-items: center; gap: 10px; min-height: 44px; padding: 0 4px 0 10px; }
.mv-wf-kept .nm { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.mv-wf-kept .tag { font-size: 11px; color: var(--muted, #999); white-space: nowrap; }
.mv-wf-kept .tag.on { color: var(--green, #3BD47A); }
.mv-wf-kept .pill { height: 32px; padding: 0 12px; font-size: 13px; }
.mv-wf-empty { color: var(--muted, #999); padding: 10px; font-size: 13px; }
.mv-wf .heading .pill { height: 30px; padding: 0 12px; font-size: 12.5px; letter-spacing: 0; text-transform: none; display: inline-flex; gap: 6px; align-items: center; }
@media (max-width: 640px) { .mv-wf { top: 0; right: 0; bottom: 0; width: 100vw; border-radius: 0; border: 0; } }
@media (prefers-reduced-motion: reduce) { .mv-wf-dot.busy, .mv-wf-prog i { animation: none; } }
`;
const bars = (rssi) => (rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1);
const barsEl = (rssi) => h("span", { class: "mv-wf-bars", title: `${rssi} dBm`, "aria-label": `signal ${bars(rssi)} of 4` },
  [5, 8, 11, 14].map((px, i) => h("i", { class: i < bars(rssi) ? "on" : "", style: { height: `${px}px` } })));
const STATE = { off: "Wi-Fi off", starting: "Wi-Fi starting", on: "Not on a network", joining: "Joining", address: "Getting an address", connected: "Connected", failed: "Not connected" };

export class WifiView {
  constructor({ toast }) {
    this.toast = toast || (() => {});
    this.isOpen = false; this.d = null; this.sel = null; this.show = false; this.busy = false; this.forgetAsk = null;
    this.asked = 0;   // (a scan asked when opened: once)
    if (!document.getElementById("mv-wf-css")) document.head.append(h("style", { id: "mv-wf-css" }, CSS));
    this.scrim = h("div", { class: "mv-wf-scrim", hidden: true, onclick: () => this.close() });
    this.status = h("div", { class: "mv-wf-status" });
    this.netsHead = h("h3", { class: "heading" }, "Networks the FM-1 hears");
    this.nets = h("div", { class: "mv-wf-list", role: "list", "aria-label": "Networks" });
    this.keptHead = h("h3", { class: "heading" }, "Networks it keeps");
    this.kept = h("div", { class: "mv-wf-list", "aria-label": "Networks kept" });
    this.note = h("p", { class: "note" }, "The FM-1's Wi-Fi is 2.4 GHz only. A phone's hotspot works too (on an iPhone: Maximise Compatibility on). The Move must be on the same network to find it.");
    // the password: one field, kept across renders (a list update never takes the focus or what was typed)
    this.pass = h("input", { type: "password", id: "mv-wf-pass", autocomplete: "off", autocapitalize: "off", spellcheck: "false", maxlength: "63",
      placeholder: "Password", "aria-label": "The network's password",
      onkeydown: (e) => { if (e.key === "Enter") { e.preventDefault(); this.join(); } }, oninput: () => this.formState() });
    this.eye = h("button", { type: "button", class: "mv-wf-eye", "aria-label": "Show the password", onclick: () => { this.show = !this.show; this.pass.type = this.show ? "text" : "password"; this.eyeIcon(); this.pass.focus(); } });
    this.joinBtn = h("button", { class: "pill primary", id: "mv-wf-join", onclick: () => this.join() }, "Join");
    this.form = h("div", { class: "mv-wf-form" },
      h("label", { class: "mv-wf-pass" }, this.pass, this.eye),
      h("div", { class: "pills two" }, h("button", { class: "pill", onclick: () => this.pick(null) }, "Cancel"), this.joinBtn));
    this.eyeIcon();
    this.el = h("section", { class: "mv-wf", id: "mv-wf", role: "dialog", "aria-modal": "true", "aria-label": "FM-1 Wi-Fi", tabindex: "-1", hidden: true },
      h("div", { class: "mv-wf-head" }, svg(IC.wifi, 18), h("b", {}, "FM-1 Wi-Fi"), h("span", { class: "sp" }),
        h("button", { class: "mv-wf-x", "aria-label": "Close FM-1 Wi-Fi", onclick: () => this.close() }, svg(IC.close, 16))),
      h("div", { class: "mv-wf-body" }, h("h3", { class: "heading" }, "The FM-1"), this.status, this.netsHead, this.nets, this.keptHead, this.kept, this.note));
    document.body.append(this.scrim, this.el);
    this.el.addEventListener("keydown", (e) => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); this.close(); } });
  }
  eyeIcon() { this.eye.replaceChildren(svg(this.show ? IC.eyeOff : IC.eye, 16)); this.eye.setAttribute("aria-label", this.show ? "Hide the password" : "Show the password"); }

  open() {
    if (this.isOpen) return;
    this.isOpen = true; this.asked = 0; this.sig = "";
    this.scrim.hidden = false; this.el.hidden = false;
    this.el.focus({ preventScroll: true });
    this.poll();
  }
  close() {
    this.isOpen = false; clearTimeout(this.timer);
    this.scrim.hidden = true; this.el.hidden = true;
    this.pass.value = ""; this.sel = null;
  }
  toggle() { if (this.isOpen) this.close(); else this.open(); }

  async poll() {
    clearTimeout(this.timer);
    if (!this.isOpen) return;
    try { this.d = await (await fetch(API, { cache: "no-store" })).json(); } catch (_) { this.d = null; }
    if (!this.isOpen) return;
    const d = this.d;
    if (d && d.usb && d.usb_wifi && !this.asked && !(d.scan && d.scan.nets.length)) { this.asked = 1; this.scan(true); }
    this.render();
    const quick = d && (d.scan.scanning || d.job.phase === "sending" || d.job.phase === "joining" || (d.usb && !d.fm1.known));
    this.timer = setTimeout(() => this.poll(), quick ? 500 : 1500);
  }
  async post(what, body) {
    const r = await fetch(`${API}/${what}`, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body || {}) });
    let j = {}; try { j = await r.json(); } catch (_) { /* (no body) */ }
    return r.ok ? true : (j.error || `HTTP ${r.status}`);
  }
  async scan(quiet) {
    const r = await this.post("scan");
    if (r !== true && !quiet) this.toast(r, "bad");
    this.poll();
  }
  pick(n) {
    this.sel = n ? n.ssid : null;
    this.pass.value = ""; this.show = false; this.pass.type = "password"; this.eyeIcon();
    this.sig = ""; this.render();
    if (n && n.secured) setTimeout(() => this.pass.focus(), 30);
  }
  formState() {
    const n = this.selNet(), p = this.pass.value;
    this.joinBtn.disabled = !n || this.busy || (n.secured && (p.length < 8 || p.length > 63));
  }
  selNet() { return this.d && this.sel != null ? this.d.scan.nets.find((n) => n.ssid === this.sel) || null : null; }
  async join() {
    const n = this.selNet();
    if (!n || this.joinBtn.disabled) return;
    const i = this.d.scan.nets.indexOf(n);
    this.busy = true; this.formState();
    const body = { ssid: n.ssid, password: n.secured ? this.pass.value : "", pick: i, scan_seq: this.d.scan.seq };
    this.pass.value = "";                          // (the password: in this request only)
    const r = await this.post("set", body);
    body.password = "";
    this.busy = false;
    if (r !== true) { this.toast(r, "bad"); this.formState(); return; }
    this.sel = null; this.sig = "";
    this.poll();
  }
  async forget(ssid) {
    this.forgetAsk = null;
    const r = await this.post("forget", { ssid });
    if (r !== true) this.toast(r, "bad"); else this.toast(`The FM-1 forgets ${ssid}.`);
    this.sig = ""; this.poll();
  }

  // ---- the FM-1's card: on USB-A or not, its network, a join under way
  statusCard(d) {
    if (!d || !d.on) return h("div", { class: "mv-wf-need" }, "The Move's FM-1 link is off.");
    const out = [];
    const line = (dot, ...t) => h("div", { class: "mv-wf-line" }, h("span", { class: `mv-wf-dot ${dot}` }), h("span", { class: "t" }, ...t));
    if (!d.usb) {
      out.push(h("div", { class: "mv-wf-need", id: "mv-wf-need" }, svg(IC.usb, 18), h("span", {}, "Setup needs the FM-1 plugged into the Move's USB-A.")));
    } else if (!d.usb_wifi) {
      out.push(h("div", { class: "mv-wf-need", id: "mv-wf-need" }, svg(IC.usb, 18), h("span", {}, "The FM-1 on USB-A runs RemoteFM1 without Wi-Fi: install RemoteFM1 Wi-Fi on it first.")));
    }
    const f = d.fm1, card = [];
    if (d.usb && d.usb_wifi) card.push(line("ok", "On the Move's USB-A", d.usb_fw ? h("span", { class: "muted" }, ` · ${d.usb_fw}`) : null));
    if (f.known) {
      const st = STATE[f.state] || f.state;
      if (f.state === "connected") card.push(line("ok", h("b", {}, f.ssid), h("span", { class: "muted" }, ` · ${f.ip} · ${f.rssi} dBm`)));
      else card.push(line(f.state === "failed" ? "bad" : f.state === "joining" || f.state === "address" || f.state === "starting" ? "busy" : "", st, f.ssid ? h("span", { class: "muted" }, ` · ${f.ssid}`) : null, f.why ? h("span", { class: "muted" }, ` (${f.why})`) : null));
    }
    if (d.link === "wifi") card.push(line("ok", h("b", {}, "The Move plays it over Wi-Fi"), h("span", { class: "muted" }, ` · ${d.addr}`)));
    else if (d.link === "usb") card.push(line("", "The Move plays it over USB-A"));
    const j = d.job;
    if (j.phase === "sending" || j.phase === "joining") {
      card.push(line("busy", `Joining ${j.ssid}…`), h("div", { class: "mv-wf-prog" }, h("i")));
    } else if (j.phase === "connected") {
      card.push(h("div", { class: "mv-wf-done", id: "mv-wf-joined" }, svg(IC.check, 14), ` Joined ${j.ssid} as ${j.ip}.`));
      if (j.same_lan === 0) card.push(h("div", { class: "mv-wf-bad" }, "That isn't the Move's network: put the Move on it too, or the Move can't find the FM-1."));
      else if (d.link !== "wifi") card.push(h("div", {}, "The Move will find it over Wi-Fi: unplug it from USB-A."));
    } else if (j.phase === "failed" || j.phase === "timeout") {
      card.push(h("div", { class: "mv-wf-bad", id: "mv-wf-failed" }, `Didn't join ${j.ssid}: ${j.phase === "timeout" ? "no address in time" : j.why || "it could not"}. Check the password and try again.`));
    }
    if (d.err) card.push(h("div", { class: "mv-wf-bad" }, d.err));
    if (card.length) out.push(h("div", { class: "mv-wf-card" }, card));
    return out;
  }

  render() {
    const d = this.d, ready = !!(d && d.on && d.usb && d.usb_wifi);
    this.status.replaceChildren(...[].concat(this.statusCard(d)));
    const busy = d && (d.job.phase === "sending" || d.job.phase === "joining");
    const nets = d && d.on ? d.scan.nets : [], kept = d && d.on ? d.kept : [];
    // the lists: built again only when what they show changes (the password field keeps its focus and text)
    const sig = JSON.stringify([ready, busy, this.sel, d && d.scan.scanning, nets, kept, this.forgetAsk]);
    this.formState();
    if (sig === this.sig) return;
    this.sig = sig;
    const scanning = d && d.scan.scanning;
    this.netsHead.replaceChildren(h("span", {}, "Networks the FM-1 hears"),
      h("button", { class: "pill", id: "mv-wf-scan", disabled: !ready || scanning, onclick: () => this.scan(false) }, svg(IC.scan, 13), scanning ? "Looking…" : "Scan"));
    if (!ready) this.nets.replaceChildren(h("div", { class: "mv-wf-empty" }, "—"));
    else if (!nets.length) this.nets.replaceChildren(h("div", { class: "mv-wf-empty" }, scanning ? "Looking for networks…" : "No networks heard yet."));
    else this.nets.replaceChildren(...nets.map((n) => {
      const sel = n.ssid === this.sel;
      const row = h("button", { class: "mv-wf-row", role: "listitem", "data-ssid": n.ssid, disabled: busy, "aria-expanded": sel ? "true" : "false",
          onclick: () => this.pick(sel ? null : n) },
        barsEl(n.rssi), h("span", { class: "nm" }, n.ssid),
        h("span", { class: `tag ${n.joined ? "on" : ""}` }, n.joined ? "JOINED" : n.kept ? "KEPT" : ""),
        h("span", { class: "lk" }, n.secured ? svg(IC.lock, 14) : null));
      return h("div", { class: `mv-wf-net ${sel ? "sel" : ""}` }, row, sel ? this.formFor(n) : null);
    }));
    this.keptHead.hidden = this.kept.hidden = !ready;
    this.kept.replaceChildren(...(kept.length ? kept.map((k) => {
      const ask = this.forgetAsk === k.ssid;
      return h("div", { class: "mv-wf-kept", "data-ssid": k.ssid },
        h("span", { class: "nm" }, k.ssid),
        h("span", { class: `tag ${k.joined ? "on" : ""}` }, k.joined ? "joined" : k.heard ? "in range" : ""),
        ask ? h("span", { style: { display: "flex", gap: "6px" } },
                h("button", { class: "pill", onclick: () => { this.forgetAsk = null; this.render(); } }, "Keep"),
                h("button", { class: "pill danger mv-wf-forget-yes", onclick: () => this.forget(k.ssid) }, "Forget"))
            : h("button", { class: "pill mv-wf-forget", disabled: busy, onclick: () => { this.forgetAsk = k.ssid; this.render(); } }, "Forget"));
    }) : [h("div", { class: "mv-wf-empty" }, "None yet.")]));
  }
  formFor(n) {
    const pw = this.form.querySelector(".mv-wf-pass");
    pw.hidden = !n.secured;
    pw.style.display = n.secured ? "" : "none";
    this.joinBtn.textContent = n.secured ? "Join" : "Join (open network)";
    this.formState();
    return this.form;
  }
}

// the settings sheet's FM-1 Wi-Fi section: a line and the button that opens the panel
export function mountMoveWifi({ sheet, toast }) {
  const view = new WifiView({ toast });
  const btn = h("button", { class: "pill primary", id: "mv-wf-open", onclick: () => { sheet.close(); view.open(); } }, "Set up the FM-1's Wi-Fi");
  const box = h("div", {}, h("p", { class: "note" }, "With RemoteFM1 Wi-Fi on the FM-1 and the FM-1 on the Move's USB-A: pick a network for it, no computer needed."),
    h("div", { class: "pills" , style: { "grid-template-columns": "1fr" } }, btn));
  const mainPage = sheet.mainPage.bind(sheet);
  sheet.mainPage = () => {
    const out = mainPage();
    const at = out.findIndex((e) => e && e.tagName === "H3" && /^(Files|Look)/.test(e.textContent));
    out.splice(at >= 0 ? at : out.length, 0, h("h3", { class: "heading" }, "FM-1 Wi-Fi"), box);
    return out;
  };
  if (/[?&]wifi\b/.test(location.search)) setTimeout(() => view.open(), 0);
  return { view };
}
