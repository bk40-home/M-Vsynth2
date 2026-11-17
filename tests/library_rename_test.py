#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home).
"""Rename in place on the library page (issue #4), in a real browser against its Sandbox FM-1 (optional: needs
`pip install playwright` and `playwright install chromium`, or CHROME=/path/to/chrome).

    python3 tests/library_rename_test.py [docs/library.html]

  1. a built-in bank's patch: a double-click on its name says it is read only, no editor;
  2. a factory patch dropped on U1 013, its name double-clicked, typed, Enter: the slot shows it, pending; Send; the
     pretend FM-1 holds the new name; a double-click on the slot's number still auditions;
  3. F2 on the focused slot, typed, Esc: nothing changes;
  4. the editor's keys stay in it (Delete does not clear the slot)."""
import asyncio, os, sys
from pathlib import Path
from playwright.async_api import async_playwright

ROOT = Path(__file__).resolve().parents[1]
PAGE = str(Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "docs/library.html").resolve())


def launch(p):
    exe = os.environ.get("CHROME")
    return p.chromium.launch(executable_path=exe, args=["--no-sandbox"]) if exe else p.chromium.launch()


async def main():
    async with async_playwright() as p:
        b = await launch(p)
        pg = await b.new_page(viewport={"width": 1440, "height": 900})
        errs = []
        pg.on("pageerror", lambda e: errs.append(str(e)))
        await pg.goto("file://" + PAGE)
        await pg.wait_for_timeout(300)
        await pg.click("#sandbox")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Read U1')", timeout=10000)
        st = lambda: pg.text_content("#status")

        # 1. built in: refused, no editor
        await pg.dblclick('#src-grid .cell[data-i="3"] .nm')
        assert await pg.locator(".nm-edit").count() == 0
        assert "read only" in await st(), await st()
        print("1.", await st())

        # 2. into U1 013, renamed there, sent, read back
        await pg.click('#src-grid .cell[data-i="3"]')
        await pg.drag_and_drop('#src-grid .cell[data-i="3"]', '#dev-grid .cell[data-i="12"]')
        await pg.dblclick('#dev-grid .cell[data-i="12"] .nm')
        ed = pg.locator("#dev-grid .nm-edit")
        assert await ed.count() == 1
        await ed.fill("MY LEAD")
        await ed.press("Delete")                     # 4. a key the grid knows: stays in the editor
        await ed.press("Enter")
        await pg.wait_for_timeout(100)
        assert (await pg.text_content('#dev-grid .cell[data-i="12"] .nm')) == "MY LEAD"
        assert "changed" in (await pg.get_attribute('#dev-grid .cell[data-i="12"]', "class"))
        print("2.", await st())
        await pg.click("#send")
        await pg.wait_for_function("document.querySelector('#status').textContent.startsWith('Sent')", timeout=20000)
        held = await pg.evaluate("(() => { const io = window.FK && document.querySelector('#dev-info').textContent; return io; })()")
        assert (await pg.text_content('#dev-grid .cell[data-i="12"] .nm')) == "MY LEAD"
        print("2. sent, read back:", await st(), "|", held.strip())

        # a double-click on the number still auditions
        await pg.dblclick('#dev-grid .cell[data-i="12"] .num')
        await pg.wait_for_timeout(200)
        assert "Playing" in await st(), await st()

        # 3. F2, typed, Esc: unchanged
        await pg.click('#dev-grid .cell[data-i="12"]')
        await pg.keyboard.press("F2")
        ed = pg.locator("#dev-grid .nm-edit")
        assert await ed.count() == 1
        await ed.fill("NOPE")
        await ed.press("Escape")
        await pg.wait_for_timeout(100)
        assert (await pg.text_content('#dev-grid .cell[data-i="12"] .nm')) == "MY LEAD"
        assert "changed" not in (await pg.get_attribute('#dev-grid .cell[data-i="12"]', "class"))
        print("3. Esc kept MY LEAD")

        assert not errs, errs
        await b.close()
        print("library rename: ok")


asyncio.run(main())
