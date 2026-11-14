# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
"""Where Synth2's sources are, for the generators, tools/sy1.py's legacy tables and the compare tools.

Synth2 (https://github.com/chab/oxo_synth2, MIT, Copyright (c) 2026 Francois Chabloz) is never compiled into the
firmware: the generators read its headers and write firmware/gen/, which is committed. So a build needs no checkout;
regenerating does:
    git submodule add https://github.com/chab/oxo_synth2 third_party/oxo_synth2     (once; pinned at PIN)
or  OXO_SYNTH2=/path/to/oxo_synth2 python3 tools/gen_params.py"""
import os, re, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "f3dc688"                                          # the commit the port was made against (2026-11-13; EngineCore as at 6953424)


def root(required=True):
    for p in (os.environ.get("OXO_SYNTH2"), ROOT / "third_party" / "oxo_synth2"):
        if p and (Path(p) / "EngineCore" / "Params.hpp").is_file():
            return Path(p)
    if required:
        raise SystemExit("Synth2's sources are not here: see tools/synth2.py (the submodule, or OXO_SYNTH2)")
    return None


def pin_note(r):
    """A line to print when the checkout is not the pinned commit ('' when it is, or when git cannot say)."""
    try:
        head = subprocess.run(["git", "-C", str(r), "rev-parse", "--short=7", "HEAD"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""
    return "" if not head or head == PIN else f"note: Synth2 is at {head}, the port was made against {PIN}: check what changed"


def strip_comments(s):
    return re.sub(r"//[^\n]*", "", re.sub(r"/\*.*?\*/", "", s, flags=re.S))


def c_array(text, name):
    """The numbers of `name[..] = { .. };` (C expressions like 4.0 / 3 evaluated), as floats."""
    m = re.search(r"\b" + re.escape(name) + r"\s*(?:\[[^\]]*\])*\s*=\s*\{(.*?)\};", strip_comments(text), re.S)
    if not m:
        raise SystemExit(f"Synth2: table {name} not found")
    out = []
    for item in m.group(1).replace("{", "").replace("}", "").split(","):
        item = item.strip()
        if item:
            if not re.fullmatch(r"[-+0-9.eE/* ()fxXa-fA-F]+", item):
                raise SystemExit(f"Synth2: {name}: cannot read {item!r}")
            out.append(float(eval(re.sub(r"(?<=[0-9.])f\b", "", item))))
    return out


def params(r):
    """[(key, name, min, max, def)] x 99 from Params.hpp, and the INIT patch's 99 values from InitPatch.hpp."""
    rows = re.findall(r'\{"(\w+)", "([^"]+)", (-?\d+), (-?\d+), (-?\d+), (?:true|false)\}', (r / "EngineCore/Params.hpp").read_text())[:99]
    init = [int(v) for v in c_array((r / "EngineCore/InitPatch.hpp").read_text(), "kInitPatchValues")]
    if len(rows) != 99 or len(init) != 99:
        raise SystemExit("Synth2: Params.hpp / InitPatch.hpp do not hold 99 parameters")
    return [(k, n, int(a), int(b), int(d)) for k, n, a, b, d in rows], init
