// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home).
// M-Vsynth2's library adapter, in Node: every factory .sy1 through the page's parser and record packer against
// tools/sy1.py's own, byte for byte; the transfer against the pretend FM-1 (library/mock-mvsynth2.js).
//   node tests/library_test.js build/unit/library_data.json     (tests/run_unit.sh makes the json)
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const ROOT = path.join(__dirname, "..");
const data = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
globalThis.FK_DATA = data;
require(path.join(ROOT, "third_party/fm1-kit/library/model.js"));
require(path.join(ROOT, "third_party/fm1-kit/library/zip.js"));
require(path.join(ROOT, "library/adapter-mvsynth2.js"));
require(path.join(ROOT, "library/mock-mvsynth2.js"));
const FK = globalThis.FK, A = FK.adapter, M = FK.mvs;
const hex = b => Buffer.from(b).toString("hex");
let n = 0;

/* 1. the files: as tools/sy1.py reads and writes them */
const enc = new TextEncoder();
for (const [file, text] of data.factory) {
  const p = A.parseFile(file, enc.encode(text))[0];
  assert.equal(hex(M.pack(p)), data.check.pack[file], file + ": the record differs from tools/sy1.py's");
  assert.equal(M.write(p), data.check.write[file], file + ": the text differs from tools/sy1.py's");
  const back = M.unpack(M.pack(p));
  assert.equal(A.fingerprint(back), A.fingerprint(p), file + ": record round trip");
  n++;
}
for (const [file, text] of data.check.legacy || []) {   /* old versions, converted as Synth2 does */
  const p = A.parseFile(file, enc.encode(text))[0];
  assert.equal(hex(M.pack(p)), data.check.legacyPack[file], file + ": legacy conversion differs");
  n++;
}
const fac = A.factory()[0].slots;
assert.equal(fac.filter(Boolean).length, data.factory.length);

/* 2. the wire, against the pretend FM-1 */
(async () => {
  const dev = A.mock();
  const id = await A.device.identify(dev);
  assert.equal(id.product, "M-VSYNTH2"); assert.ok(A.product.test(id.product)); n++;
  const f0 = await A.device.read(dev, 0, 128);
  assert.equal(f0.filter(Boolean).length, fac.filter(Boolean).length);
  fac.forEach((p, s) => assert.equal(p ? A.fingerprint(f0[s]) : null, p ? A.fingerprint(p) : null)); n++;
  const pats = fac.filter(Boolean).slice(0, 40);
  for (let s = 0; s < pats.length; s++) await A.device.write(dev, 3, s + 10, pats[s]);
  const u3 = await A.device.read(dev, 3, 128);
  pats.forEach((p, s) => assert.equal(A.fingerprint(u3[s + 10]), A.fingerprint(p)));
  assert.equal(u3.filter(Boolean).length, 40); n++;
  await A.device.clear(dev, 3, 10);
  assert.equal(await A.device.readSlot(dev, 3, 10), null);
  assert.equal(A.fingerprint(await A.device.readSlot(dev, 3, 11)), A.fingerprint(pats[1])); n++;
  await assert.rejects(A.device.write(dev, 0, 0, pats[0]), /refused/); n++;
  dev.busyNext = 2;
  await A.device.write(dev, 4, 0, pats[2]);
  assert.equal(A.fingerprint(await A.device.readSlot(dev, 4, 0)), A.fingerprint(pats[2])); n++;
  await A.device.audition(dev, pats[3]);
  assert.equal(A.fingerprint(dev.played[0]), A.fingerprint(pats[3])); n++;
  /* a renamed, recoloured patch survives the trip */
  const q = A.withColour(A.withName(pats[4], "My Own Pad 16chr"), "yellow");
  await A.device.write(dev, 5, 127, q);
  const q2 = await A.device.readSlot(dev, 5, 127);
  assert.equal(A.nameOf(q2), "My Own Pad 16chr"); assert.equal(q2.color, "yellow"); n++;
  /* HOME's knobs (vs_patch.h): sent only when not default, read back with the patch, played with the audition */
  const k = Object.assign({}, pats[5], {home: [43, 0x7F, 86, 5]});   /* (86, a routing word: the default) */
  assert.deepEqual(M.homeOf(k), [43, 0x7F, 0x7F, 5]);
  dev.log.length = 0;
  await A.device.write(dev, 6, 1, k);
  assert.deepEqual(dev.homes[6][1], [43, 0x7F, 0x7F, 5]);
  assert.deepEqual((await A.device.readSlot(dev, 6, 1)).home, [43, 0x7F, 0x7F, 5]); n++;
  await A.device.write(dev, 6, 2, pats[5]);
  assert.deepEqual(dev.homes[6][2], [0x7F, 0x7F, 0x7F, 0x7F]);
  assert.deepEqual((await A.device.readSlot(dev, 6, 2)).home, [0x7F, 0x7F, 0x7F, 0x7F]); n++;
  assert.notEqual(A.fingerprint(k), A.fingerprint(pats[5]));
  assert.equal(A.fingerprint(Object.assign({}, pats[5], {home: undefined})), A.fingerprint(pats[5])); n++;
  await A.device.audition(dev, k);
  assert.deepEqual(dev.played[1].home, [43, 0x7F, 0x7F, 5]); n++;
  /* LFO FADE (the own parameters): sent only when set, read back, kept by a fingerprint, auditioned */
  const fd = Object.assign({}, pats[5], {own: [90, 200]});   /* (200: out of range, 0) */
  assert.deepEqual(M.ownOf(fd), [90, 0]);
  await A.device.write(dev, 6, 3, fd);
  assert.deepEqual(dev.owns[6][3], [90, 0]); assert.deepEqual(dev.homes[6][3], [0x7F, 0x7F, 0x7F, 0x7F]);
  assert.deepEqual((await A.device.readSlot(dev, 6, 3)).own, [90, 0]); n++;
  assert.deepEqual((await A.device.readSlot(dev, 6, 2)).own, [0, 0]); n++;
  assert.notEqual(A.fingerprint(fd), A.fingerprint(pats[5]));
  assert.equal(A.fingerprint(Object.assign({}, pats[5], {own: undefined})), A.fingerprint(pats[5])); n++;
  await A.device.audition(dev, fd);
  assert.deepEqual(dev.played[2].own, [90, 0]); n++;
  /* the favourite's star (#12): bit 6 of the colour byte, as tools/sy1.py; on the wire, a fingerprint, a .sy1 */
  const st = A.withFav(pats[6], true);
  assert.equal(M.pack(st)[119], M.pack(pats[6])[119] | 0x40);
  await A.device.write(dev, 6, 4, st);
  const st2 = await A.device.readSlot(dev, 6, 4);
  assert.ok(st2.fav); assert.equal(st2.color, pats[6].color); assert.equal(A.fingerprint(st2), A.fingerprint(st));
  assert.notEqual(A.fingerprint(st), A.fingerprint(pats[6]));
  assert.ok(A.parseFile("x.sy1", A.serialise(st))[0].fav); assert.ok(!/fav/.test(M.write(pats[6]))); n++;
  /* a bank out as a zip of NNN.sy1 and back in */
  const z = FK.zip(u3.map((p, s) => p && {name: A.fileName(p, s), data: A.serialise(p)}).filter(Boolean));
  const back = await FK.unzip(z);
  const names = back.map(e => e.name), at = FK.placeByNumber(names, 128);
  back.forEach((e, i) => assert.equal(A.fingerprint(A.parseFile(e.name, e.data)[0]), A.fingerprint(u3[at[i]])));
  n++;
  console.log("library_test: " + n + " ok (" + data.factory.length + " factory patches byte for byte with tools/sy1.py)");
})().catch(e => { console.error(e); process.exit(1); });
