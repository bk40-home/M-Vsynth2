// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-Vsynth2 simulator: the sim and an FM-1 linked, the same sound on both, both ways (as M-vance's bank/link.js).
// Both run the same firmware, so both speak vs_xfer.h's link:
//   LINK  07       the device answers with EDIT (what plays) and from then on reports every change as an EDIT,
//                  for 30 s after the last LINK (renewed here every RENEW_MS)
//   EDIT  17 rec   what plays on the device (the 138-byte packed record, as the patch library's DATA)
//   PLAY  03 rec   sets what plays on a device, acknowledged (ACK 7F 03 00 00 st); the device then knows the page
//                  has it, so it is not reported back
// The page keeps `cur` (the sound both should have) and, a device at a time, what it knows each has:
//   adopt   linking, the FM-1's first EDIT becomes `cur` (the FM-1 leads) and goes to the sim
//   report  a device's EDIT (an edit there) becomes `cur` and goes to the other device
//   push    tick(): a device marked stale gets PLAY(cur), one at a time (the next after its ACK)
//   renew   every RENEW_MS: LINK again; its dump is taken as a report (an edit made meanwhile is kept, not undone)
// No DOM here: sim.js drives it, sim/link-test.mjs tests it with pretend devices.

export const RENEW_MS = 20000;            // the firmware's link lapses at 30 s (vs_xfer.h VS_XF_LINK_HOLD)
export const ACK_MS = 600;                // a PLAY not acknowledged by then is sent again
export const ADOPT_MS = 2000;             // no EDIT from the FM-1 by then: it does not link (old firmware)
export const CMD = { PLAY: 0x03, IDENT: 0x05, LINK: 0x07, IDREP: 0x15, EDIT: 0x17, ACK: 0x7f };

export function frame(cmd, data = []) {   // F0 7D 4D 56 cmd data sum F7
  const m = new Uint8Array(7 + data.length);
  m.set([0xf0, 0x7d, 0x4d, 0x56, cmd]);
  m.set(data, 5);
  let s = cmd;
  for (const b of data) s += b;
  m[5 + data.length] = (128 - (s & 127)) & 127;
  m[6 + data.length] = 0xf7;
  return m;
}
export function parse(m) {                // -> { cmd, data } for one of ours with a good sum, else null
  if (!m || m.length < 7 || m[0] !== 0xf0 || m[1] !== 0x7d || m[2] !== 0x4d || m[3] !== 0x56 || m[m.length - 1] !== 0xf7) return null;
  let s = 0;
  for (let i = 4; i < m.length - 2; i++) s += m[i];
  if (((128 - (s & 127)) & 127) !== m[m.length - 2]) return null;
  return { cmd: m[4], data: m.subarray(5, m.length - 2) };
}
export function unpack7(p) {              // the record's 7-bit packing (vs_xfer.c vs_xfer_unpack7)
  const out = [];
  for (let i = 0; i < p.length; ) {
    const top = p[i++];
    for (let k = 0; k < 7 && i < p.length; k++) out.push((p[i++] & 0x7f) | (((top >> k) & 1) << 7));
  }
  return out;
}
export function nameOf(rec7) {            // a record's name (its first 16 bytes), trimmed
  return String.fromCharCode(...unpack7(rec7).slice(0, 16)).replace(/[\x00\s]+$/, '');
}
const keyOf = (rec7) => Array.prototype.join.call(rec7, ',');

class Dev {
  constructor(name, send) {
    Object.assign(this, { name, send, has: null, stale: false, waitAck: false, ackAt: 0, linked: false });
  }
}

export class Link {
  // simSend(msg): into the sim; fm1Send(msg): to the FM-1. onChange(why): 'adopt' | 'edit' | 'nolink'
  constructor({ simSend, fm1Send, onChange = () => {}, now = () => performance.now() }) {
    this.now = now; this.onChange = onChange;
    this.sim = new Dev('sim', simSend);
    this.fm1 = new Dev('fm1', fm1Send);
    this.cur = null; this.curKey = null;
    this.adopting = true; this.t0 = now(); this.renewAt = now() + RENEW_MS;
    this.fm1.linked = true;
    this.fm1.send(frame(CMD.LINK));           // the FM-1's sound comes, and becomes the sim's (adopt)
  }
  get name() { return this.cur ? nameOf(this.cur) : ''; }

  fromSim(m) { this.from(this.sim, m); }
  fromFm1(m) { this.from(this.fm1, m); }
  from(dev, m) {
    const p = parse(m);
    if (!p) return;
    if (p.cmd === CMD.ACK && p.data[0] === CMD.PLAY) { dev.waitAck = false; if (p.data[3]) dev.stale = true; return; }
    if (p.cmd === CMD.ACK && p.data[0] === CMD.LINK && dev === this.fm1) { this.adopting = false; this.onChange('nolink'); return; }
    if (p.cmd !== CMD.EDIT) return;
    const rec = new Uint8Array(p.data), key = keyOf(rec);
    if (this.adopting) {
      if (dev !== this.fm1) return;             // (the sim's sound is replaced)
      this.adopting = false;
      this.fm1.has = key;
      this.set(rec, key, this.fm1, 'adopt');
      return;
    }
    dev.has = key;
    if (dev.waitAck) dev.stale = true;          // it reported before taking our push: that push will overwrite this edit
    if (key === this.curKey) return;
    this.set(rec, key, dev, 'edit');
  }
  set(rec, key, from, why) {
    this.cur = rec; this.curKey = key;
    for (const d of [this.sim, this.fm1]) if (d !== from && d.has !== key) d.stale = true;
    this.onChange(why);
  }

  // every ~30 ms: pushes, ACK timeouts, renewals, the sim's link once it has the sound
  tick() {
    const t = this.now();
    if (this.adopting && t - this.t0 > ADOPT_MS) { this.adopting = false; this.onChange('nolink'); }
    for (const d of [this.sim, this.fm1]) {
      if (d.waitAck && t - d.ackAt > ACK_MS) { d.waitAck = false; d.stale = true; }
      if (d.stale && !d.waitAck && this.cur) {
        d.send(frame(CMD.PLAY, this.cur));
        d.has = this.curKey; d.stale = false; d.waitAck = true; d.ackAt = t;
      }
    }
    if (this.cur && !this.sim.linked && !this.sim.stale) {   // the sim reports its edits from now (after the PLAY)
      this.sim.linked = true;
      this.sim.send(frame(CMD.LINK));
    }
    if (t >= this.renewAt) {
      this.renewAt = t + RENEW_MS;
      for (const d of [this.sim, this.fm1]) if (d.linked) d.send(frame(CMD.LINK));
    }
  }
}
