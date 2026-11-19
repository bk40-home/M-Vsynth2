// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Kris (bk40-home)
//
// M-Vsynth2 online simulator: the firmware itself (WebAssembly, built by sim/build.sh with sim/mv_websim.c as its
// platform) run in real time; the panel drives it, the AudioWorklet plays it. As M-vance's sim/sim.js.
//   Link FM-1 (an FM-1 running M-Vsynth2 on USB, Chrome or Edge): the sim and the FM-1 hold the same sound both
//   ways (link.js, vs_xfer.h LINK / EDIT / PLAY). The sim is muted (you hear the FM-1); its knobs and buttons still
//   drive the sim, whose edits go to the FM-1, and the FM-1's own edits show here. The keys (and a MIDI keyboard)
//   play the FM-1, OCT moves their octave, PRESETS sends the FM-1 a Program Change (its new sound then comes here).
//   clock   the app's clock runs with the audio it renders, so pump() pulls real time's worth of frames every 8 ms
//           whether or not they are heard (before Start audio they are dropped): its screen and timers keep time
//   SysEx   what the app sends comes to M.onSysEx, a moment later (never while the app is mid-pass)
import { Link, frame, parse, CMD } from './link.js?v=20261119230127';

let M, pull, ctx, node, fbPtr, pullBuf;
let apiKeys, apiBtn, apiEnc, apiMidi, apiSysex, sxBuf;
// ---- the FM-1 and the link ----
let fm1Out = null, fm1Name = '';         // an FM-1 running M-Vsynth2 (found by name, confirmed by IDENT), its version
let link = null;                         // linked: link.js (null: the sim plays on its own)
let webOct = 0, progSel = 0, linkNote = '';
const held = new Int16Array(27).fill(-1);          // linked: the MIDI note each key sounds on the FM-1 (-1 = up)
const ENC_PRESETS = 6;                             // platform.c, the FM-1 layout
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
  M = await MVsynth2({ locateFile: (path) => path + '?v=20261119230127' });   // the .wasm with the build stamp too
  pull    = M.cwrap('wsim_pull', 'number', ['number', 'number']);
  apiKeys = M.cwrap('wsim_keys', null, ['number']);
  apiBtn  = M.cwrap('wsim_btn',  null, ['number', 'number']);
  apiEnc  = M.cwrap('wsim_enc',  null, ['number', 'number']);
  apiMidi = M.cwrap('wsim_midi', null, ['number', 'number', 'number']);
  apiSysex = M.cwrap('wsim_sysex_in', null, ['number', 'number']);
  fbPtr   = M.ccall('wsim_fb', 'number', [], []);
  pullBuf = M._malloc(PULL_FRAMES * 2 * 4);     // float32 stereo, in the WASM heap
  sxBuf   = M._malloc(256);                     // a SysEx into the sim (the longest: a PLAY, 144 B)
  M.onMidiOut = () => {};
  M.onSysEx = (bytes) => { const m = new Uint8Array(bytes); queueMicrotask(() => link && link.fromSim(m)); };
  setInterval(pump, 8);
  setInterval(() => link && link.tick(), 30);
  document.getElementById('link').onchange = (e) => setLink(e.target.checked);
  wireInput();
  requestAnimationFrame(draw);
  document.getElementById('start').onclick = startAudio;   // audio needs a user gesture
}

function startAudio() {
  if (ctx) { ctx.resume(); return; }
  // the engine's fixed 44.1 kHz (a context's default 48 kHz would play sharp and drift)
  ctx = new AudioContext({ sampleRate: FS, latencyHint: 'interactive' });
  ctx.audioWorklet.addModule('audio-worklet.js?v=20261119230127').then(() => {
    node = new AudioWorkletNode(ctx, 'mvsynth2', { outputChannelCount: [2] });
    node.connect(ctx.destination);
    const b = document.getElementById('start');
    b.textContent = '♪ Audio on';
    b.classList.remove('primary');
    setMode();
  });
}

function sysexIn(m) {                            // a whole F0..F7 message into the sim (wsim_sysex_in)
  if (m.length > 256) return;
  M.HEAPU8.set(m, sxBuf);
  apiSysex(sxBuf, m.length);
}

// ---- the mode line, the switch ----
function setMode() {
  const el = document.getElementById('mode');
  const oct = `oct ${webOct >= 0 ? '+' : ''}${webOct}`;
  el.textContent = link ? (link.cur ? `● Linked to FM-1 · ${link.name || 'no name'} · ${oct}` : 'Linking to FM-1…')
    : linkNote || (fm1Out ? `${ctx ? 'Playing' : 'Emulator'} · FM-1 found (${fm1Name})` : ctx ? 'Playing' : 'Emulator');
  el.className = link && link.cur ? 'mode on' : 'mode';
  const sw = document.getElementById('link');
  sw.disabled = !fm1Out; sw.checked = !!link;
  document.getElementById('linkBox').className = fm1Out ? 'link' : 'link off';
}

// Link FM-1 on: the FM-1's sound comes into the sim, the sim is muted, the keys play the FM-1; off: the sim plays its
// own again (the FM-1 keeps what it has; its link lapses 30 s later)
function setLink(on) {
  if (on && !fm1Out) on = false;
  if (on === !!link) { setMode(); return; }
  for (let k = 0; k < 27; k++) noteSet(k, false);                  // no note left hanging, where it plays now
  noteMask = 0; apiKeys(0);
  linkNote = '';
  link = on ? new Link({ simSend: sysexIn, fm1Send: (m) => fm1Out.send(m), onChange: linkChanged }) : null;
  if (!on) held.fill(-1);
  setMode();
}
function linkChanged(why) {
  if (why === 'nolink') {                       // an FM-1 with an older M-Vsynth2 (no LINK): said so
    link = null;
    linkNote = "This FM-1's firmware has no link: update M-Vsynth2 on it";
  }
  setMode();
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
  if (n > 0 && node && !link)                   // (linked: muted, you hear the FM-1)
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
  if (link && fm1Out) {                          // linked: the key plays the FM-1, in webOct's octave
    if (on) {
      const n = KEY0_MIDI + k + 12 * webOct;
      if (n >= 0 && n < 128) { held[k] = n; fm1Out.send([0x90, n, 100]); }
    } else if (held[k] >= 0) {
      fm1Out.send([0x80, held[k], 0]); held[k] = -1;               // off the exact note sent (the octave may have moved)
    }
    const el = document.querySelector(`.key[data-k="${k}"]`);
    if (el) el.classList.toggle('on', on);
    return;
  }
  noteMask = on ? (noteMask | (1 << k)) : (noteMask & ~(1 << k));
  apiKeys(noteMask);
  const el = document.querySelector(`.key[data-k="${k}"]`);
  if (el) el.classList.toggle('on', on);
}
function turn(id, dir) {
  if (link && fm1Out && id === ENC_PRESETS) {    // linked: PRESETS changes the FM-1's program (its sound then comes)
    progSel = (progSel + dir + 128) % 128;
    fm1Out.send([0xc0, progSel]);
  } else apiEnc(id, dir);
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
    if (e.target.closest('input[type=text], input[type=search], select, textarea')) return;   // (a checkbox: keys still play)
    const i = ROW.indexOf(e.key.toLowerCase());
    if (i >= 0 && !e.repeat) { noteSet(i, true); return; }
    if (e.key === '[') { pressBtn(0, 1); pressBtn(0, 0); }        // OCT -
    if (e.key === ']') { pressBtn(1, 1); pressBtn(1, 0); }        // OCT +
  });
  addEventListener('keyup', (e) => { const i = ROW.indexOf(e.key.toLowerCase()); if (i >= 0) noteSet(i, false); });

  buildKeyboard();
  document.querySelectorAll('.knob[data-enc]').forEach(wireKnob);
  document.querySelectorAll('button[data-btn]').forEach((el) => {
    const id = +el.dataset.btn;
    let down = false;
    const up = () => { if (down) { down = false; pressBtn(id, 0); el.classList.remove('on'); } };
    el.addEventListener('pointerdown', () => { down = true; pressBtn(id, 1); el.classList.add('on'); });
    el.addEventListener('pointerup', up);
    el.addEventListener('pointerleave', up);
  });

  // WebMIDI with SysEx (for the link): an FM-1 found by name and confirmed by IDENT as running M-Vsynth2 (an FM-1
  // running M-vance or another firmware is not offered); re-scanned on hot-plug.
  //   from the FM-1     its SysEx -> the link (or its IDENT); its Program Changes -> the program shown
  //   another input     (a keyboard) plays the sim, or, linked, the FM-1
  if (navigator.requestMIDIAccess) {
    const FM1_NAME = /felucca|fm-?1|m-?vsynth|m-?vave|dinghy/i;
    navigator.requestMIDIAccess({ sysex: true }).then((access) => {
      const scan = () => {
        let cand = null;
        for (const o of access.outputs.values()) if (FM1_NAME.test(o.name || '')) cand = o;
        for (const inp of access.inputs.values()) {
          const fromFm1 = FM1_NAME.test(inp.name || '');
          inp.onmidimessage = (m) => {
            const d = m.data;
            if (fromFm1) {
              if (d[0] === 0xf0) {
                const p = parse(d);
                if (p && p.cmd === CMD.IDREP) {                   // "M-VSYNTH2 <version>": this one can link
                  const id = String.fromCharCode(...p.data).split('\0')[0];
                  if (/^M-VSYNTH2/.test(id)) { fm1Out = cand; fm1Name = id.replace('M-VSYNTH2', '').trim() || 'M-Vsynth2'; setMode(); }
                } else if (link) link.fromFm1(new Uint8Array(d));
              } else if ((d[0] & 0xf0) === 0xc0) progSel = d[1];
              return;
            }
            if (d.length !== 3) return;
            if (link && fm1Out) fm1Out.send(d);                    // linked: a keyboard plays the FM-1
            else apiMidi(d[0], d[1], d[2]);
          };
        }
        if (!cand) { if (fm1Out) { fm1Out = null; if (link) setLink(false); setMode(); } return; }
        if (cand !== fm1Out) cand.send(frame(CMD.IDENT));          // which firmware? (the answer above)
      };
      scan();
      access.onstatechange = scan;
    }).catch(() => {});
  }
}

// a panel button: linked, OCT moves the octave the keys play the FM-1 in; every other button drives the sim (its
// pages: what the knobs edit, on the FM-1 too through the link)
function pressBtn(id, down) {
  if (link && (id === 0 || id === 1)) {
    if (down) { webOct = id === 0 ? Math.max(-3, webOct - 1) : Math.min(3, webOct + 1); setMode(); }
    return;
  }
  apiBtn(id, down);
}

boot();
