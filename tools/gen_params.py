#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
"""Generate firmware/gen/vs_params.h: the M-Vsynth2 parameter table, keyed by Synth1 parameter index.

  python3 tools/gen_params.py            (run from the repo root; the output is committed)

Inputs:
  Synth2 (tools/synth2.py says where)   EngineCore/Params.hpp (ranges, the original's defaults), InitPatch.hpp (INIT),
                                        MeasuredTables.hpp, Lfo.hpp, Effects.hpp (what a value reads as)
  tools/vsynth2-params.yaml             our overlay: C name, panel label, section, kind, CC, choices; the pages; hi: a
                                        range end of our own past Synth2's (FILTER TYPE 5, SH101)

What comes out (all const, so it costs flash, not RAM):
  VS_P_<KEY>         the parameter index (= the .sy1 line index)
  VS_PARAMS[]        range, default, INIT value, section, kind, CC, first choice, label
  VS_CC_PARAM[128]   the panel's CC -> parameter (0xFF: not mapped)
  VS_REC_ID[95]      the stored record's value bytes -> parameter (every one but the four routing words)
  VS_CHOICE[]        the names of the enum parameters' values
  VS_PAGES[]         the panel: button, title, page n of m, four parameters
  value texts        OSC2 PITCH semitones, FINE / TUNE cents, LFO SPEED in Hz and as note values, ARP BEAT and
                     DELAY TIME note values, the EFFECT knobs' names per type (Synth2's ParamFormat.hpp)
The script fails (exit 1) on anything inconsistent, so a bad edit never reaches the firmware."""
import math, sys
from pathlib import Path

import yaml

sys.path.insert(0, str(Path(__file__).resolve().parent))
import synth2

ROOT = Path(__file__).resolve().parents[1]
OVL = ROOT / "tools/vsynth2-params.yaml"
OUT = ROOT / "firmware/gen/vs_params.h"
KINDS = ["num", "signed", "switch", "enum", "table", "src", "dst"]
BUTTONS = ["LFO", "EDIT", "ENV", "SEL", "FX", "GLO", "ARP"]
ROUTING = (86, 87, 88, 89)
CC_TAKEN = {0, 1, 6, 32, 38, 64, 65, 66, 67, 68, 69, *range(96, 102), *range(120, 128)}


def fail(msg):
    sys.exit(f"gen_params: {msg}")


def division(beats):
    """Synth2's DivisionLabel, in the panel's capitals: 0.5 -> 1/8, 1/12 -> 1/32T, 0.75 -> 1/8., 4 -> 1 BAR."""
    if beats >= 8.0 - 1e-6 or abs(beats - 4.0) < 1e-6:
        bars = beats / 4.0
        return ("%.0f BAR%s" % (bars, "" if abs(bars - 1) < 1e-6 else "S")) if abs(bars - round(bars)) < 1e-6 else "%.3g BARS" % bars
    whole = beats / 4.0
    q = 1
    while q <= 512:
        if abs(whole * q - 1) < 1e-4: return f"1/{q}"
        if abs(whole * q * 1.5 - 1) < 1e-4: return f"1/{q}T"
        if abs(whole * q / 1.5 - 1) < 1e-4: return f"1/{q}."
        if abs(whole * q / 1.75 - 1) < 1e-4: return f"1/{q}.."
        q *= 2
    q = 1
    while q <= 512:
        for p in range(1, q):
            if abs(whole * q - p) < 1e-4: return f"{p}/{q}"
        q *= 2
    fail(f"no note value for {beats} beats")


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def rows16(vals, fmt="%d"):
    return "\n".join("    " + ", ".join(fmt % v for v in vals[i:i + 16]) + "," for i in range(0, len(vals), 16))


def texts(name, items, width, what):
    if max(len(s) for s in items) >= width:
        fail(f"{name}: a text is longer than {width - 1}")
    return [f"/* {what} */", f"static const char {name}[{len(items)}][{width}] = {{",
            "\n".join("    " + ", ".join(c_str(s) for s in items[i:i + 8]) + "," for i in range(0, len(items), 8)), "};", ""]


def main():
    s2 = synth2.root()
    note = synth2.pin_note(s2)
    if note:
        print(note)
    rows, init = synth2.params(s2)
    y = yaml.safe_load(OVL.read_text(encoding="utf-8"))
    ovl, pages, fxk = y["params"], y["pages"], y["fx_knobs"]
    if sorted(ovl) != list(range(99)):
        fail("the overlay must have parameters 0..98")
    for i in range(99):                         # M-Vsynth2's own range ends (the overlay's hi: FILTER TYPE's SH101)
        if "hi" in ovl[i]:
            if ovl[i]["hi"] < rows[i][3]: fail(f"{i}: hi {ovl[i]['hi']} is below Synth2's {rows[i][3]}")
            rows[i] = (*rows[i][:3], ovl[i]["hi"], rows[i][4])
    secs, keys, choices, ch0, ccs = [], set(), [], {}, {}
    for i in range(99):
        o, (_, _, lo, hi, de) = ovl[i], rows[i]
        if o["kind"] not in KINDS: fail(f"{i}: kind {o['kind']}")
        if len(o["label"]) > 7: fail(f"{i}: label {o['label']!r} is longer than 7")
        if o["key"] in keys: fail(f"{i}: key {o['key']} twice")
        keys.add(o["key"])
        if o["sec"] not in secs: secs.append(o["sec"])
        if not (lo <= de <= hi and lo <= init[i] <= hi): fail(f"{i}: default or INIT out of range")
        if o["kind"] == "enum":
            ch = [str(c) for c in o["choices"]]
            if len(ch) != hi - lo + 1: fail(f"{i}: {len(ch)} choices for {lo}..{hi}")
            ch0[i] = len(choices)
            choices += ch
        if o["kind"] == "switch" and (lo, hi) != (0, 1): fail(f"{i}: a switch is 0..1")
        cc = o.get("cc")
        if cc is not None:
            if cc in CC_TAKEN or cc in ccs or not 0 < cc < 128: fail(f"{i}: CC {cc} is taken")
            if i in ROUTING or hi - lo > 127: fail(f"{i}: this parameter can have no CC")
            ccs[cc] = i
    # ---- pages: every parameter once, but ARP's switch
    seen, out_pages, count, idx = [], [], {}, {}
    for p in pages:
        if p["btn"] not in BUTTONS: fail(f"page {p['title']}: button {p['btn']}")
        if len(p["knobs"]) != 4 or len(p["title"]) > 12: fail(f"page {p['title']}: 4 knobs, a title of 12 at most")
        count[(p["btn"], p["title"])] = count.get((p["btn"], p["title"]), 0) + 1
    for p in pages:
        k = (p["btn"], p["title"])
        idx[k] = idx.get(k, 0) + 1
        ids = [255 if i is None else i for i in p["knobs"]]
        seen += [i for i in ids if i != 255]
        out_pages.append((p["btn"], p["title"], idx[k], count[k], ids))
    if len(seen) != len(set(seen)): fail("a parameter is on two pages")
    if sorted(set(range(99)) - set(seen)) != [59]: fail(f"not on a page: {sorted(set(range(99)) - set(seen))} (only 59 may be)")
    # ---- what values read as (Synth2's laws)
    mt = (s2 / "EngineCore/MeasuredTables.hpp").read_text()
    semis = [int(v) for v in synth2.c_array(mt, "kOsc2PitchSemis")]
    cents = [int(v) for v in synth2.c_array(mt, "kFineCents")]
    arp = synth2.c_array((s2 / "EngineCore/Params.hpp").read_text(), "kArpBeats")
    dly = synth2.c_array((s2 / "EngineCore/Effects.hpp").read_text(), "kDelayBeats")
    syn = synth2.c_array((s2 / "EngineCore/Lfo.hpp").read_text(), "kSyncBeats")
    if (len(semis), len(cents), len(arp), len(dly), len(syn)) != (128, 128, 19, 20, 30): fail("a Synth2 table changed its size")
    if min(semis + cents) < -127 or max(semis + cents) > 127: fail("semitones / cents do not fit int8")
    hz = [2.0 ** (s / 12.0367) / 12.0 for s in range(128)]            # Lfo::SpeedHz
    lfo_txt = [round(h * 100) if h < 10 else round(h * 10) for h in hz]   # "%.2f Hz" below 10, "%.1f Hz" from it
    tenths = next(s for s in range(128) if hz[s] >= 10)
    fx_types = [str(c) for c in ovl[78]["choices"]]
    if list(fxk) != fx_types: fail("fx_knobs must list the EFFECT types in their order")

    h = ["/* SPDX-License-Identifier: GPL-3.0-only */",
         "/* GENERATED by tools/gen_params.py from tools/vsynth2-params.yaml and Synth2's EngineCore (Params.hpp,",
         " * InitPatch.hpp, MeasuredTables.hpp, Lfo.hpp, Effects.hpp: MIT, Copyright (c) 2026 Francois Chabloz).",
         " * Do not edit by hand. The 99 Synth1 parameters in Synth1's own index order (= the .sy1 line index), native",
         " * integer values. */", "#pragma once", "#include <stdint.h>", "",
         "#define VS_NPARAM 99u", "#define VS_NREC 95u                             /* parameters stored a byte each (vs_patch.h) */", "", "enum {"]
    h += [f"    VS_P_{ovl[i]['key']} = {i}," for i in range(99)] + ["};", ""]
    h += ["enum { " + ", ".join(f"VS_SEC_{s}" for s in secs) + " };",
          "enum { " + ", ".join(f"VS_K_{k.upper()}" for k in KINDS) + " };",
          "enum { " + ", ".join(f"VS_BTN_{b}" for b in BUTTONS) + " };   /* a page's button (vs_ui.c maps it to the platform's) */", "",
          "typedef struct {",
          "    int32_t min, max;                           /* native range (Params.hpp) */",
          "    int32_t def;                                /* the ORIGINAL's init value: a .sy1's missing-line fallback */",
          "    int32_t init;                               /* the INIT patch (InitPatch.hpp): INIT, and a knob's reset */",
          "    uint8_t sec, kind;                          /* VS_SEC_*, VS_K_* */",
          "    uint8_t cc;                                 /* the panel's CC, 0xFF none */",
          "    uint8_t choice;                             /* VS_K_ENUM: its first name in VS_CHOICE (value min) */",
          "    char label[8];                              /* panel label */", "} vs_param_info_t;", "",
          "static const vs_param_info_t VS_PARAMS[VS_NPARAM] = {"]
    inv = {i: c for c, i in ccs.items()}
    for i in range(99):
        o, (_, name, lo, hi, de) = ovl[i], rows[i]
        ent = f"{{{lo}, {hi}, {de}, {init[i]}, VS_SEC_{o['sec']}, VS_K_{o['kind'].upper()}, {inv.get(i, 255)}, {ch0.get(i, 255)}, {c_str(o['label'])}}},"
        h.append(f"    {ent:<74}/* {i} {name} */")
    h += ["};", "", "/* the panel's CC -> parameter (0xFF: not mapped) */", "static const uint8_t VS_CC_PARAM[128] = {",
          rows16([ccs.get(c, 255) for c in range(128)], "%3d"), "};", "",
          "/* the stored record's value bytes -> parameter: every parameter but the routing words 86..89 */",
          "static const uint8_t VS_REC_ID[VS_NREC] = {", rows16([i for i in range(99) if i not in ROUTING]), "};", ""]
    h += texts("VS_CHOICE", choices, 8, "the enum parameters' value names (VS_PARAMS[].choice + value - min)")
    h += ["/* the panel: a page's button, its title, page n of m, its four parameters (0xFF: an empty column) */",
          "typedef struct {", "    uint8_t btn, n, of;", "    uint8_t id[4];", "    char title[13];", "} vs_page_t;", "",
          f"#define VS_NPAGE {len(out_pages)}u", "static const vs_page_t VS_PAGES[VS_NPAGE] = {"]
    h += [f"    {{VS_BTN_{b}, {n}, {of}, {{{', '.join(str(i) for i in ids)}}}, {c_str(t)}}}," for b, t, n, of, ids in out_pages]
    h += ["};", "",
          "/* OSC2 PITCH -> semitones; FINE and TUNE -> cents (Synth2 kOsc2PitchSemis, kFineCents) */",
          "static const int8_t VS_OSC2_SEMIS[128] = {", rows16(semis), "};",
          "static const int8_t VS_FINE_CENTS[128] = {", rows16(cents), "};", "",
          "/* LFO SPEED as it reads: Hz = 2^(s / 12.0367) / 12, in 1/100 Hz below VS_LFO_TENTHS, in 1/10 Hz from it */",
          f"#define VS_LFO_TENTHS {tenths}u", "static const uint16_t VS_LFO_HZ[128] = {", rows16(lfo_txt), "};", ""]
    h += texts("VS_LFO_SYNC", [division(b) for b in syn], 10, "LFO SPEED with TEMPO on: step = speed x 30 / 128 (Synth2 Lfo::kSyncBeats)")
    h += texts("VS_ARP_BEAT", [division(b) for b in arp], 9, "ARP BEAT (Synth2 kArpBeats)")
    h += texts("VS_DLY_TIME", ["MIN"] + [division(b) for b in dly[1:]], 9, "DELAY TIME (Synth2 kDelayBeats; 0: the shortest)")
    flat = [str(k) for t in fx_types for k in fxk[t]]
    if any(len(k) > 7 for k in flat) or len(flat) != 30: fail("fx_knobs: 3 names of 7 at most a type")
    h += texts("VS_FX_KNOB", flat, 8, "EFFECT's CTRL 1, CTRL 2, LEVEL by type: VS_FX_KNOB[type x 3 + knob]")
    OUT.write_text("\n".join(h))
    print(f"gen_params: {OUT.relative_to(ROOT)}: 99 parameters, {len(out_pages)} pages, {len(ccs)} CCs, {len(choices)} choice names")


main()
