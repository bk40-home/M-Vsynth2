#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/sy1.py against Synth2's own test data (needs Synth2's sources: tools/synth2.py):
  legacy   EngineCoreTests/legacy_fixture.txt: 203 old patches (no ver, 100, 105..108) with the values the ORIGINAL
           Synth1 keeps after loading them (Synth2's check_legacy.py --fixture). Every value must match
  spot     the cases of Synth2's ParamsTests.cpp: clamping, a missing line, a legacy patch without a ver line
  banks    every .sy1 Synth2 ships loads, packs, unpacks and writes back to the same values; 001 is the INIT patch
  names    Shift-JIS, Windows-1252 and long names become 16 ASCII characters
  factory  (given tests/vs_patch_test's binary) the factory bank as the firmware's C unpacks it is what the files hold"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sy1, synth2

fails = 0


def expect(ok, what):
    global fails
    if not ok:
        fails += 1
        print("FAIL", what)


def text(ver, vals):
    return ("fixture\n" + (f"ver={ver}\n" if ver else "") + "".join(f"{i},{v}\n" for i, v in enumerate(vals))).encode()


r = synth2.root()
n = 0
for line in (r / "EngineCoreTests/legacy_fixture.txt").read_text().splitlines():
    if not line.strip():
        continue
    left, right = line.split("|")
    a, want = [int(x) for x in left.split()], [int(x) for x in right.split()]
    got = sy1.parse(text(a[0], a[1:]))["values"]
    bad = [i for i in range(99) if got[i] != want[i]]
    expect(not bad, f"legacy patch {n} ver {a[0]}: parameters {bad}")
    n += 1
expect(n >= 200, f"only {n} legacy patches in the fixture")

p = sy1.parse(b"Lead A\r\ncolor=red\r\nver=112\r\n0,3\r\n9,-12\r\n19,127\r\n94,32\r\n21,128\r\n")
t = sy1.tables()
expect(p["name"].rstrip() == "Lead A" and p["color"] == "red" and p["version"] == 112, "current format: header")
expect((p["values"][0], p["values"][9], p["values"][19], p["values"][94]) == (3, -12, 127, 32), "current format: values")
expect(p["values"][21] == 127, "an out-of-range value is clamped")
expect(p["values"][2] == t["de"][2], "a missing line keeps the original's default")
expect(sy1.parse(b"Rydeen melo A1\n0,1\n45,0\n33,64\n")["values"][33] == 18, "no ver line: legacy, ARP BEAT clamped")
p = sy1.parse(b"old\nver=105\n21,0\n26,20\n38,0\n44,19\n50,127\n87,10\n93,8\n")
expect((p["values"][21], p["values"][26], p["values"][93]) == (63, 8, 2), "ver 105 spot values")

init = sy1.parse((r / "Shared/Resources/Synth2 Factory/001.sy1").read_bytes())
expect(init["values"] == t["init"] and init["name"].rstrip() == "INIT", "Synth2 Factory 001 is the INIT patch")
m = 0
for f in sorted((r / "Shared/Resources").rglob("*.sy1")):
    p = sy1.parse(f.read_bytes())
    q = sy1.unpack(sy1.pack(p))
    expect(q["values"] == p["values"] and q["name"] == p["name"] and q["color"] == p["color"], f"record round trip: {f.name}")
    expect(sy1.parse(sy1.write(q))["values"] == p["values"], f".sy1 round trip: {f.name}")
    m += 1
expect(m >= 180, f"only {m} .sy1 files found in Synth2")
expect(sy1.unpack(sy1.empty()) is None, "an EMPTY record unpacks as none")

# the factory bank as the firmware's own C reads it (tests/vs_patch_test's dump), against the .sy1 files
if len(sys.argv) > 1:
    import subprocess
    dump = subprocess.run([sys.argv[1], "dump"], capture_output=True, text=True, check=True).stdout.splitlines()
    root = Path(__file__).resolve().parents[1]
    files = {int(f.name[:3]): f for f in (root / "tools/bank/factory").glob("[0-9][0-9][0-9]*.sy1")}
    expect(len(dump) == len(files) and len(files) > 0, f"the factory bank: {len(dump)} records for {len(files)} files")
    for line in dump:
        head, vals = line.split("|")
        slot, color = int(head.split()[0]), int(head.split()[1])
        name = head.split(" ", 2)[2]
        p = sy1.parse(files[slot].read_bytes())
        expect([int(x) for x in vals.split()] == p["values"] and name == p["name"] and sy1.COLORS[color] == p["color"],
               f"factory {slot:03d}: the C's record is not the file's patch")

expect(sy1.ascii_name("ｼﾝｾ Lead".encode("cp932")).rstrip() == "??? Lead" or sy1.ascii_name("ｼﾝｾ Lead".encode("cp932")).isascii(), "a Shift-JIS name is ASCII")
expect(sy1.ascii_name("Caf\xe9 Pad".encode("cp1252")).rstrip() == "Cafe Pad", "a Windows-1252 accent drops to its letter")
expect(len(sy1.ascii_name(b"A very long patch name indeed")) == 16, "a long name is cut to 16")
print(f"sy1_test: {n} legacy patches, {m} files" + (f": {fails} FAILED" if fails else ": ok"))
sys.exit(1 if fails else 0)
