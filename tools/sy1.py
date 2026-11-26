#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home). The codec and the legacy conversion are Synth2's (EngineCore/Sy1Codec.hpp,
# LegacyTables.hpp: MIT, Copyright (c) 2026 Francois Chabloz), measured by it on the original Synth1.
"""Synth1 .sy1 patches for M-Vsynth2: read, convert old versions, clamp, pack into the firmware's record, and back.

A .sy1 is text: line 1 the name, then "color=" / "ver=" lines, then "index,value" lines of native values; a missing
line keeps the original's default. The firmware never sees the text: this module (and its JavaScript twin for
docs/library.html, Stage 6) turns a file into the 120-byte record of firmware/src/vs_patch.h, and a record back into
a .sy1 written as ver=112, as Synth2 writes it, so it opens in Synth2 and in Synth1.

  python3 tools/sy1.py show FILE.sy1 ..        what a file holds after loading (the values that differ from INIT)
  python3 tools/sy1.py check FILE|FOLDER ..    every file loads, packs, unpacks and writes back to the same values

What a load does, in Synth2's order (ParseSy1):
  1. each "index,value" is clamped to the parameter's range (the original plays an out-of-range value as the
     nearest in range)
  2. a version the original converts (no ver line = 100; 100; 105 / 106; 107 / 108) is converted by its rules:
     tables for the knobs whose law changed, fixed values for the parameters the version lacked, the two CTRL
     sens values, the unison settings by play mode. Any other version is left as it is
The name becomes 16 ASCII characters: old patches are Shift-JIS or Windows-1252, the FM-1's font is ASCII."""
import re, sys, unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import synth2

NPARAM, NAME, REC, EMPTY = 99, 16, 120, 0x7F
ROUTING = (86, 87, 88, 89)
REC_ID = [i for i in range(NPARAM) if i not in ROUTING]
COLORS = ["default", "red", "blue", "green", "yellow"]
FAV = 0x40                                      # the record's last byte: the favourite bit (vs_patch.h VS_REC_FAV)
PLAY_MODE, LFO1_SPEED, LFO1_DEPTH, SENS1, SENS2, UNI_DETUNE, UNI_VOICES = 38, 43, 44, 50, 51, 75, 93
VER100, VER105, VER107 = 1, 2, 4

_cache = {}


def own_hi():
    """M-Vsynth2's own range ends past Synth2's (vsynth2-params.yaml hi:, as gen_params.py): FILTER TYPE 5, SH101."""
    import yaml
    ovl = yaml.safe_load((Path(__file__).resolve().parent / "vsynth2-params.yaml").read_text(encoding="utf-8"))["params"]
    return {i: o["hi"] for i, o in ovl.items() if "hi" in o}


OWN_HI = own_hi()


def tables():
    """Ranges and defaults (Params.hpp) and the legacy rules (LegacyTables.hpp), read once from Synth2."""
    if not _cache:
        r = synth2.root()
        rows, init = synth2.params(r)
        rows = [(*x[:3], OWN_HI.get(i, x[3]), x[4]) for i, x in enumerate(rows)]
        txt = synth2.strip_comments((r / "EngineCore/LegacyTables.hpp").read_text())
        flat = [int(v) for v in synth2.c_array(txt, "kTables")]
        if len(flat) % 128:
            raise SystemExit("sy1: LegacyTables.hpp kTables is not rows of 128")
        groups = {"kVer100": VER100, "kVer105": VER105, "kVer107": VER107}
        body = re.search(r"kRules\[\]\s*=\s*\{(.*?)\};", txt, re.S).group(1)
        rules = [(int(p), int(t), int(v), sum(groups[g.strip()] for g in gs.split("|")))
                 for p, t, v, gs in re.findall(r"\{\s*(\d+),\s*(-?\d+),\s*(-?\d+),\s*([\w| ]+?)\s*\}", body)]
        poly = int(re.search(r"kUnisonDetunePoly\s*=\s*(\d+)", txt).group(1))
        mono = int(re.search(r"kUnisonDetuneMono\s*=\s*(\d+)", txt).group(1))
        if not rules:
            raise SystemExit("sy1: LegacyTables.hpp kRules not read")
        _cache.update(lo=[x[2] for x in rows], hi=[x[3] for x in rows], de=[x[4] for x in rows], init=init,
                      tab=[flat[i:i + 128] for i in range(0, len(flat), 128)], rules=rules, poly=poly, mono=mono)
    return _cache


def clamp(i, v):
    t = tables()
    return max(t["lo"][i], min(t["hi"][i], v))


def legacy_group(version):
    return {0: VER100, 100: VER100, 105: VER105, 106: VER105, 107: VER107, 108: VER107}.get(version, 0)


def convert_legacy(values, version):
    """Synth2's ConvertLegacy: the original's conversion of an old-version patch (values as read, clamped)."""
    g = legacy_group(version)
    if not g:
        return list(values)
    t, old, v = tables(), list(values), list(values)
    c127 = lambda x: max(0, min(127, x))
    for param, table, value, groups in t["rules"]:
        if groups & g:
            v[param] = value if table < 0 else t["tab"][table][c127(old[param])]
    for sens, target in ((SENS1, LFO1_DEPTH), (SENS2, LFO1_SPEED)):
        v[sens] = 64 + old[sens] * (127 - old[target]) * 63 // (127 * 127)
    mono = old[PLAY_MODE] != 0
    v[UNI_VOICES] = 4 if mono else 2
    if g == VER107:
        v[UNI_DETUNE] = t["tab"][t["mono"] if mono else t["poly"]][c127(old[UNI_DETUNE])]
    else:
        v[UNI_DETUNE] = 70 if mono else 48
    return v


def ascii_name(raw):
    """A name's bytes as 16 ASCII characters. UTF-8 if it is, else Shift-JIS, else Windows-1252 (Synth2's
    TextEncoding order); accents dropped to their letters, what has no ASCII form to '?'."""
    for enc in ("utf-8", "cp932", "cp1252"):
        try:
            s = raw.decode(enc)
            break
        except UnicodeDecodeError:
            s = raw.decode("latin-1")
    out = ""
    for ch in unicodedata.normalize("NFKD", s):
        if unicodedata.combining(ch):
            continue
        out += ch if 32 <= ord(ch) <= 126 else "?"
    return out.strip()[:NAME].ljust(NAME)


def parse(data):
    """A .sy1 file's bytes -> {"name", "color", "version", "values", "fav"} as Synth2's ParseSy1 loads it ("fav=1":
    M-Vsynth2's favourite star, issue #12; Synth2 skips a key it doesn't know)."""
    t = tables()
    lines = re.split(rb"\r\n|\r|\n", data)
    p = {"name": ascii_name(lines[0] if lines else b""), "color": "default", "version": 0, "values": list(t["de"]),
         "fav": False}
    for line in lines[1:]:
        s = line.decode("latin-1")
        if "=" in s:
            k, val = s.split("=", 1)
            if k == "ver" and re.fullmatch(r"\s*-?\d+\s*", val): p["version"] = int(val)
            elif k == "color": p["color"] = val
            elif k == "fav": p["fav"] = val.strip() == "1"
        elif "," in s:
            a, b = s.split(",", 1)
            if re.fullmatch(r"\s*[-+]?\d+\s*", a) and re.fullmatch(r"\s*[-+]?\d+\s*", b) and 0 <= int(a) < NPARAM:
                p["values"][int(a)] = clamp(int(a), int(b))
    p["values"] = convert_legacy(p["values"], p["version"])
    return p


def write(p):
    """The patch as .sy1 text, ver=112 (Synth2's WriteSy1: the newest version the original's loader reads)."""
    return (p["name"].rstrip() + "\ncolor=" + p["color"] + ("\nfav=1" if p.get("fav") else "") + "\nver=112\n" +
            "".join(f"{i},{v}\n" for i, v in enumerate(p["values"]))).encode("ascii")


def pack(p):
    """The firmware's record (vs_patch.h vs_rec_t): 120 bytes."""
    t = tables()
    r = bytearray(REC)
    r[:NAME] = p["name"].ljust(NAME)[:NAME].encode("ascii")
    for j, i in enumerate(REC_ID):
        r[NAME + j] = clamp(i, p["values"][i]) - t["lo"][i]
    for k, i in enumerate(ROUTING):
        v = clamp(i, p["values"][i])
        w = 0xFFFF if v < 0 else v
        r[NAME + len(REC_ID) + 2 * k] = w & 0xFF
        r[NAME + len(REC_ID) + 2 * k + 1] = w >> 8
    r[REC - 1] = (COLORS.index(p["color"]) if p["color"] in COLORS else 0) | (FAV if p.get("fav") else 0)
    if r[NAME] == EMPTY:
        raise SystemExit("sy1: a record's first value byte can not be the EMPTY mark")
    return bytes(r)


def empty():
    r = bytearray(REC)
    r[:NAME] = b" " * NAME
    r[NAME] = EMPTY
    return bytes(r)


def unpack(r):
    """A record -> the patch (None: an EMPTY slot)."""
    if r[NAME] == EMPTY:
        return None
    t = tables()
    v = [0] * NPARAM
    for j, i in enumerate(REC_ID):
        v[i] = clamp(i, r[NAME + j] + t["lo"][i])
    for k, i in enumerate(ROUTING):
        w = r[NAME + len(REC_ID) + 2 * k] | r[NAME + len(REC_ID) + 2 * k + 1] << 8
        v[i] = -1 if w == 0xFFFF else w
    c = r[REC - 1] & (FAV - 1)
    return {"name": bytes(r[:NAME]).decode("ascii", "replace"), "color": COLORS[c] if c < len(COLORS) else "default",
            "version": 112, "values": v, "fav": bool(r[REC - 1] & FAV)}


def files(args):
    for a in args:
        p = Path(a)
        yield from (sorted(p.rglob("*.sy1")) if p.is_dir() else [p])


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("show", "check"):
        sys.exit(__doc__)
    t, n, bad = tables(), 0, 0
    for f in files(sys.argv[2:]):
        p = parse(f.read_bytes())
        n += 1
        if sys.argv[1] == "show":
            diff = " ".join(f"{i}={v}" for i, v in enumerate(p["values"]) if v != t["init"][i])
            print(f"{f}: '{p['name'].rstrip()}' color {p['color']} ver {p['version']}: {diff or '(the INIT values)'}")
            continue
        q = unpack(pack(p))
        again = parse(write(q))
        # 65535 in a routing word is the one value a record can not hold (it reads back as -1, none)
        want = [(-1 if i in ROUTING and v == 65535 else v) for i, v in enumerate(p["values"])]
        if q["values"] != want or again["values"] != want or q["name"] != p["name"]:
            print(f"FAIL {f}")
            bad += 1
    if sys.argv[1] == "check":
        print(f"sy1 check: {n} files, {bad} failed")
        sys.exit(1 if bad or not n else 0)


if __name__ == "__main__":
    main()
