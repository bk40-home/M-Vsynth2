#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home).
"""Patches to and from the FM-1 over USB-MIDI (firmware/src/vs_xfer.h is the protocol). Needs mido and python-rtmidi:
    pip install mido python-rtmidi

  python3 tools/sy1_xfer.py ident                          who is there
  python3 tools/sy1_xfer.py send FILES|FOLDER --bank 2     .sy1 files into U2, from their numbers (012.sy1 -> slot 12)
                                  [--from 1]               .. or one after another from slot 1 (in name order)
  python3 tools/sy1_xfer.py read --bank 2 OUT/             all 128 slots of U2 as OUT/NNN.sy1 (a Synth1 bank folder;
                                                           --bank 0: FACTORY)
  python3 tools/sy1_xfer.py play FILE.sy1                  an audition: it plays now, nothing is saved
  python3 tools/sy1_xfer.py select BANK SLOT               the FM-1 shows bank BANK and plays slot SLOT (1..128)
  python3 tools/sy1_xfer.py clear --bank 2 SLOT..          empty slots
  --port NAME   the MIDI port (default: the first whose name has "FM-1", "M-VAVE" or "SMK")

Every write is acknowledged: the tool sends the next one when the FM-1 has answered, and sends one again when the
FM-1 says it is busy (a bank with unsaved changes while something sounds: lift your hands off the keys)."""
import re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sy1

HDR = [0x7D, 0x4D, 0x56]
WRITE, CLEAR, PLAY, READ, IDENT, SELECT = 1, 2, 3, 4, 5, 6
DATA, IDREP, ACK = 0x11, 0x15, 0x7F
STATUS = {0: "ok", 1: "bad message", 2: "refused (FACTORY, or out of range)", 3: "busy"}


def pack7(b):
    out = []
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << k for k, x in enumerate(g)))
        out.extend(x & 0x7F for x in g)
    return out


def unpack7(p):
    out, i = [], 0
    while i < len(p):
        top = p[i]
        i += 1
        for k in range(7):
            if i >= len(p):
                break
            out.append((p[i] & 0x7F) | ((top >> k) & 1) << 7)
            i += 1
    return bytes(out)


def body(cmd, data):
    s = (cmd + sum(data)) & 0x7F
    return HDR + [cmd] + list(data) + [(128 - s) & 0x7F]


class Link:
    def __init__(self, port):
        import mido
        self.mido = mido
        names = mido.get_output_names()
        want = port or next((n for n in names if re.search(r"FM-1|M-VAVE|SMK", n, re.I)), None)
        if not want:
            sys.exit(f"sy1_xfer: no FM-1 among the MIDI ports {names}: --port NAME")
        self.out = mido.open_output(want)
        ins = mido.get_input_names()
        self.inp = mido.open_input(want if want in ins else next(n for n in ins if n.split(":")[0] == want.split(":")[0]))

    def send(self, cmd, data=()):
        self.out.send(self.mido.Message("sysex", data=body(cmd, data)))

    def recv(self, timeout=2.0):
        end = time.time() + timeout
        while time.time() < end:
            for m in self.inp.iter_pending():
                if m.type == "sysex" and list(m.data[:3]) == HDR:
                    return list(m.data[3:-1])   # cmd, data (the sum dropped)
            time.sleep(0.002)
        return None

    def acked(self, cmd, data, what):
        for attempt in range(50):
            self.send(cmd, data)
            r = self.recv()
            if r is None:
                sys.exit(f"sy1_xfer: no answer to {what}")
            if r[0] == ACK and r[1] == cmd:
                if r[4] == 3:
                    time.sleep(0.3)             # busy: again
                    continue
                if r[4]:
                    sys.exit(f"sy1_xfer: {what}: {STATUS.get(r[4], r[4])}")
                return
        sys.exit(f"sy1_xfer: {what}: still busy (lift your hands off the keys)")


def main():
    a = sys.argv[1:]
    if not a or a[0] not in ("ident", "send", "read", "play", "select", "clear"):
        sys.exit(__doc__)

    def opt(name, default=None):
        if name in a:
            i = a.index(name)
            v = a[i + 1]
            del a[i:i + 2]
            return v
        return default
    port = opt("--port")
    bank = int(opt("--bank", "1"))
    start = opt("--from")
    cmd = a.pop(0)
    ln = Link(port)
    if cmd == "ident":
        ln.send(IDENT)
        r = ln.recv()
        if not r or r[0] != IDREP:
            sys.exit("sy1_xfer: no IDENT answer")
        z = r[1:].index(0) + 1
        print(bytes(r[1:z]).decode("ascii"), "voices", r[z + 1], "banks", r[z + 2])
    elif cmd == "send":
        files = list(sy1.files(a))
        for n, f in enumerate(files):
            m = re.match(r"(\d+)", f.stem)
            slot = int(start) - 1 + n if start else (int(m.group(1)) - 1 if m else n)
            if not 0 <= slot < 128:
                print(f"skip {f}: slot {slot + 1}")
                continue
            rec = sy1.pack(sy1.parse(f.read_bytes()))
            ln.acked(WRITE, [bank, slot] + pack7(rec), f"{f.name} -> U{bank} {slot + 1:03}")
            print(f"{f.name} -> U{bank} {slot + 1:03}")
    elif cmd == "read":
        out = Path(a[0] if a else f"bank{bank}")
        out.mkdir(parents=True, exist_ok=True)
        ln.send(READ, [bank, 0, 0])
        got = 0
        while got < 128:
            r = ln.recv()
            if r is None:
                sys.exit(f"sy1_xfer: the read stopped at {got}")
            if r[0] == ACK:
                sys.exit(f"sy1_xfer: read: {STATUS.get(r[4], r[4])}")
            if r[0] != DATA:
                continue
            p = sy1.unpack(unpack7(r[3:3 + 138]))   # (HOME's knobs after it: not in a .sy1)
            if p:
                (out / f"{r[2] + 1:03}.sy1").write_bytes(sy1.write(p))
            got += 1
        print(f"read {'FACTORY' if bank == 0 else f'U{bank}'} -> {out}/")
    elif cmd == "play":
        rec = sy1.pack(sy1.parse(Path(a[0]).read_bytes()))
        ln.acked(PLAY, pack7(rec), a[0])
    elif cmd == "select":
        ln.acked(SELECT, [int(a[0]), int(a[1]) - 1], "select")
    elif cmd == "clear":
        for s in a:
            ln.acked(CLEAR, [bank, int(s) - 1], f"clear U{bank} {s}")


if __name__ == "__main__":
    main()
