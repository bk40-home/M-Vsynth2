// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-Vsynth2 online simulator: the firmware itself (WebAssembly, built by sim/build.sh with sim/mv_websim.c as its
// platform) run in real time; the panel drives it, the AudioWorklet plays it. Taken from M-vance's sim/sim.js
// without its FM-1 link (M-vance's own SysEx); a MIDI keyboard plays the sim.
//   clock   the app's clock runs with the audio it renders, so pump() pulls real time's worth of frames every 8 ms
//           whether or not they are heard (before Start audio they are dropped): its screen and timers keep time
//   SysEx   what the app sends comes to M.onSysEx (none expected here; the patch library talks to an FM-1)

let M, pull, ctx, node, fbPtr, pullBuf;
let apiKeys, apiBtn, apiEnc, apiMidi;
let owed = 0, lastT = 0, noteMask = 0;
const PULL_FRAMES = 2048;                // the scratch buffer (frames): at most this much a pump
const FS = 44100;                        // the engine's fixed rate (mv_websim.c FS)
const KEY0_MIDI = 53;                    // key 0 = F3; 27 keys up to G5
const BLACK = [1, 3, 6, 8, 10];          // black-key pitch classes
const indOf = {}, knobAng = {};          // enc id -> its indicator and angle (cosmetic)

async function boot() {
  if (typeof MVsynth2 !== 'function') {
    document.getElementById('mode').textContent = 'Not built: run sh sim/build.sh';
    return;
  }
  M = await MVsynth2({ locateFile: (path) => path + '?v=20261114224758' });   // the .wasm with the build stamp too
  pull    = M.cwrap('wsim_pull', 'number', ['number', 'number']);
  apiKeys = M.cwrap('wsim_keys', null, ['number']);
  apiBtn  = M.cwrap('wsim_btn',  null, ['number', 'number']);
  apiEnc  = M.cwrap('wsim_enc',  null, ['number', 'number']);
  apiMidi = M.cwrap('wsim_midi', null, ['number', 'number', 'number']);
  fbPtr   = M.ccall('wsim_fb', 'number', [], []);
  pullBuf = M._malloc(PULL_FRAMES * 2 * 4);     // float32 stereo, in the WASM heap
  M.onMidiOut = () => {};
  M.onSysEx = () => {};
  setInterval(pump, 8);
  wireInput();
  requestAnimationFrame(draw);
  document.getElementById('start').onclick = startAudio;   // audio needs a user gesture
}

function startAudio() {
  if (ctx) { ctx.resume(); return; }
  // the engine's fixed 44.1 kHz (a context's default 48 kHz would play sharp and drift)
  ctx = new AudioContext({ sampleRate: FS, latencyHint: 'interactive' });
  ctx.audioWorklet.addModule('audio-worklet.js?v=20261114224758').then(() => {
    node = new AudioWorkletNode(ctx, 'mvsynth2', { outputChannelCount: [2] });
    node.connect(ctx.destination);
    const b = document.getElementById('start');
    b.textContent = '♪ Audio on';
    b.classList.remove('primary');
    document.getElementById('mode').textContent = 'Playing';
    document.getElementById('mode').className = 'mode on';
  });
}

// pull only as much audio as real time has advanced (never back up)
function pump() {
  const now = performance.now();
  if (lastT === 0) lastT = now;
  owed += (now - lastT) * FS / 1000;
  lastT = now;
  if (owed > PULL_FRAMES) owed = PULL_FRAMES;
  const want = Math.floor(owed);
  if (want <= 0) return;
  const n = pull(pullBuf, want);
  owed -= n;
  if (n > 0 && node)
    node.port.postMessage(new Float32Array(M.HEAPF32.subarray(pullBuf >> 2, (pullBuf >> 2) + n * 2)));
}

const canvas = document.getElementById('screen');
const c2d = canvas.getContext('2d');
const img = c2d.createImageData(240, 240);
function draw() {
  const px = M.HEAPU16.subarray(fbPtr >> 1, (fbPtr >> 1) + 240 * 240);
  const d = img.data;
  for (let i = 0; i < 240 * 240; i++) {
    const v = px[i];                            // RGB565
    d[i * 4]     = ((v >> 11) & 31) * 255 / 31;
    d[i * 4 + 1] = ((v >> 5) & 63) * 255 / 63;
    d[i * 4 + 2] = (v & 31) * 255 / 31;
    d[i * 4 + 3] = 255;
  }
  c2d.putImageData(img, 0, 0);
  requestAnimationFrame(draw);
}

// ---- the keys (bit 0 = F3), an encoder detent, a panel button ----
function noteSet(k, on) {
  if (k < 0 || k > 26) return;
  noteMask = on ? (noteMask | (1 << k)) : (noteMask & ~(1 << k));
  apiKeys(noteMask);
  const el = document.querySelector(`.key[data-k="${k}"]`);
  if (el) el.classList.toggle('on', on);
}
function turn(id, dir) {
  apiEnc(id, dir);
  const el = indOf[id];
  if (el) { knobAng[id] = (knobAng[id] || 0) + dir * 20; el.style.transform = `rotate(${knobAng[id]}deg)`; }
}

function buildKeyboard() {
  const host = document.getElementById('keys');
  host.innerHTML = '';
  const WW = 34, BW = 22, blacks = [];
  let x = 0;
  for (let k = 0; k < 27; k++) {
    const isBlack = BLACK.includes((KEY0_MIDI + k) % 12);
    const el = document.createElement('div');
    el.className = 'key ' + (isBlack ? 'black' : 'white');
    el.dataset.k = k;
    el.style.left = (isBlack ? x - BW / 2 : x) + 'px';
    if (!isBlack) x += WW; else blacks.push(el);
    el.addEventListener('pointerdown', (e) => { e.preventDefault(); el.setPointerCapture?.(e.pointerId); noteSet(k, true); });
    const up = () => noteSet(k, false);
    el.addEventListener('pointerup', up);
    el.addEventListener('pointercancel', up);
    el.addEventListener('pointerleave', (e) => { if (e.buttons) up(); });
    host.appendChild(el);
  }
  blacks.forEach((b) => host.appendChild(b));
  host.style.width = x + 'px';
}

// a rotary encoder: drag (up = clockwise), scroll, or focused: arrow keys
function wireKnob(el) {
  const id = +el.dataset.enc;
  indOf[id] = el.querySelector('.ind');
  let dragging = false, acc = 0;
  el.addEventListener('wheel', (e) => { turn(id, e.deltaY < 0 ? 1 : -1); e.preventDefault(); }, { passive: false });
  el.addEventListener('pointerdown', (e) => { dragging = true; acc = 0; el.setPointerCapture(e.pointerId); el.focus(); });
  el.addEventListener('pointermove', (e) => {
    if (!dragging) return;
    acc += e.movementY;
    while (acc <= -6) { turn(id, 1); acc += 6; }
    while (acc >= 6)  { turn(id, -1); acc -= 6; }
  });
  const end = () => { dragging = false; };
  el.addEventListener('pointerup', end);
  el.addEventListener('pointercancel', end);
  el.addEventListener('keydown', (e) => {
    const d = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
    if (d) { turn(id, d); e.preventDefault(); e.stopPropagation(); }
  });
}

function wireInput() {
  const ROW = 'zsxdcvgbhnjm,l.;/';          // the computer keys (a piano row from Z = F3)
  addEventListener('keydown', (e) => {
    if (e.target.closest('input, select, textarea')) return;
    const i = ROW.indexOf(e.key.toLowerCase());
    if (i >= 0 && !e.repeat) { noteSet(i, true); return; }
    if (e.key === '[') { apiBtn(0, 1); apiBtn(0, 0); }            // OCT -
    if (e.key === ']') { apiBtn(1, 1); apiBtn(1, 0); }            // OCT +
  });
  addEventListener('keyup', (e) => { const i = ROW.indexOf(e.key.toLowerCase()); if (i >= 0) noteSet(i, false); });

  buildKeyboard();
  document.querySelectorAll('.knob[data-enc]').forEach(wireKnob);
  document.querySelectorAll('button[data-btn]').forEach((el) => {
    const id = +el.dataset.btn;
    let down = false;
    const up = () => { if (down) { down = false; apiBtn(id, 0); el.classList.remove('on'); } };
    el.addEventListener('pointerdown', () => { down = true; apiBtn(id, 1); el.classList.add('on'); });
    el.addEventListener('pointerup', up);
    el.addEventListener('pointerleave', up);
  });

  // a MIDI keyboard plays the sim (notes, CCs, pitch bend: as the FM-1's USB would hand them over)
  if (navigator.requestMIDIAccess) {
    navigator.requestMIDIAccess().then((access) => {
      const scan = () => { for (const inp of access.inputs.values())
        inp.onmidimessage = (m) => { const d = m.data; if (d.length === 3) apiMidi(d[0], d[1], d[2]); }; };
      scan();
      access.onstatechange = scan;
    }).catch(() => {});
  }
}

boot();
