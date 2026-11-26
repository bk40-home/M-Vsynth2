/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home).
 * M-Vsynth2's adapter for the fm1-kit library page (third_party/fm1-kit/library/adapters/README.md says what an adapter
 * gives). The .sy1 rules are tools/sy1.py's (Synth2's ParseSy1 and ConvertLegacy: one copy of them, written into the
 * page as data by tools/gen_library_page.py); the wire is firmware/src/vs_xfer.h's SysEx.
 * Needs FK_DATA = {sy1: {lo, hi, de, tab, poly, mono, rules}, factory: [[file, text], ...]}. */
(function (G) {
  "use strict";
  const FK = G.FK;
  const T = G.FK_DATA.sy1;
  const NPARAM = 99, NAME = 16, REC = 120, EMPTY = 0x7F, ROUTING = [86, 87, 88, 89];
  const REC_ID = [...Array(NPARAM).keys()].filter(i => !ROUTING.includes(i));
  const COLORS = ["default", "red", "blue", "green", "yellow"];
  /* a favourite (issue #12, Kris 25 Nov): bit 6 of the record's last byte, under it the colour; in a .sy1 "fav=1"
   * (written only when set: Synth2 skips a key it doesn't know). The FM-1's SEQ shows it as a star and lists them */
  const FAV = 0x40;
  /* HOME's four knobs (vs_patch.h, Kris 18 Nov): a patch's .home, four parameter ids, 0x7F that knob's default. Not in
   * a .sy1 (it has no place for them): a patch from a file has the defaults. On the wire, four bytes after a record */
  const HOME_N = 4, HOME_DEF = 0x7F, REC7 = 138;
  const homeOf = p => { const h = (p && p.home) || []; return [0, 1, 2, 3].map(k => Number.isInteger(h[k]) && h[k] >= 0 && h[k] < NPARAM && !ROUTING.includes(h[k]) ? h[k] : HOME_DEF); };
  const homeDefault = p => homeOf(p).every(b => b === HOME_DEF);
  /* M-Vsynth2's own parameters (vs_patch.h vs_own_get, Kris 19 Nov): LFO1 FADE, LFO2 FADE, a byte each 0..127 (0 = no
   * fade). Not in a .sy1 either (a file's patch: 0). On the wire, after the knobs; sent only when one is set (so a
   * firmware from before them, which refuses the longer message, still takes every patch without a fade) */
  const OWN_N = 2;
  const ownOf = p => { const o = (p && p.own) || []; return [0, 1].map(k => Number.isInteger(o[k]) && o[k] >= 0 && o[k] <= 127 ? o[k] : 0); };
  const ownDefault = p => ownOf(p).every(b => b === 0);

  /* ---- .sy1, as tools/sy1.py */
  const clamp = (i, v) => Math.max(T.lo[i], Math.min(T.hi[i], v));
  const DEC = {};
  const decoder = (enc, fatal) => DEC[enc + fatal] || (DEC[enc + fatal] = new TextDecoder(enc, {fatal}));
  function asciiName(bytes) {
    let s = null;
    if (bytes.every(x => x < 128)) s = String.fromCharCode(...bytes);     /* (nearly every name: plain ASCII) */
    else for (const enc of ["utf-8", "shift_jis", "windows-1252"]) { try { s = decoder(enc, true).decode(bytes); break; } catch (e) { /* next */ } }
    if (s === null) s = decoder("windows-1252", false).decode(bytes);
    s = s.normalize("NFKD").replace(/[̀-ͯ]/g, "").replace(/[^\x20-\x7e]/g, "?").trim();
    return s.slice(0, NAME).padEnd(NAME);
  }
  function legacy(v, ver) {
    const g = {0: 1, 100: 1, 105: 2, 106: 2, 107: 4, 108: 4}[ver] || 0;
    if (!g) return v;
    const old = v.slice(), c127 = x => Math.max(0, Math.min(127, x));
    for (const [p, t, val, gs] of T.rules) if (gs & g) v[p] = t < 0 ? val : T.tab[t][c127(old[p])];
    for (const [s, t] of [[50, 44], [51, 43]]) v[s] = 64 + Math.floor(old[s] * (127 - old[t]) * 63 / (127 * 127));
    const mono = old[38] !== 0;
    v[93] = mono ? 4 : 2;
    v[75] = g === 4 ? T.tab[mono ? T.mono : T.poly][c127(old[75])] : (mono ? 70 : 48);
    return v;
  }
  const LATIN1 = new TextDecoder("latin1");
  function parse(bytes) {
    /* the first line is the name (its encoding guessed); the rest is read as Latin-1 at once */
    let e = 0;
    while (e < bytes.length && bytes[e] !== 10 && bytes[e] !== 13) e++;
    const p = {name: asciiName(bytes.subarray(0, e)), color: "default", version: 0, values: T.de.slice(), home: homeOf(null), own: ownOf(null), fav: false};
    const lines = LATIN1.decode(bytes.subarray(e)).split(/\r\n|\r|\n/);
    for (const s of lines) {
      if (!s) continue;
      const q = s.indexOf("="), c = s.indexOf(",");
      if (q >= 0) {
        const k = s.slice(0, q), v = s.slice(q + 1);
        if (k === "ver" && /^\s*-?\d+\s*$/.test(v)) p.version = +v; else if (k === "color") p.color = v;
        else if (k === "fav") p.fav = v.trim() === "1";
      } else if (c >= 0) {
        const x = s.slice(0, c), y = s.slice(c + 1);
        if (/^\s*[-+]?\d+\s*$/.test(x) && /^\s*[-+]?\d+\s*$/.test(y) && +x >= 0 && +x < NPARAM) p.values[+x] = clamp(+x, +y);
      }
    }
    p.values = legacy(p.values, p.version);
    p.version = 112;
    if (!COLORS.includes(p.color)) p.color = "default";
    return p;
  }
  function pack(p) {
    const r = new Uint8Array(REC);
    for (let i = 0; i < NAME; i++) r[i] = p.name.charCodeAt(i) || 32;
    REC_ID.forEach((id, j) => { r[NAME + j] = clamp(id, p.values[id]) - T.lo[id]; });
    ROUTING.forEach((id, k) => { const v = clamp(id, p.values[id]), w = v < 0 ? 0xFFFF : v; r[NAME + REC_ID.length + 2 * k] = w & 255; r[NAME + REC_ID.length + 2 * k + 1] = w >> 8; });
    r[REC - 1] = Math.max(0, COLORS.indexOf(p.color)) | (p.fav ? FAV : 0);
    return r;
  }
  function empty() { const r = new Uint8Array(REC).fill(0); for (let i = 0; i < NAME; i++) r[i] = 32; r[NAME] = EMPTY; return r; }
  function unpack(r, hk, ow) { /* hk: the four knob bytes after the record, ow: the own bytes after those, if sent */
    if (r[NAME] === EMPTY) return null;
    const v = new Array(NPARAM).fill(0);
    REC_ID.forEach((id, j) => { v[id] = clamp(id, r[NAME + j] + T.lo[id]); });
    ROUTING.forEach((id, k) => { const w = r[NAME + REC_ID.length + 2 * k] | r[NAME + REC_ID.length + 2 * k + 1] << 8; v[id] = w === 0xFFFF ? -1 : w; });
    return {name: String.fromCharCode(...r.slice(0, NAME)), color: COLORS[r[REC - 1] & (FAV - 1)] || "default", version: 112, values: v,
            fav: !!(r[REC - 1] & FAV),
            home: homeOf({home: hk ? Array.from(hk) : null}), own: ownOf({own: ow ? Array.from(ow) : null})};
  }
  /* a record on the wire, the knobs after it only when one is not default (so a firmware from before them, which
   * refuses the longer message, still takes every patch that does not use them) */
  const wire = p => ownDefault(p) ? (homeDefault(p) ? pack7(pack(p)) : [...pack7(pack(p)), ...homeOf(p)])
                                   : [...pack7(pack(p)), ...homeOf(p), ...ownOf(p)];
  const write = p => p.name.trimEnd() + "\ncolor=" + p.color + (p.fav ? "\nfav=1" : "") + "\nver=112\n" + p.values.map((v, i) => i + "," + v + "\n").join("");

  /* ---- the wire (vs_xfer.h) */
  const pack7 = b => { const o = []; for (let i = 0; i < b.length; i += 7) { const g = Array.from(b.slice(i, i + 7)); o.push(g.reduce((t, x, k) => t | ((x >> 7) & 1) << k, 0)); g.forEach(x => o.push(x & 127)); } return o; };
  const unpack7 = p => { const o = []; let i = 0; while (i < p.length) { const top = p[i++]; for (let k = 0; k < 7 && i < p.length; k++) o.push((p[i++] & 127) | ((top >> k) & 1) << 7); } return new Uint8Array(o); };
  function msg(cmd, data) { const s = (cmd + data.reduce((a, b) => a + b, 0)) & 127; return [0xF0, 0x7D, 0x4D, 0x56, cmd, ...data, (128 - s) & 127, 0xF7]; }
  function body(d) {     /* ours, with a good sum: cmd and data; else null */
    if (d.length < 7 || d[0] !== 0xF0 || d[1] !== 0x7D || d[2] !== 0x4D || d[3] !== 0x56 || d[d.length - 1] !== 0xF7) return null;
    let s = 0;
    for (let i = 4; i < d.length - 1; i++) s += d[i];
    return (s & 127) === 0 ? Array.from(d.slice(4, d.length - 2)) : null;
  }
  function exchange(io, bytes, until, ms) {
    return new Promise((ok, no) => {
      let off = null;
      const t = setTimeout(() => { if (off) off(); no(new Error("no answer")); }, ms || 2000);
      off = io.listen(d => { const r = body(d); if (r && until(r)) { clearTimeout(t); off(); ok(r); } });
      io.send(bytes);
    });
  }
  /* one at a time: a second request waits for the first */
  let chain = Promise.resolve();
  const serial = fn => { const r = chain.then(fn, fn); chain = r.catch(() => {}); return r; };
  const ERR = ["done", "a bad message", "refused (FACTORY, or a bank or slot out of range)", "busy"];
  function acked(io, cmd, data) {
    return serial(async () => {
      for (let n = 0; n < 50; n++) {
        const r = await exchange(io, msg(cmd, data), r => r[0] === 0x7F && r[1] === cmd);
        if (r[4] === 3) { await new Promise(z => setTimeout(z, 300)); continue; }
        if (r[4]) throw new Error(ERR[r[4]] || "error " + r[4]);
        return;
      }
      throw new Error("still busy: lift your hands off the keys and send again");
    });
  }
  function readRange(io, bank, from, n, onProgress) {
    return serial(() => new Promise((ok, no) => {
      const out = [];
      let got = 0, t = 0, off = null;
      const arm = () => { clearTimeout(t); t = setTimeout(() => { off(); no(new Error("the read stopped at " + (from + got))); }, 3000); };
      off = io.listen(d => {
        const r = body(d);
        if (!r || r[0] !== 0x11 || r[1] !== bank) return;
        const s = r[2];
        if (s < from || s >= from + n) return;
        out[s - from] = unpack(unpack7(r.slice(3, 3 + REC7)), r.length >= 3 + REC7 + HOME_N ? r.slice(3 + REC7, 3 + REC7 + HOME_N) : null,
                               r.length >= 3 + REC7 + HOME_N + OWN_N ? r.slice(3 + REC7 + HOME_N, 3 + REC7 + HOME_N + OWN_N) : null);
        got++;
        if (onProgress) onProgress(got);
        if (got >= n) { clearTimeout(t); off(); ok(out); } else arm();
      });
      arm();
      io.send(msg(4, [bank, from, n >= 128 ? 0 : n]));
    }));
  }

  /* ---- what the page shows */
  const OSC1 = ["SINE", "SAW", "PULSE", "TRI"], OSC2 = ["", "SAW", "PULSE", "TRI", "NOISE"];
  const FLT = [["LP12", "lp", 2], ["LP24", "lp", 4], ["HP12", "hp", 2], ["BP12", "bp", 2], ["LPDL", "lp", 4], ["SH101", "lp", 4]];
  const MODE = ["POLY", "MONO", "LEGATO"];
  const v127 = x => Math.max(0, Math.min(1, x / 127));

  FK.mvs = {parse, pack, unpack, write, empty, pack7, unpack7, msg, body, homeOf, ownOf, REC7};   /* (the mock and the tests use them) */

  FK.adapter = {
    id: "mvsynth2",
    name: "M-Vsynth2",
    product: /^M-VSYNTH2$/,
    deviceName: "FM-1",
    portMatch: /FM-1|M-VAVE|SMK|VSYNTH/i,
    nameLength: NAME,
    bankSize: 128,
    deviceBanks: [{id: 0, name: "FACTORY", size: 128, readOnly: true}].concat([1, 2, 3, 4, 5, 6, 7, 8].map(b => ({id: b, name: "U" + b, size: 128}))),
    fileTypes: [".sy1"],
    colours: COLORS,
    nameOf: p => p.name.trimEnd(),
    colourOf: p => p.color,
    withName: (p, n) => Object.assign({}, p, {name: n.slice(0, NAME).padEnd(NAME), values: p.values.slice()}),
    withColour: (p, c) => Object.assign({}, p, {color: c, values: p.values.slice()}),
    withFav: (p, f) => Object.assign({}, p, {fav: !!f, values: p.values.slice()}),
    fingerprint: p => p.name.trimEnd() + "|" + p.color + "|" + p.values.join(",") + "|" + homeOf(p).join(",") + "|" + ownOf(p).join(",") + (p.fav ? "|fav" : ""),
    soundKey: p => p.values.join(","),
    parseFile(name, bytes) { return /\.sy1$/i.test(name) ? [parse(bytes)] : null; },
    fileName: (p, slot) => FK.slotName(slot) + ".sy1",
    serialise: p => new TextEncoder().encode(write(p)),
    describe: p => ".sy1, ver 112" + (p && p.fav ? " · ★ favourite" : ""),
    /* the star (#12): an ordinary change to the slot shown (a device slot: pending until Send, undoable) */
    actions: [{label: "★ Favourite", title: "Star or unstar this patch: the FM-1's SEQ lists the starred ones as FAVOURITES",
               run(p, ref) {
                 if (!ref) { FK.lib.say("Open the patch in its bank or source to star it", "bad"); return; }
                 const why = FK.lib.putAt(ref, FK.adapter.withFav(p, !p.fav), (p.fav ? "Unstarred " : "Starred ") + p.name.trimEnd());
                 if (why) FK.lib.say(why, "bad");
               }}],
    preview(p) {
      const v = p.values, f = FLT[v[14]] || FLT[0];
      return {
        filter: {label: f[0] + " · " + v[19] + " · " + v[20], kind: f[1], order: f[2], freq: v127(v[19]), res: v127(v[20])},
        env: {a: v127(v[25]), d: v127(v[26]), s: v127(v[27]), r: v127(v[28])},
        facts: [["OSC 1", OSC1[v[0]] || "?"], ["OSC 2", OSC2[v[1]] || "?"], ["MIX", String(v[5])], ["MODE", MODE[v[38]] || "?"],
                ["UNISON", v[73] ? "ON " + v[93] : "OFF"], ["ARP", v[59] ? "ON" : "OFF"], ["DELAY", v[65] ? "ON" : "OFF"], ["CHORUS", v[66] ? "ON" : "OFF"]],
      };
    },
    factory() {
      const files = (G.FK_DATA.factory || []).filter(f => /\.sy1$/i.test(f[0]));
      const enc = new TextEncoder(), pats = files.map(f => parse(enc.encode(f[1])));
      const at = FK.placeByNumber(files.map(f => f[0]), 128), slots = new Array(128).fill(null);
      pats.forEach((p, i) => { if (at[i] < 128) slots[at[i]] = p; });
      return [{name: "Synth2 Factory", slots}];
    },
    device: {
      async identify(io) {
        const r = await serial(() => exchange(io, msg(5, []), r => r[0] === 0x15));
        const z = r.indexOf(0, 1), text = String.fromCharCode(...r.slice(1, z < 0 ? r.length : z)).trim();
        const sp = text.indexOf(" ");
        return {product: sp < 0 ? text : text.slice(0, sp), version: sp < 0 ? "" : text.slice(sp + 1), voices: r[z + 1], banks: r[z + 2]};
      },
      read: (io, bank, size, onProgress) => readRange(io, bank, 0, size, onProgress),
      readSlot: async (io, bank, slot) => (await readRange(io, bank, slot, 1))[0] || null,
      write: (io, bank, slot, p) => acked(io, 1, [bank, slot, ...wire(p)]),
      clear: (io, bank, slot) => acked(io, 2, [bank, slot]),
      audition: (io, p) => acked(io, 3, wire(p)),
    },
    mock: () => FK.mockDevice(),
  };
})(typeof window !== "undefined" ? window : globalThis);
