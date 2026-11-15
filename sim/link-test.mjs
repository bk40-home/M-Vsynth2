// SPDX-License-Identifier: GPL-3.0-only
// sim/link.js with two pretend devices that behave as vs_xfer.c's link does (LINK -> EDIT and reports, PLAY ->
// ACK and no echo). Run: node sim/link-test.mjs   (exit 0 = all passed)
import { Link, frame, parse, CMD, ACK_MS, ADOPT_MS, RENEW_MS, nameOf } from './link.js';

let fails = 0, checks = 0, t = 0;
const ok = (c, what) => { checks++; if (!c) { fails++; console.log('FAIL ' + what); } };

const rec = (name, v = 0) => {             // a packed record: a name and one value (enough for the page: it never decodes more)
  const raw = new Array(120).fill(0);
  for (let i = 0; i < 16; i++) raw[i] = (name.charCodeAt(i) || 32) & 0x7f;
  raw[40] = v & 0x7f;
  const out = [];
  for (let i = 0; i < raw.length; i += 7) { out.push(0); for (let k = 0; k < 7 && i + k < raw.length; k++) out.push(raw[i + k]); }
  return Uint8Array.from(out);
};
const same = (a, b) => a && b && a.length === b.length && a.every((x, i) => x === b[i]);

class FakeDev {                            // the firmware's side (vs_xfer.c)
  constructor(sound, { oldFirmware = false, dropPlay = 0 } = {}) {
    Object.assign(this, { sound, has: null, linkReq: false, linked: false, inbox: [], out: null, oldFirmware, dropPlay, plays: 0, edits: 0 });
  }
  receive(m) { this.inbox.push(m); }
  step() {                                 // one main-loop pass: take messages, then the link's report
    for (const m of this.inbox.splice(0)) {
      const p = parse(m);
      if (p.cmd === CMD.LINK) {
        if (this.oldFirmware) { this.out(frame(CMD.ACK, [CMD.LINK, 0, 0, 1])); continue; }
        this.linkReq = true;
      } else if (p.cmd === CMD.PLAY) {
        if (this.dropPlay > 0) { this.dropPlay--; continue; }   // lost on the way
        this.sound = new Uint8Array(p.data); this.plays++;
        if (this.linked) this.has = this.sound;
        this.out(frame(CMD.ACK, [CMD.PLAY, 0, 0, 0]));
      }
    }
    if (this.linkReq || (this.linked && !same(this.sound, this.has))) {
      this.linkReq = false; this.linked = true; this.has = this.sound; this.edits++;
      this.out(frame(CMD.EDIT, this.sound));
    }
  }
}

function setup(simSound, fm1Sound, opts = {}) {
  const sim = new FakeDev(simSound), fm1 = new FakeDev(fm1Sound, opts);
  const why = [];
  const q = [];                            // messages in flight (delivered in order)
  const link = new Link({ simSend: (m) => q.push(() => sim.receive(m)), fm1Send: (m) => q.push(() => fm1.receive(m)),
                          onChange: (w) => why.push(w), now: () => t });
  sim.out = (m) => q.push(() => link.fromSim(m));
  fm1.out = (m) => q.push(() => link.fromFm1(m));
  const run = (ms) => { for (let i = 0; i < ms / 10; i++) { t += 10; while (q.length) q.shift()(); sim.step(); fm1.step(); while (q.length) q.shift()(); link.tick(); } };
  return { sim, fm1, link, why, run };
}

{ // adopt: the FM-1's sound comes to the sim; the sim then linked
  const { sim, fm1, link, why, run } = setup(rec('SIM'), rec('FM1 PAD', 5));
  run(200);
  ok(why[0] === 'adopt' && link.name === 'FM1 PAD', "linking adopts the FM-1's sound");
  ok(same(sim.sound, fm1.sound), 'the sim plays it');
  ok(sim.linked, 'the sim is linked after it has the sound');
  const e = sim.edits + fm1.edits, p = sim.plays + fm1.plays;
  run(1000);
  ok(sim.edits + fm1.edits === e && sim.plays + fm1.plays === p, 'then quiet: no echo loop');

  sim.sound = rec('FM1 PAD', 40); run(200);
  ok(same(fm1.sound, sim.sound), 'a knob turned in the sim reaches the FM-1');
  fm1.sound = rec('FM1 PAD', 77); run(200);
  ok(same(sim.sound, fm1.sound), 'a knob turned on the FM-1 shows in the sim');
  const e2 = sim.edits + fm1.edits; run(1000);
  ok(sim.edits + fm1.edits === e2, 'still no echo');

  sim.sound = rec('FM1 PAD', 11); fm1.sound = rec('FM1 PAD', 99); run(500);   // both turned at once
  ok(same(sim.sound, fm1.sound), 'edits on both at once: they end the same');

  let links = 0; const s0 = fm1.receive.bind(fm1); fm1.receive = (m) => { if (parse(m).cmd === CMD.LINK) links++; s0(m); };
  run(RENEW_MS + 100);
  ok(links === 1, 'the link is renewed');
}
{ // a lost PLAY is sent again
  const { sim, fm1, run } = setup(rec('SIM'), rec('FM1'), {});
  run(200);
  fm1.dropPlay = 1; sim.sound = rec('FM1', 60); run(100);
  ok(!same(fm1.sound, sim.sound), '(the PLAY was lost)');
  run(ACK_MS + 100);
  ok(same(fm1.sound, sim.sound), 'not acknowledged: sent again, and it arrives');
}
{ // an FM-1 without the link (older M-Vsynth2): said so
  const { why, run } = setup(rec('SIM'), rec('FM1'), { oldFirmware: true });
  run(100);
  ok(why.includes('nolink'), 'an FM-1 that refuses LINK: reported');
}
{ // an FM-1 that never answers
  const { fm1, why, run } = setup(rec('SIM'), rec('FM1'));
  fm1.step = () => { fm1.inbox.length = 0; };
  run(ADOPT_MS + 100);
  ok(why.includes('nolink'), 'no answer: reported');
}
ok(nameOf(rec('BRASS 1')) === 'BRASS 1', 'the name read from a record');

console.log(`link-test: ${checks} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
