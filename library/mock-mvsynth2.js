/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home).
 * A pretend FM-1 running M-Vsynth2, for the library page's Sandbox and for the tests: it answers firmware/src/vs_xfer.h's
 * SysEx as vs_xfer.c does (WRITE, CLEAR, PLAY, READ, IDENT, SELECT, ACK; FACTORY refused; a bad sum refused).
 * FACTORY holds the Synth2 Factory bank; U1..U8 start empty. Nothing is kept after the page closes. */
(function (G) {
  "use strict";
  const FK = G.FK;
  FK.mockDevice = function (opts) {
    const o = opts || {};
    const M = FK.mvs, NB = 8, listeners = new Set();
    const banks = [];
    for (let b = 0; b <= NB; b++) banks.push(Array.from({length: 128}, () => M.empty()));
    const fac = FK.adapter.factory()[0].slots;
    fac.forEach((p, s) => { if (p) banks[0][s] = M.pack(p); });
    if (o.seed) o.seed(banks);
    const later = (fn, ms) => setTimeout(fn, ms === undefined ? 1 : ms);
    const reply = bytes => later(() => listeners.forEach(f => f(Uint8Array.from(bytes))));
    const ack = (cmd, bb, ss, st) => reply(M.msg(0x7F, [cmd, bb, ss, st]));
    const dev = {
      banks, played: [], log: [],
      busyNext: 0,                               /* the tests: answer BUSY to the next n writes */
      send(bytes) {
        const d = Uint8Array.from(bytes);
        if (d[0] !== 0xF0 || d[1] !== 0x7D) return;
        const r = M.body(d);
        if (!r) { ack(d[4] || 0, 0, 0, 1); return; }
        const cmd = r[0];
        dev.log.push(cmd);
        if (cmd === 1) {
          const [, bb, ss] = r;
          if (dev.busyNext > 0) { dev.busyNext--; ack(1, bb, ss, 3); return; }
          if (bb < 1 || bb > NB || ss > 127 || r.length !== 3 + 138) { ack(1, bb, ss, r.length !== 3 + 138 ? 1 : 2); return; }
          banks[bb][ss] = M.unpack7(r.slice(3));
          ack(1, bb, ss, 0);
        } else if (cmd === 2) {
          const [, bb, ss] = r;
          if (bb < 1 || bb > NB || ss > 127) { ack(2, bb, ss, 2); return; }
          banks[bb][ss] = M.empty();
          ack(2, bb, ss, 0);
        } else if (cmd === 3) {
          dev.played.push(M.unpack(M.unpack7(r.slice(1))));
          ack(3, 0, 0, 0);
        } else if (cmd === 4) {
          const [, bb, ss, nn] = r;
          if (bb > NB) return;
          const end = nn ? Math.min(128, ss + nn) : 128;
          for (let s = ss; s < end; s++) later(() => listeners.forEach(f => f(Uint8Array.from(M.msg(0x11, [bb, s, ...M.pack7(banks[bb][s])])))), 1 + (s - ss));
        } else if (cmd === 5) {
          reply(M.msg(0x15, [..."M-VSYNTH2 sandbox".split("").map(c => c.charCodeAt(0)), 0, 8, NB]));
        } else if (cmd === 6) {
          ack(6, r[1], r[2], r[1] > NB ? 2 : 0);
        }
      },
      listen(f) { listeners.add(f); return () => listeners.delete(f); },
      close() { listeners.clear(); },
    };
    return dev;
  };
})(typeof window !== "undefined" ? window : globalThis);
