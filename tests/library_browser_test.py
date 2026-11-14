#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home).
"""The library page in a real browser, against its Sandbox FM-1 (optional: needs `pip install playwright` and
`playwright install chromium`, or CHROME=/path/to/chrome).

    python3 tests/library_browser_test.py [docs/library.html] [OUT_DIR]

Opens the page; Sandbox; three factory patches dragged into U1 013..015, sent, acknowledged, read back and compared;
a move and its undo; search; details; audition; All sources; "Empty slots only" with too many; the PAPER theme; a
narrow window (no name cut). Then: a deflated zip bank and a loose .sy1 opened; a tag; the library exported; a
reload (sources and tags kept); a whole bank sent with S; the bank downloaded; patches dragged up from the FM-1;
Restore. Screenshots in OUT_DIR (build/library-shots)."""
import asyncio, os, sys, zipfile
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[1]
PAGE = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "docs/library.html").resolve())
OUT = str(Path(sys.argv[2] if len(sys.argv) > 2 else ROOT / "build/library-shots").resolve())
os.makedirs(OUT, exist_ok=True)
FIX = Path(OUT)
fac = sorted((ROOT / "tools/bank/factory").glob("*.sy1"))
with zipfile.ZipFile(FIX / "bank_deflated.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for f in fac[:20]:
        z.writestr("My Bank/" + f.name.replace(".sy1", "_x.sy1"), f.read_bytes())
    z.writestr("My Bank/readme.txt", "not a patch")
(FIX / "loose.sy1").write_bytes(fac[30].read_bytes())
os.chdir(FIX)


def launch(p):
    exe = os.environ.get("CHROME")
    return p.chromium.launch(executable_path=exe, args=["--no-sandbox"]) if exe else p.chromium.launch()


async def flow1():
    async with async_playwright() as p:
        b = await launch(p)
        pg = await b.new_page(viewport={"width": 1440, "height": 900})
        errs = []
        pg.on("console", lambda m: errs.append(m.text) if m.type == "error" else None)
        pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.goto("file://" + PAGE)
        await pg.wait_for_timeout(500)
        await pg.screenshot(path=OUT + "/1-open.png")
        await pg.click("#sandbox")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read U1')", timeout=10000)
        # pick three factory patches: 004, then shift 006
        await pg.click('#src-grid .cell[data-i="3"]')
        await pg.click('#src-grid .cell[data-i="5"]', modifiers=["Shift"])
        names = await pg.eval_on_selector_all('#src-grid .cell.sel .nm', "e => e.map(x => x.textContent)")
        print("selected", names)
        await pg.drag_and_drop('#src-grid .cell[data-i="3"]', '#dev-grid .cell[data-i="12"]')
        await pg.wait_for_timeout(200)
        print("send button:", await pg.text_content("#send"))
        print("status:", await pg.text_content("#status"))
        await pg.screenshot(path=OUT + "/2-dropped.png")
        await pg.click("#send")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Sent')", timeout=20000)
        print("status:", await pg.text_content("#status"))
        dev = await pg.eval_on_selector_all('#dev-grid .cell', "e => e.slice(12,15).map(x => x.querySelector('.nm').textContent)")
        print("U1 013-015:", dev)
        assert dev == names, (dev, names)
        # move one inside the bank: 013 onto 020 (empty): a move
        await pg.drag_and_drop('#dev-grid .cell[data-i="12"]', '#dev-grid .cell[data-i="19"]')
        await pg.wait_for_timeout(100)
        print("after move:", await pg.text_content("#status"))
        # undo it
        await pg.keyboard.press("Control+z")
        await pg.wait_for_timeout(100)
        print("after undo:", await pg.text_content("#send"))
        # search
        await pg.fill("#search", "cat:pad")
        await pg.wait_for_timeout(200)
        print("count:", await pg.text_content("#src-count"))
        await pg.screenshot(path=OUT + "/3-search.png")
        # details
        await pg.click('#src-grid .cell[data-i="1"]')
        await pg.keyboard.press("i")
        await pg.wait_for_timeout(200)
        await pg.screenshot(path=OUT + "/4-details.png")
        await pg.keyboard.press("Enter")
        await pg.wait_for_timeout(300)
        print("audition:", await pg.text_content("#status"))
        await pg.click("#d-close")
        # all sources with the search
        await pg.click("#src-all")
        await pg.wait_for_timeout(200)
        await pg.screenshot(path=OUT + "/5-all.png")
        await pg.fill("#search", "")
        # many into a nearly full place: overflow, empty only
        await pg.click('#src-all')
        await pg.click('#src-grid .cell[data-i="0"]')
        await pg.click('#src-grid .cell[data-i="9"]', modifiers=["Shift"])
        await pg.check("#empty-only")
        await pg.drag_and_drop('#src-grid .cell[data-i="0"]', '#dev-grid .cell[data-i="120"]')
        await pg.wait_for_timeout(100)
        print("empty-only drop:", await pg.text_content("#status"))
        # paper theme
        await pg.select_option("#theme", "paper")
        await pg.wait_for_timeout(200)
        await pg.screenshot(path=OUT + "/6-paper.png")
        await pg.select_option("#theme", "sea")
        # narrow window: names still whole?
        await pg.set_viewport_size({"width": 1000, "height": 800})
        await pg.wait_for_timeout(300)
        cut = await pg.evaluate("[...document.querySelectorAll('.cell .nm')].filter(n => n.scrollWidth > n.clientWidth + 1 || n.getBoundingClientRect().right > n.parentElement.getBoundingClientRect().right + 1).length")
        print("cut names at 1000 px:", cut)
        await pg.screenshot(path=OUT + "/7-narrow.png")
        assert not errs, errs
        print("flow 1: ok")
        await b.close()



async def flow2():
    async with async_playwright() as p:
        b = await launch(p)
        ctx = await b.new_context(viewport={"width":1440,"height":900}, accept_downloads=True)
        pg = await ctx.new_page(); errs=[]
        pg.on("pageerror", lambda e: errs.append(str(e))); pg.on("console", lambda m: errs.append(m.text) if m.type=="error" else None)
        await pg.goto("file://"+PAGE); await pg.wait_for_timeout(400)
        await pg.set_input_files("#file-in", ["bank_deflated.zip","loose.sy1"])
        await pg.wait_for_timeout(800)
        print("status:", await pg.text_content("#status"))
        print("banks:", await pg.evaluate("FK._state.sources.map(s => s.group + ' > ' + s.name)"))
        # tag a patch, then export the library
        await pg.click('#src-grid .cell[data-i="0"]'); await pg.keyboard.press("i")
        await pg.fill("#d-tag-in","warm"); await pg.press("#d-tag-in","Enter"); await pg.wait_for_timeout(200)
        async with pg.expect_download() as dl:
            await pg.click("#export")
        f = await (await dl.value).path(); z = zipfile.ZipFile(f)
        print("export:", len(z.namelist()), [n for n in z.namelist() if n.endswith("json")])
        # reload: the sources and the tag are kept
        await pg.reload(); await pg.wait_for_timeout(800)
        print("after reload banks:", await pg.evaluate("FK._state.sources.map(s => s.group + ' > ' + s.name)"))
        await pg.fill("#search","tag:warm"); await pg.wait_for_timeout(200)
        assert (await pg.text_content("#src-count")).startswith("1 match"), "the tag was not kept"
        await pg.fill("#search","")
        # sandbox, send the zip bank into U2 from 001 via S key, download U2
        await pg.click("#sandbox"); await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read')")
        await pg.click('#dev-tabs .tab:nth-child(3)'); await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read U2')")
        await pg.click('#dev-grid .cell[data-i="0"]')
        await pg.click('#src-pick'); await pg.fill('#src-filter', 'my bank'); await pg.keyboard.press('Enter')
        await pg.click('#src-grid'); await pg.keyboard.press("Control+a"); await pg.keyboard.press("s")
        print("S:", await pg.text_content("#status"))
        await pg.click("#send"); await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Sent')", timeout=30000)
        assert (await pg.text_content("#status")).startswith("Sent 20")
        async with pg.expect_download() as dl:
            await pg.click("#download")
        z = zipfile.ZipFile(await (await dl.value).path())
        print("U2 zip:", len(z.namelist()), z.namelist()[:3])
        # take two from the device into the library (drag up)
        await pg.click('#dev-grid .cell[data-i="0"]'); await pg.click('#dev-grid .cell[data-i="1"]', modifiers=["Shift"])
        await pg.drag_and_drop('#dev-grid .cell[data-i="0"]', '#src-grid')
        print("up:", await pg.text_content("#status"))
        # restore U2 as before the send
        await pg.click("#restore"); print("restore:", await pg.text_content("#send"))
        assert not errs, errs
        print("flow 2: ok")
        await b.close()


asyncio.run(flow1())
asyncio.run(flow2())
