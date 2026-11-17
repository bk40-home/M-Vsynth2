#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home).
"""The library page's Connect against a pretend Windows MIDI setup (issue #2), in a real browser (optional: needs
`pip install playwright` and `playwright install chromium`, or CHROME=/path/to/chrome).

    python3 tests/library_connect_test.py [docs/library.html]

Web MIDI is replaced before the page loads. The FM-1 is "Felucca" (its USB name, src/usb.c), wired to the page's own
pretend M-Vsynth2 (library/mock-mvsynth2.js). Around it, as on Windows after an install:
  - "Microsoft GS Wavetable Synth", an output only, listed first (the old pick took it: "does not answer");
  - a "Felucca" input and output from before the restart, still listed, state "disconnected";
  - another device's input ("Other Keys") that never answers SysEx.
Cases:
  1. Connect finds the live Felucca, reads U1, and the status names it;
  2. the FM-1 restarts (its ports go "disconnected"): the page lets go and says so; Connect again works;
  3. the port is held by another program (open() refused, as WinMM does): the message says so;
  4. on Disconnect the ports are closed (Windows: one holder at a time)."""
import asyncio, os, sys
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[1]
PAGE = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "docs/library.html").resolve())

FAKE_MIDI = r"""
(() => {
  const W = window.__midi = {busy: false, opened: 0, closed: 0, dev: null};
  function port(kind, name, state, wired) {
    const p = {type: kind, name, state: state || "connected", connection: "closed", onmidimessage: null, onstatechange: null,
      open() {
        if (wired && W.busy) return Promise.reject(new DOMException("Port is not available", "InvalidAccessError"));
        if (p.state === "disconnected") return Promise.reject(new DOMException("gone", "InvalidStateError"));
        if (p.connection !== "open") { p.connection = "open"; W.opened++; }
        return Promise.resolve(p);
      },
      close() { if (p.connection === "open") { p.connection = "closed"; W.closed++; } return Promise.resolve(p); },
      send(bytes) {
        if (!wired) return;                                   /* the wavetable synth: says nothing */
        if (!W.dev) { W.dev = window.FK.mockDevice(); W.dev.listen(d => W.feliIn.onmidimessage && W.feliIn.onmidimessage({data: d})); }
        W.dev.send(bytes);
      },
    };
    return p;
  }
  const ins = new Map(), outs = new Map();
  outs.set("o0", port("output", "Microsoft GS Wavetable Synth"));
  ins.set("i-old", port("input", "Felucca", "disconnected"));
  outs.set("o-old", port("output", "Felucca", "disconnected"));
  ins.set("i-other", port("input", "Other Keys"));
  outs.set("o-other", port("output", "Other Keys"));
  W.feliIn = port("input", "Felucca", "connected", true);
  W.feliOut = port("output", "Felucca", "connected", true);
  ins.set("i-new", W.feliIn);
  outs.set("o-new", W.feliOut);
  W.restart = () => {                                         /* the FM-1 goes away */
    for (const p of [W.feliIn, W.feliOut]) { p.state = "disconnected"; p.connection = "closed"; if (p.onstatechange) p.onstatechange({port: p}); }
  };
  W.back = () => { for (const p of [W.feliIn, W.feliOut]) p.state = "connected"; };
  navigator.requestMIDIAccess = () => Promise.resolve({inputs: ins, outputs: outs, sysexEnabled: true, onstatechange: null});
})();
"""


def launch(p):
    exe = os.environ.get("CHROME")
    return p.chromium.launch(executable_path=exe, args=["--no-sandbox"]) if exe else p.chromium.launch()


async def status(pg):
    return await pg.text_content("#status")


async def main():
    async with async_playwright() as p:
        b = await launch(p)
        pg = await b.new_page(viewport={"width": 1440, "height": 900})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.add_init_script(FAKE_MIDI)
        await pg.goto("file://" + PAGE)
        await pg.wait_for_timeout(300)

        # 1. finds the live Felucca past the wavetable synth, the stale port and the silent device
        await pg.click("#conn")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read U1')", timeout=15000)
        info = await pg.text_content("#dev-info")
        assert "M-VSYNTH2" in info, info
        print("1. connected:", info.strip())

        # 2. the FM-1 restarts: the page lets go and says so
        await pg.evaluate("window.__midi.restart()")
        await pg.wait_for_timeout(100)
        s = await status(pg)
        assert "went away" in s, s
        assert (await pg.text_content("#dev-info")).strip() == "not connected"
        print("2. restart seen:", s)
        await pg.evaluate("window.__midi.back()")
        await pg.click("#conn")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read U1')", timeout=15000)
        print("2. connected again")

        # 4. Disconnect closes the ports
        closed = await pg.evaluate("window.__midi.closed")
        await pg.click("#conn")                                # (Connect while connected = Disconnect)
        await pg.wait_for_timeout(100)
        assert await pg.evaluate("window.__midi.feliIn.connection") == "closed"
        assert await pg.evaluate("window.__midi.feliOut.connection") == "closed"
        assert await pg.evaluate("window.__midi.closed") > closed
        print("4. ports closed on Disconnect")

        # 3. held by another program
        await pg.evaluate("window.__midi.busy = true")
        await pg.click("#conn")
        await pg.wait_for_function("/in use|answered/.test(document.querySelector('#status').textContent)", timeout=15000)
        s = await status(pg)
        assert "in use by another program" in s and "Felucca" in s, s
        print("3. busy:", s)

        assert not errs, errs
        await b.close()
        print("library connect: ok")


asyncio.run(main())
