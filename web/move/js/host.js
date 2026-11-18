// SPDX-License-Identifier: GPL-3.0-only
// Who serves the page (index.html's <meta name="fm1-host">, which its first script also puts on <html data-host>):
//   "web"   remotefm1.com and its local copies: the engines in the browser (AudioWorklet), Web MIDI, WebUSB, saves
//   "move"  an Ableton Move running RemoteFM14Move (http://remotefm1.local:7730/): the engine is the firmware on the
//           Move (js/movehost.js); nothing that needs a secure page (plain http on a .local name is not one)
export const HOST = (typeof document !== "undefined" && document.querySelector('meta[name="fm1-host"]')?.content) || "web";
export const ON_MOVE = HOST === "move";
