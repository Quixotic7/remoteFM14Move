// SPDX-License-Identifier: GPL-3.0-only
// Listen here on RemoteFM1's page (RemoteFM14Move; README "Listening on the page"): the Move's whole output, streamed
// by the host (src/listen.c, web.c /ws/audio), played on this device (an iPad or a phone with AirPods), the Move's
// own output silent meanwhile unless this listener keeps its speaker on.
//   - the header: a Listen cap (headphones; a dot: off, connecting, on, amber while it keeps running dry) and beside
//     it the Move's speaker (on / off for this listener; the Move is silent if any listener has it off; remembered)
//   - the tap (a user gesture, as iOS wants) creates or resumes an AudioContext; the stream plays through a jitter
//     buffer (ListenCore: ~150 ms to start, more after each run dry, up to 500 ms; a little faster when too far behind,
//     a jump when far too far), resampled linearly to the context's rate (Safari may run at 48000). In an AudioWorklet
//     on a secure page (https, localhost); on the Move's plain http page (no AudioWorklet there) a ScriptProcessor
//     runs the same ListenCore
//   - the socket: binary frames (12 bytes: 'L', 1, frames u16, number u32, rate u32; then stereo int16), text state
//     messages; "hb" each second (the host lets a listener go after 3 s without one), "spk 0" / "spk 1"
//   - Listen off, the page hidden or left (visibilitychange, pagehide): the socket closes, the Move's sound comes back
// Nothing here runs unless js/host.js ON_MOVE (js/moveslots.js mountMoveUi mounts it). window.FM1Listen: the state
// and the counters (the e2e test's).
import { h, svg } from "./moveui.js";

const CSS = `
.mv-lis { display: inline-flex; align-items: center; gap: 4px; flex: none; }
.mv-lis .mv-lisbtn { position: relative; }
.mv-lis .mv-lisbtn .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--muted, #8a8580); flex: none; }
.mv-lis[data-st="connecting"] .dot { background: #f5a524; animation: mv-lis-p 0.9s ease-in-out infinite; }
.mv-lis[data-st="on"] .dot { background: var(--green, #3bd47a); }
.mv-lis[data-st="on"][data-dry="1"] .dot { background: #f5a524; }
.mv-lis[data-st="on"] .mv-lisbtn, .mv-lis[data-st="connecting"] .mv-lisbtn { background: var(--bezel); color: var(--text); }
.mv-lis .mv-spkbtn { width: 34px; padding: 0; justify-content: center; }
.mv-lis .mv-spkbtn[aria-pressed="false"] { opacity: .75; }
.mv-lis[data-st="off"] .mv-spkbtn { display: none !important; }
@keyframes mv-lis-p { 50% { opacity: .35; } }
@media (prefers-reduced-motion: reduce) { .mv-lis .dot { animation: none !important; } }
@media (max-width: 1400px) { .mv-lis .mv-lisbtn > span:not(.mv-ic):not(.dot) { display: none; } .mv-lis .mv-lisbtn { padding: 0 10px; } }
@media (max-width: 640px) { .mv-lis .mv-lisbtn, .mv-lis .mv-spkbtn { display: inline-flex !important; height: 34px; } }
@media (max-width: 640px) { .mv-lis[data-st="off"] .mv-spkbtn { display: none !important; } }
`;
const IC = {
  phones: '<path d="M2.5 10V8a5.5 5.5 0 0111 0v2"/><rect x="2" y="9.5" width="3" height="4.5" rx="1.2"/><rect x="11" y="9.5" width="3" height="4.5" rx="1.2"/>',
  spk: '<path d="M2 6h2.5L8 3v10L4.5 10H2z"/><path d="M10.5 5.5a3.5 3.5 0 010 5M12.5 3.5a6.3 6.3 0 010 9"/>',
  spkOff: '<path d="M2 6h2.5L8 3v10L4.5 10H2z"/><path d="M10.5 6l4 4M14.5 6l-4 4"/>',
};

// the jitter buffer and the resampler: one class, run in the AudioWorklet (its source text) or in a ScriptProcessor
export class ListenCore {
  constructor(outRate) {
    this.inRate = 44100;
    this.base = this.inRate / outRate;
    this.CAP = 65536;                              // frames (1.5 s)
    this.buf = new Float32Array(this.CAP * 2);
    this.minT = Math.round(0.15 * this.inRate);
    this.maxT = Math.round(0.5 * this.inRate);
    this.reset();
    this.under = 0; this.nonzero = 0; this.frames = 0; this.jumps = 0;
  }
  reset() { this.w = 0; this.r = 0; this.playing = false; this.target = this.minT; this.good = 0; }
  push(pcm) {                                      // stereo int16, interleaved
    const n = pcm.length >> 1, M = this.CAP - 1, b = this.buf;
    if (this.w + n - this.r > this.CAP - 16) { this.r = this.w + n - this.target; this.jumps++; }
    for (let k = 0; k < n; k++) {
      const a = ((this.w + k) & M) * 2, l = pcm[2 * k], r = pcm[2 * k + 1];
      if (l || r) this.nonzero++;
      b[a] = l / 32768; b[a + 1] = r / 32768;
    }
    this.w += n; this.frames += n;
  }
  process(L, R) {
    const n = L.length, M = this.CAP - 1, b = this.buf;
    let avail = this.w - this.r;
    if (avail > this.target + 0.4 * this.inRate) { this.r = this.w - this.target; this.jumps++; avail = this.target; }
    const step = avail > this.target + 0.06 * this.inRate ? this.base * 1.02 : this.base;   // (catching up: 2 % faster)
    for (let i = 0; i < n; i++) {
      avail = this.w - this.r;
      if (!this.playing) {
        if (avail < this.target) { L[i] = 0; R[i] = 0; continue; }
        this.playing = true;
      }
      if (avail < 2) {                             // ran dry: silence, wait for more, a deeper buffer from now on
        this.playing = false; this.under++; this.good = 0;
        this.target = Math.min(this.maxT, this.target + Math.round(0.05 * this.inRate));
        L[i] = 0; R[i] = 0; continue;
      }
      const i0 = Math.floor(this.r), f = this.r - i0, a = (i0 & M) * 2, c = ((i0 + 1) & M) * 2;
      L[i] = b[a] + (b[c] - b[a]) * f;
      R[i] = b[a + 1] + (b[c + 1] - b[a + 1]) * f;
      this.r += step;
    }
    if ((this.good += n) > 20 * this.inRate / this.base && this.target > this.minT) {   // 20 s without running dry
      this.target = Math.max(this.minT, this.target - Math.round(0.02 * this.inRate)); this.good = 0;
    }
  }
  stats() {
    return { ms: Math.round((this.w - this.r) / 44.1), target: Math.round(this.target / 44.1), under: this.under,
      nonzero: this.nonzero, frames: this.frames, jumps: this.jumps, playing: this.playing };
  }
}
const WORKLET = `${ListenCore.toString().replace(/^export /, "")}
registerProcessor("fm1-listen", class extends AudioWorkletProcessor {
  constructor() {
    super(); this.core = new ListenCore(sampleRate); this.t = 0;
    this.port.onmessage = (e) => { if (e.data === "reset") this.core.reset(); else this.core.push(new Int16Array(e.data)); };
  }
  process(_, outs) {
    const o = outs[0]; this.core.process(o[0], o[1] || o[0]);
    if ((this.t += o[0].length) >= sampleRate / 4) { this.t = 0; this.port.postMessage(this.core.stats()); }
    return true;
  }
});`;

const KEY = "fm1.listen.spk";
const load = () => { try { return localStorage.getItem(KEY) === "1"; } catch (_) { return false; } };
const save = (v) => { try { localStorage.setItem(KEY, v ? "1" : "0"); } catch (_) { /* (private mode) */ } };

export function mountMoveListen({ toast } = {}) {
  if (!document.getElementById("mv-lis-css")) document.head.append(h("style", { id: "mv-lis-css" }, CSS));
  const forceSp = /[?&]listen=sp\b/.test(location.search);   // (tests: the ScriptProcessor path on a secure page)
  const L = window.FM1Listen = { state: "off", spk: load(), muted: 0, listeners: 0, stats: null, path: "", frames: 0, gaps: 0 };
  let ctx = null, node = null, core = null, ws = null, hb = 0, retry = 0, lastSeq = -1, dryAt = 0, want = false;

  const label = h("span", {}, "Listen");
  const lisBtn = h("button", { class: "cap mv-mixbtn mv-lisbtn", id: "mv-lisbtn", "aria-pressed": "false", title: "Listen here",
    onclick: () => (want ? stop() : start()) }, svg(IC.phones, 15), label, h("span", { class: "dot", "aria-hidden": "true" }));
  const spkBtn = h("button", { class: "cap mv-mixbtn mv-spkbtn", id: "mv-spkbtn", onclick: () => setSpk(!L.spk) });
  const el = h("span", { class: "mv-lis", id: "mv-lis", "data-st": "off" }, lisBtn, spkBtn);

  function render() {
    el.dataset.st = L.state;
    el.dataset.dry = Date.now() - dryAt < 5000 ? "1" : "0";
    lisBtn.setAttribute("aria-pressed", L.state === "off" ? "false" : "true");
    lisBtn.title = L.state === "off" ? "Listen here" : L.state === "connecting" ? "Listen: connecting" :
      `Listen: on${L.stats ? ` · ${L.stats.ms} ms` : ""}${el.dataset.dry === "1" ? " · running dry" : ""}`;
    spkBtn.setAttribute("aria-pressed", L.spk ? "true" : "false");
    spkBtn.title = `Move speaker: ${L.spk ? "on" : "off"}${!L.spk || !L.muted ? "" : " (another listener has it off)"}`;
    spkBtn.setAttribute("aria-label", spkBtn.title);
    spkBtn.replaceChildren(svg(L.spk ? IC.spk : IC.spkOff, 15));
  }
  function setSpk(v) {
    L.spk = v; save(v);
    if (ws && ws.readyState === 1) ws.send(v ? "spk 1" : "spk 0");
    render();
  }
  function stats(s) {
    const was = L.stats ? L.stats.under : 0;
    L.stats = s;
    if (s.under > was) dryAt = Date.now();
    render();
  }

  async function audio() {   // (inside the tap: iOS lets audio start only from a gesture)
    if (!ctx) {
      const AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) throw new Error("no Web Audio here");
      try { if (navigator.audioSession) navigator.audioSession.type = "playback"; } catch (_) { /* (Safari 16.4+: past the silent switch) */ }
      ctx = new AC({ latencyHint: "interactive" });
    }
    const resumed = ctx.state === "running" ? null : ctx.resume();
    if (!node) {
      if (ctx.audioWorklet && window.isSecureContext && !forceSp) {
        const url = URL.createObjectURL(new Blob([WORKLET], { type: "text/javascript" }));
        try { await ctx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
        node = new AudioWorkletNode(ctx, "fm1-listen", { numberOfInputs: 0, outputChannelCount: [2] });
        node.port.onmessage = (e) => stats(e.data);
        L.path = "worklet";
      } else {   // (the Move's plain http page: no AudioWorklet outside a secure context)
        core = new ListenCore(ctx.sampleRate);
        node = ctx.createScriptProcessor(2048, 1, 2);
        let t = 0;
        node.onaudioprocess = (e) => {
          const o = e.outputBuffer;
          core.process(o.getChannelData(0), o.getChannelData(1));
          if ((t += o.length) >= ctx.sampleRate / 4) { t = 0; stats(core.stats()); }
        };
        L.path = "scriptprocessor";
      }
      node.connect(ctx.destination);
    }
    if (resumed) await resumed;
    L.rate = ctx.sampleRate;
  }
  function feed(buf) {
    const d = new DataView(buf);
    if (buf.byteLength < 12 || d.getUint8(0) !== 76) return;
    const seq = d.getUint32(4, true);
    if (lastSeq >= 0 && seq !== ((lastSeq + 1) >>> 0)) L.gaps++;
    lastSeq = seq; L.frames++;
    const pcm = buf.slice(12);
    if (core) core.push(new Int16Array(pcm));
    else node.port.postMessage(pcm, [pcm]);
  }
  function connect() {
    clearTimeout(retry);
    const url = `${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws/audio?spk=${L.spk ? 1 : 0}`;
    const s = ws = new WebSocket(url);
    s.binaryType = "arraybuffer";
    s.onopen = () => { if (ws === s) { L.state = "on"; render(); } };
    s.onmessage = (e) => {
      if (ws !== s) return;
      if (typeof e.data !== "string") { feed(e.data); return; }
      try { const m = JSON.parse(e.data); if (m.type === "listen") { L.muted = m.muted; L.listeners = m.listeners; render(); } } catch (_) { /* */ }
    };
    s.onclose = () => {
      if (ws !== s) return;
      ws = null;
      if (want) { L.state = "connecting"; render(); retry = setTimeout(connect, 1000); }   // (the Move restarting, Wi-Fi)
    };
  }
  async function start() {
    want = true; L.state = "connecting"; render();
    try { await audio(); } catch (e) {
      want = false; L.state = "off"; render();
      if (toast) toast(`Listen: ${e && e.message ? e.message : e}`, "bad");
      return;
    }
    if (!want) return;
    if (core) core.reset(); else node.port.postMessage("reset");
    lastSeq = -1;
    connect();
    clearInterval(hb);
    hb = setInterval(() => { if (ws && ws.readyState === 1) ws.send("hb"); }, 1000);
  }
  function stop() {
    want = false;
    clearTimeout(retry); clearInterval(hb);
    if (ws) { const s = ws; ws = null; try { s.close(); } catch (_) { /* */ } }
    if (ctx && ctx.state === "running") ctx.suspend().catch(() => {});
    L.state = "off"; L.stats = null; render();
  }
  document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden" && want) stop(); });
  window.addEventListener("pagehide", () => { if (want) stop(); });
  setInterval(() => { if (want) render(); }, 1000);   // (the amber dot fades 5 s after the last run dry)
  render();
  return { el, start, stop, setSpk, state: L };
}
