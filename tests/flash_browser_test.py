#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Optional (Playwright): the firmware page (docs/flash) in Chrome against a pretend FM-1 behind a fake Web MIDI.
Find -> the latest release (build/mvsynth2.fwsc as a pretend release) -> install (running -> the loader under its
own port name -> the new firmware, every byte checked) -> the page counter's beacons. Run from the repo root after
sh flash/build.sh and ./build.sh:
  python3 tests/flash_browser_test.py            (CHROME=/path/to/chrome to use an installed Chrome)"""
import asyncio, mimetypes, os, shutil, subprocess, sys, tempfile
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[1]
SITE = Path(tempfile.mkdtemp(prefix="flash-site-"))
shutil.copytree(ROOT / "docs", SITE, dirs_exist_ok=True)
subprocess.run([sys.executable, str(ROOT / "tools/flash_manifest.py"), "--version", "1.0", "--tag", "v1.0",
                "--url", "https://github.com/bk40-home/M-Vsynth2/releases/tag/v1.0", "--out", str(SITE / "flash/firmware"),
                str(ROOT / "build/mvsynth2.fwsc")], check=True)
for f in (SITE / "flash/index.html", SITE / "index.html"):
    f.write_text(f.read_text().replace("__COUNT__", "https://mvs2.goatcounter.com/count"))
FAKE = r"""
Object.defineProperty(navigator, 'webdriver', {get: () => false});
(() => {
  const dev = {identity: 'FM-1_015', name: 'FM-1', gen: 0, served: 0, bad: 0, upgrades: 0, waiting: null, queue: [], log: []};
  window.__fm1 = dev;
  const eq = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
  const HS = [0xF0,0x00,0x32,0x45,0x00,0x00,0x00,0x40,0x7F,0xF7], UP = [0xF0,0x22,0x24,0x35,0x7F,0xF7];
  function pack7(d){const o=[];let a=0,n=0;for(const b of d){a|=b<<n;n+=8;while(n>=7){o.push(a&0x7F);a>>>=7;n-=7;}}if(n)o.push(a&0x7F);return o;}
  function unpack7(s){const o=[];let a=0,n=0;for(const b of s){a|=(b&0x7F)<<n;n+=7;while(n>=8){o.push(a&0xFF);a>>>=8;n-=8;}}return o;}
  let inputs = new Map(), outputs = new Map(), access = null;
  function makePorts() {
    const g = ++dev.gen;
    for (const p of [...inputs.values(), ...outputs.values()]) p.state = 'disconnected';
    inputs = new Map(); outputs = new Map();
    const inp = {id: 'i' + g, name: dev.name, type: 'input', state: 'connected', onmidimessage: null,
      open: async () => {}, close: async () => {}};
    const out = {id: 'o' + g, name: dev.name, type: 'output', state: 'connected', open: async () => {}, close: async () => {},
      send(d) { if (this.state !== 'connected') throw new Error('gone'); rx(Uint8Array.from(d)); }};
    inputs.set(inp.id, inp); outputs.set(out.id, out);
    if (access) { access.inputs = inputs; access.outputs = outputs; }
  }
  function tx(arr) { const inp = [...inputs.values()][0]; const d = Uint8Array.from([0xF0, ...pack7(arr), 0xF7]);
    setTimeout(() => inp && inp.onmidimessage && inp.onmidimessage({data: d}), 1); }
  function boot(identity, name) { dev.identity = identity; dev.name = name; dev.waiting = null; dev.queue = []; dev.log.push('boot ' + identity + ' on ' + name); makePorts(); }
  function next() {
    if (!dev.queue.length) return;
    const [addr, n] = dev.waiting = dev.queue.shift();
    const u = [0, 0x59, 0x30, 0, 0, 0, 0];
    for (let i = 0; i < 4; i++) u.push(Math.floor(addr / 2 ** (8 * i)) & 0xFF);
    u.push(n & 0xFF, n >> 8, 0);
    let s = 0; for (let i = 6; i < 14; i++) s += u[i]; u.push(~s & 0xFF);
    tx(u);
  }
  function rx(d) {
    if (eq([...d], HS)) { const t = [...new TextEncoder().encode(dev.identity)]; const b = [0, 0x59, 0x11, 0, 0, 0, ...t]; while (b.length < 34) b.push(0); tx(b); }
    else if (eq([...d], UP)) {
      dev.upgrades++;
      dev.queue = dev.identity.startsWith('ota-')
        ? [...Array(40).keys()].map((k) => [k * 512, 512]).concat([[0xF0000000, 8]])
        : [[0, 64], [0x40, 160], [0x1000, 512], [0xE0000000, 8]];
      next();
    } else if (dev.waiting) {
      const u = unpack7(d.subarray(1, -1)), [addr, n] = dev.waiting;
      const fin = addr >= 0xE0000000, len = fin ? 8 : n;
      if (u.length !== 15 + len) dev.bad++;
      if (!fin && window.__image && !eq(u.slice(14, 14 + n), [...window.__image.subarray(addr, addr + n)])) dev.bad++;
      dev.waiting = null; dev.served++;
      if (addr === 0xE0000000) setTimeout(() => boot('ota-FM-1_900', 'Felucca Update'), 300);
      else if (addr === 0xF0000000) setTimeout(() => boot('FM-1_968090', 'Felucca'), 300);
      else next();
    }
  }
  boot('FM-1_015', 'FM-1');
  navigator.requestMIDIAccess = async (o) => { if (!o || !o.sysex) throw new Error('no sysex');
    access = {inputs, outputs, sysexEnabled: true, onstatechange: null}; return access; };
})();
"""

async def main():
    hits, errors = [], []
    async with async_playwright() as pw:
        b = await pw.chromium.launch(executable_path=os.environ.get("CHROME") or None)
        page = await b.new_page(viewport={"width": 1100, "height": 900})
        page.on("console", lambda m: m.type == "error" and errors.append(m.text))
        page.on("pageerror", lambda e: errors.append(str(e)))

        async def serve(route):
            url = route.request.url
            if "goatcounter.com" in url:
                hits.append(url); return await route.fulfill(status=200, body=b"GIF89a", content_type="image/gif")
            path = url.split("fm1.test/", 1)[1].split("?")[0]
            f = SITE / (path or "index.html")
            if f.is_dir(): f = f / "index.html"
            if not f.exists(): return await route.fulfill(status=404, body=b"")
            ct = mimetypes.guess_type(str(f))[0] or "application/octet-stream"
            if f.suffix == ".js": ct = "text/javascript"
            await route.fulfill(status=200, body=f.read_bytes(), content_type=ct)
        await page.route("**/*", serve)
        await page.add_init_script(FAKE)

        await page.goto("https://fm1.test/flash/")
        await page.wait_for_function("document.getElementById('fx-latest').textContent.includes('1.0')")
        await page.wait_for_function("!document.getElementById('fx-pkg').hidden")
        print("latest:", await page.text_content("#fx-latest"), "| package:", await page.text_content("#fx-pkg-id"))
        # the fake's byte check: the logical image, from the page's own module
        await page.evaluate("""async () => { const O = await import('/flash/fm1ota.js');
            const b = new Uint8Array(await (await fetch('/flash/firmware/mvsynth2.fwsc')).arrayBuffer());
            window.__image = O.loadPackage(b).image; }""")
        print("before find:", await page.text_content("#fx-why"), "| go disabled:", await page.is_disabled("#fx-go"))
        await page.click("#fx-connect")
        await page.wait_for_function("!document.getElementById('fx-dev').hidden")
        print("device:", await page.text_content("#fx-dev-id"), "| port:", await page.text_content("#fx-dev-port"))
        print("before tick:", await page.text_content("#fx-why"))
        await page.check("#fx-ack")
        await page.click("#fx-go")
        await page.wait_for_function("document.getElementById('fx-msg').classList.contains('good') || document.getElementById('fx-msg').classList.contains('bad')", timeout=90000)
        print("result:", await page.text_content("#fx-msg"))
        print("log:\n" + await page.text_content("#fx-log"))
        print("fake:", await page.evaluate("JSON.stringify({served: __fm1.served, bad: __fm1.bad, upgrades: __fm1.upgrades, log: __fm1.log})"))
        print("bar:", await page.evaluate("document.getElementById('fx-bar').value"))
        ok = "runs FM-1_968090" in await page.text_content("#fx-msg") and \
            await page.evaluate("__fm1.bad === 0 && __fm1.upgrades === 2")

        # wrong file: not a package
        await page.set_input_files("#fx-file", files=[{"name": "junk.fwsc", "mimeType": "application/octet-stream", "buffer": b"\0" * 2000}])
        await page.wait_for_function("!document.getElementById('fx-pkg-msg').hidden")
        print("junk file:", await page.text_content("#fx-pkg-msg"), "| go disabled:", await page.is_disabled("#fx-go"))

        ok = ok and await page.is_disabled("#fx-go")
        await page.goto("https://fm1.test/")
        await page.wait_for_timeout(500)
        await b.close()
    print("counter:", *[h.split("?")[1].split("&rnd")[0][:90] for h in hits], sep="\n  ")
    print("console errors:", errors or "none")
    ok = ok and not errors and any("flash%2Fdone" in h for h in hits)
    print("flash page:", "ok" if ok else "FAIL")
    shutil.rmtree(SITE, ignore_errors=True)
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    asyncio.run(main())
