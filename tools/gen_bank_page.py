#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home).
"""The bank page became the patch library (docs/library.html): this runs tools/gen_library_page.py, which also writes
docs/bank.html as a pointer to it. Kept so old notes and scripts still work."""
import runpy
from pathlib import Path

runpy.run_path(str(Path(__file__).resolve().parent / "gen_library_page.py"), run_name="__main__")
