#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Kris (bk40-home)
"""The firmware's engine against Synth2's, case by case (tests/run_compare.sh builds the two renderers and runs this).

  python3 tools/compare/compare.py REF_RENDER VS_RENDER OUTDIR [case ..]

Each case is a patch (the INIT patch plus a few values) and some notes, written as a script both renderers play:
tools/compare/ref_render (Synth2's own C++ engine) and tests/vs_render (the firmware's C engine, on this computer).
What is compared, per case:
  level     the whole render's RMS, dB apart
  env       RMS in 256-frame windows, the worst dB apart where the reference is above -50 dBFS
  spectrum  1/3-octave bands 40 Hz .. 16 kHz, the mean and the worst dB apart where the reference's band is within
            60 dB of its loudest (above 16 kHz the two differ by design: Synth2's output low-pass is the EQ, not
            ported yet, and the FM-1's tables stop at 17.7 kHz)
  null      RMS of the difference under 8 kHz, dB under the reference: how far the two agree sample for sample.
            Reported, with a bound only on the cases whose phases are fixed (OSC PHASE above 0, or one voice)
A case fails outside its bounds; the script exits 1 if any does. Only what the engine has ported is in the cases:
they grow with the stages."""
import subprocess, sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sy1
ROOT = Path(__file__).resolve().parents[2]

FS = 44100
SEC = FS // 256 * 256                                    # a second, on the 256 grid


LEAD = 4096                                              # silence first: FREQ's glide from INIT's value is over (it snaps while nothing sounds)


def at(seconds):
    return LEAD + int(seconds * FS) // 16 * 16


def note(n, vel=127, on=0.0, off=1.0):
    return [("on", at(on), n, vel), ("off", at(off), n)]


# name: (patch values over INIT, events, seconds, bounds overriding DEFAULT)
DEFAULT = {"level": 0.3, "env": 1.0, "spec_mean": 0.5, "spec_max": 2.0, "null": None, "side": None}
P = {"OSC1_SHAPE": 0, "OSC2_SHAPE": 1, "OSC2_PITCH": 2, "OSC2_FINE": 3, "OSC2_TRACK": 4, "MIX": 5, "PW": 8, "KEY_SHIFT": 9,
     "FLT_TYPE": 14, "FENV_A": 15, "FENV_D": 16, "FENV_S": 17, "FENV_R": 18, "FREQ": 19, "RES": 20, "AMOUNT": 21, "KBD": 22,
     "SAT": 23, "VELSW": 24, "AENV_A": 25, "AENV_D": 26, "AENV_S": 27, "AENV_R": 28, "GAIN": 29, "VEL": 30, "BEND": 40,
     "TUNE": 72, "PHASE": 91, "POLY": 94, "SUB_GAIN": 95, "SUB_SHAPE": 96, "SUB_OCT": 97, "DETUNE": 76, "FM": 45, "SYNC": 6, "RING": 7,
     "MENV_ON": 10, "MENV_AMOUNT": 11, "MENV_A": 12, "MENV_D": 13, "MENV_DEST": 71,
     "L1_DEST": 41, "L1_WAVE": 42, "L1_SPEED": 43, "L1_DEPTH": 44, "L2_DEST": 46, "L2_WAVE": 47, "L2_SPEED": 48, "L2_DEPTH": 49,
     "L1_ON": 57, "L2_ON": 58, "L1_TEMPO": 67, "L1_KEY": 68, "L2_TEMPO": 69, "L2_KEY": 70, "PAN": 90, "MODE": 38, "PORTA": 39, "PORTA_AUTO": 74,
     "SENS1": 50, "SENS2": 51, "SRC1": 86, "DST1": 87, "SRC2": 88, "DST2": 89, "FX": 77, "FX_TYPE": 78, "FX_C1": 79, "FX_C2": 80, "FX_LEVEL": 81, "EQ_TONE": 60, "EQ_FREQ": 61, "EQ_LEVEL": 62,
     "EQ_Q": 63, "DLY": 65, "DLY_TIME": 35, "DLY_FBK": 36, "DLY_DW": 37, "DLY_TYPE": 82, "DLY_SPREAD": 83, "DLY_TONE": 98,
     "CHO": 66, "CHO_TYPE": 64, "CHO_TIME": 52, "CHO_DEPTH": 53, "CHO_RATE": 54, "CHO_FBK": 55, "CHO_LEVEL": 56, "ARP": 59, "ARP_TYPE": 31, "ARP_RANGE": 32, "ARP_BEAT": 33, "ARP_GATE": 34, "UNI": 73, "UNI_DETUNE": 75, "UNI_PAN": 84, "UNI_PITCH": 85, "UNI_PHASE": 92, "UNI_VOICES": 93}
OPEN = {"FREQ": 127, "RES": 0, "AMOUNT": 64, "KBD": 0}   # the filter out of the way
CASES = {}


def case(name, patch, events, seconds=1.5, **bounds):
    CASES[name] = (patch, events, seconds, {**DEFAULT, **bounds})


case("init", {}, note(60))
for sh, nm in enumerate(["sine", "saw", "pulse", "tri"]):
    for n in (36, 60, 84, 108):
        case(f"osc1-{nm}-{n}", {**OPEN, "OSC1_SHAPE": sh, "MIX": 0, "PHASE": 1}, note(n), null=-30.0 if n < 108 else -20.0)
for sh, nm in ((1, "saw"), (2, "pulse"), (3, "tri")):
    case(f"osc2-{nm}", {**OPEN, "OSC2_SHAPE": sh, "MIX": 127, "PHASE": 40}, note(60), null=-30.0)
case("osc2-noise", {**OPEN, "OSC2_SHAPE": 4, "MIX": 127}, note(60), spec_mean=1.0, spec_max=3.0)
case("osc2-pitch-fine", {**OPEN, "MIX": 127, "OSC2_PITCH": 79, "OSC2_FINE": 90, "PHASE": 1}, note(57), null=-30.0)
case("osc2-no-track", {**OPEN, "MIX": 127, "OSC2_TRACK": 0, "OSC2_PITCH": 70, "PHASE": 1}, note(72), null=-30.0)
case("mix-two-saws", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 64, "OSC2_FINE": 80, "PHASE": 30}, note(48), null=-30.0)
case("pw-narrow", {**OPEN, "OSC1_SHAPE": 2, "MIX": 0, "PW": 10, "PHASE": 1}, note(60), null=-30.0)
case("pw-zero", {**OPEN, "OSC1_SHAPE": 2, "MIX": 0, "PW": 0, "PHASE": 1}, note(60), null=-30.0)
case("key-shift-tune", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "KEY_SHIFT": 12, "TUNE": 100, "PHASE": 1}, note(50), null=-30.0)
for ft, nm in ((0, "lp12"), (2, "hp12"), (3, "bp12")):
    for freq, res in ((40, 0), (70, 64), (100, 100), (64, 125)):
        case(f"{nm}-f{freq}-r{res}", {"OSC1_SHAPE": 1, "MIX": 0, "FLT_TYPE": ft, "FREQ": freq, "RES": res, "AMOUNT": 64, "KBD": 0, "PHASE": 1}, note(48))
case("filter-env", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 30, "RES": 60, "AMOUNT": 110, "FENV_A": 45, "FENV_D": 75, "FENV_S": 30, "FENV_R": 60, "PHASE": 1},
     note(48, off=1.2), seconds=2.0, spec_mean=1.0, spec_max=3.0)
case("filter-env-down", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 100, "RES": 40, "AMOUNT": 20, "FENV_A": 30, "FENV_D": 70, "FENV_S": 0, "FENV_R": 40, "PHASE": 1},
     note(48), spec_mean=1.0, spec_max=3.0)
case("kbd-track", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 50, "RES": 80, "AMOUNT": 64, "KBD": 127, "PHASE": 1}, note(84))
case("vel-switch", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 40, "RES": 50, "AMOUNT": 120, "VELSW": 1, "FENV_D": 70, "FENV_S": 60, "PHASE": 1}, note(48, vel=60))
case("amp-env", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "AENV_A": 60, "AENV_D": 70, "AENV_S": 50, "AENV_R": 85, "PHASE": 1}, note(60, off=1.0), seconds=3.0)
case("amp-env-fast", {**OPEN, "OSC1_SHAPE": 0, "MIX": 0, "AENV_A": 0, "AENV_D": 30, "AENV_S": 0, "AENV_R": 0, "PHASE": 1}, note(60, off=0.5), seconds=1.0, env=2.0, spec_max=2.5)   # (bands 40 dB down: the fast decay's own)
case("gain-vel", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "GAIN": 100, "VEL": 127, "PHASE": 1}, note(60, vel=50))
case("saturation", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 90, "RES": 90, "SAT": 90, "AMOUNT": 64, "PHASE": 1}, note(48))
case("bend", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "BEND": 12, "PHASE": 1}, note(60) + [("bend", at(0.3), 16383), ("bend", at(0.6), 2000), ("bend", at(0.8), 8192)])
case("chord", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0}, sum((note(n, on=0.05 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64))), []), spec_mean=1.0)
case("steal", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "POLY": 2, "AENV_R": 80}, note(48, on=0.0, off=0.3) + note(55, on=0.2, off=0.9) + note(60, on=0.4, off=1.0) + note(67, on=0.6, off=1.0), spec_mean=1.0)
case("retrigger-pedal", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "AENV_R": 40}, note(60, on=0.0, off=0.2) + [("pedal", at(0.1), 1)] + note(60, on=0.4, off=0.5) + [("pedal", at(0.9), 0)], spec_mean=1.0)

# 4b: the sub
for sh, nm in enumerate(["sine", "tri", "saw", "square"]):
    for octv in (0, 1):
        case(f"sub-{nm}-{'oct' if octv else 'unison'}", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "SUB_GAIN": 64, "SUB_SHAPE": sh, "SUB_OCT": octv, "PHASE": 1}, note(48), null=-30.0)
case("sub-loud-mix", {**OPEN, "OSC1_SHAPE": 2, "OSC2_SHAPE": 1, "MIX": 80, "SUB_GAIN": 127, "SUB_SHAPE": 3, "SUB_OCT": 1, "OSC2_FINE": 70, "PHASE": 20}, note(40), null=-30.0)
case("sub-free-phase", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "SUB_GAIN": 40, "SUB_SHAPE": 0, "SUB_OCT": 1}, note(48, off=0.4) + note(55, on=0.5, off=1.0) + note(48, on=0.7, off=1.2), seconds=1.6)
case("sub-high", {**OPEN, "OSC1_SHAPE": 3, "MIX": 0, "SUB_GAIN": 90, "SUB_SHAPE": 2, "SUB_OCT": 1, "PHASE": 1}, note(100), null=-25.0)
case("sub-filtered-chord", {"OSC1_SHAPE": 1, "MIX": 30, "SUB_GAIN": 70, "SUB_SHAPE": 3, "SUB_OCT": 1, "FREQ": 60, "RES": 70, "AMOUNT": 64, "KBD": 0},
     sum((note(n, on=0.03 * i, off=1.0) for i, n in enumerate((36, 43, 48, 52))), []), spec_mean=1.0)

# 4b: detune (the copies' truncated reads alias a little differently from Synth2's per-note copy tables: the nulls
# are bounded looser than the plain oscillators')
for sh, nm in enumerate(["sine", "saw", "pulse", "tri"]):
    for d in (20, 127):
        # (the triangle at note 48, the bottom of a 24-note table: its table stops at 6.8 kHz, and the bands above are
        # 56..60 dB under the fundamental where Synth2 still has harmonics: tools/gen_wave.py, the 6-table choice)
        case(f"detune-{nm}-{d}", {**OPEN, "OSC1_SHAPE": sh, "MIX": 0, "DETUNE": d, "PHASE": 1}, note(48), null=-25.0,
             **({"spec_max": 12.0, "spec_mean": 1.0} if nm == "tri" else {}))
case("detune-sub-square", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "DETUNE": 60, "SUB_GAIN": 80, "SUB_SHAPE": 3, "SUB_OCT": 1, "PHASE": 1}, note(45), null=-25.0)
case("detune-sub-tri", {**OPEN, "OSC1_SHAPE": 2, "MIX": 0, "PW": 40, "DETUNE": 90, "SUB_GAIN": 50, "SUB_SHAPE": 1, "SUB_OCT": 0, "PHASE": 60}, note(52), null=-25.0)
case("detune-free-phase", {**OPEN, "OSC1_SHAPE": 1, "MIX": 40, "DETUNE": 70, "SUB_GAIN": 30, "SUB_OCT": 1},
     note(48, off=0.4) + note(48, on=0.6, off=1.2), seconds=1.6)
case("detune-high", {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "DETUNE": 100, "PHASE": 1}, note(96), null=-20.0)
case("detune-chord", {"OSC1_SHAPE": 1, "MIX": 0, "DETUNE": 64, "FREQ": 80, "RES": 30, "AMOUNT": 64, "KBD": 0},
     sum((note(n, on=0.02 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64, 67))), []), spec_mean=1.0)

# 4b: FM, RING, SYNC (the cross path). FM integrates OSC2 into OSC1's phase every frame, so an error in OSC2 that
# does not average out walks OSC1's phase away. Up to FM 90 the two engines null to 50..63 dB. At FM 127 from a saw
# or a pulse they part (null -22 dB, a few dB in some bands): the deviation follows OSC2's jumps, which our tables
# (one per 6 semitones, read linearly) shape differently from Synth2's (one a note, a cubic). Not chaos: Synth2 with
# its FM constant moved by 1e-6 stays within -85 dB of itself; from a triangle (no jump) FM 127 nulls to -54 dB
for fm in (10, 40, 90, 127):
    for sh2, nm in ((1, "saw"), (2, "pulse"), (3, "tri")):
        jump = fm == 127 and sh2 != 3
        case(f"fm{fm}-{nm}", {**OPEN, "OSC1_SHAPE": 0, "OSC2_SHAPE": sh2, "MIX": 0, "FM": fm, "OSC2_PITCH": 76, "PHASE": 1}, note(60),
             null=None if jump else -45.0, spec_mean=1.5 if jump else 0.5, spec_max=6.0 if jump else 2.0)
case("fm-noise", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 4, "MIX": 0, "FM": 30}, note(60), spec_mean=1.0, spec_max=3.0)
case("fm-saw-mix", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 50, "FM": 25, "OSC2_PITCH": 64, "OSC2_FINE": 70, "PHASE": 1}, note(48), null=-45.0)
case("fm-low-note", {**OPEN, "OSC1_SHAPE": 2, "OSC2_SHAPE": 3, "MIX": 0, "FM": 60, "PHASE": 1}, note(30), null=-35.0)
case("fm-detune-sub", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 0, "FM": 20, "DETUNE": 50, "SUB_GAIN": 60, "SUB_SHAPE": 3, "SUB_OCT": 1, "PHASE": 1},
     note(48), null=-35.0)
for sh2, nm in ((1, "saw"), (2, "pulse"), (3, "tri"), (4, "noise")):
    case(f"ring-{nm}", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": sh2, "MIX": 127, "RING": 1, "OSC2_PITCH": 71, "PHASE": 1}, note(57),
         null=-30.0 if sh2 != 4 else None)
    case(f"ring-fm-{nm}", {**OPEN, "OSC1_SHAPE": 0, "OSC2_SHAPE": sh2, "MIX": 100, "RING": 1, "FM": 50, "OSC2_PITCH": 71, "PHASE": 1}, note(57),
         null=-30.0 if sh2 != 4 else None)
# (every oscillator and copy starts at phase 0, on the saws' jump: the note's first ~0.3 ms follows how the tables
# shape the jump, and differs; from then on it nulls to -53 dB. The whole-render null is bounded off for that)
case("ring-detune-sub", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 90, "RING": 1, "DETUNE": 40, "SUB_GAIN": 50, "SUB_SHAPE": 2, "OSC2_PITCH": 80, "PHASE": 1},
     note(50))
for pitch in (70, 88, 110):
    case(f"sync-saw-{pitch}", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 127, "SYNC": 1, "OSC2_PITCH": pitch, "PHASE": 1}, note(48), null=-25.0)
case("sync-pulse", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 2, "MIX": 127, "SYNC": 1, "OSC2_PITCH": 95, "PW": 30, "PHASE": 1}, note(45), null=-25.0)
case("sync-noise", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 4, "MIX": 127, "SYNC": 1, "PHASE": 1}, note(55), null=-25.0)
case("sync-fm", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 100, "SYNC": 1, "FM": 35, "OSC2_PITCH": 90, "PHASE": 1}, note(48), spec_mean=1.0, spec_max=3.0)
case("sync-fm-deep", {**OPEN, "OSC1_SHAPE": 1, "OSC2_SHAPE": 3, "MIX": 100, "SYNC": 1, "FM": 100, "OSC2_PITCH": 70, "PHASE": 1}, note(60), spec_mean=1.5, spec_max=4.0)
case("cross-chord", {"OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 70, "SYNC": 1, "FM": 15, "OSC2_PITCH": 84, "FREQ": 90, "RES": 40, "AMOUNT": 64, "KBD": 0},
     sum((note(n, on=0.02 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64))), []), spec_mean=1.0, spec_max=3.0)
# 4b: the mod env (AD, never released). OSC2 pitch (+-60 semitones x level), FM (the knob moved toward 127 or 0,
# a log law after the attack), P/W (the duty moved by the room left). Each in both directions, fast and slow
# (OSC2 keeps its note's table as Synth2 does, so a pitch held far down plays the table's top harmonics low. Ours,
# one a 6 semitones, stops below Synth2's one a note: held 5 octaves down that shows above 10 kHz, -3 dB at 10 kHz,
# -14 dB at 16 kHz where the band is already 49 dB under the loudest. Only the slow, downward cases hold it there)
ME = {**OPEN, "MENV_ON": 1}
for amt in (127, 90, 30, 0):
    for a, d, sp in ((0, 40, "fast"), (60, 90, "slow")):
        down = amt < 64 and sp == "slow"
        case(f"menv-pitch-{amt}-{sp}", {**ME, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 127, "MENV_DEST": 0, "MENV_AMOUNT": amt,
             "MENV_A": a, "MENV_D": d, "PHASE": 1}, note(48), null=-30.0, spec_mean=1.5 if down else 0.5, spec_max=25.0 if down else 2.0)
        case(f"menv-fm-{amt}-{sp}", {**ME, "OSC1_SHAPE": 0, "OSC2_SHAPE": 3, "MIX": 0, "FM": 40, "MENV_DEST": 1, "MENV_AMOUNT": amt,
             "MENV_A": a, "MENV_D": d, "OSC2_PITCH": 76, "PHASE": 1}, note(60), null=-30.0)
        case(f"menv-pw-{amt}-{sp}", {**ME, "OSC1_SHAPE": 2, "MIX": 0, "PW": 70, "MENV_DEST": 2, "MENV_AMOUNT": amt,
             "MENV_A": a, "MENV_D": d, "PHASE": 1}, note(48), null=-30.0)
case("menv-sync-sweep", {**ME, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 127, "SYNC": 1, "OSC2_PITCH": 70, "MENV_DEST": 0, "MENV_AMOUNT": 110,
     "MENV_A": 0, "MENV_D": 80, "PHASE": 1}, note(45), null=-25.0)
case("menv-fm-ring67", {**ME, "OSC1_SHAPE": 1, "OSC2_SHAPE": 2, "MIX": 110, "RING": 1, "FM": 0, "MENV_DEST": 1, "MENV_AMOUNT": 67,
     "MENV_A": 10, "MENV_D": 70, "OSC2_PITCH": 71, "PHASE": 1}, note(57), null=-30.0)
case("menv-retrigger", {**ME, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 127, "MENV_DEST": 0, "MENV_AMOUNT": 100, "MENV_A": 30, "MENV_D": 60, "PHASE": 1},
     note(48, off=0.3) + note(48, on=0.4, off=1.0), spec_mean=1.0, spec_max=3.0)
# 4b: the LFOs (global, free running from power-on in both engines). Pitch in whole 1/128-octave steps of their own,
# by wave; FILTER toward 125, AMP a gain, P/W and FM the room-left laws, PAN after the voices (the side is checked)
L = {**OPEN, "L1_ON": 1, "L2_ON": 1}
WAVES = ["saw", "tri", "square", "sh", "smooth", "sine"]
for w, nm in enumerate(WAVES):
    case(f"lfo-pitch12-{nm}", {**L, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 50, "L1_DEST": 2, "L1_WAVE": w, "L1_SPEED": 75, "L1_DEPTH": 60,
         "PHASE": 1}, note(48), null=-30.0)
    case(f"lfo-cut-{nm}", {**L, "OSC1_SHAPE": 1, "MIX": 0, "FREQ": 50, "RES": 60, "L1_DEST": 3, "L1_WAVE": w, "L1_SPEED": 80, "L1_DEPTH": 90,
         "PHASE": 1}, note(48), null=-30.0, spec_max=3.0 if w == 0 else 2.0)   # (the saw's jump: our cutoff ramps over 16 frames)
    case(f"lfo-amp-{nm}", {**L, "OSC1_SHAPE": 1, "MIX": 0, "L2_DEST": 4, "L2_WAVE": w, "L2_SPEED": 85, "L2_DEPTH": 100, "PHASE": 1},
         note(48), null=-30.0)
    case(f"lfo-pan-{nm}", {**L, "OSC1_SHAPE": 3, "MIX": 0, "L1_DEST": 7, "L1_WAVE": w, "L1_SPEED": 80, "L1_DEPTH": 110, "PHASE": 1},
         note(55), null=-30.0, side=-30.0)
for w in (1, 2, 5):
    case(f"lfo-pitch2-{WAVES[w]}", {**L, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 100, "OSC2_PITCH": 71, "L2_DEST": 1, "L2_WAVE": w,
         "L2_SPEED": 60, "L2_DEPTH": 90, "PHASE": 1}, note(48), null=-30.0)
    case(f"lfo-pw-{WAVES[w]}", {**L, "OSC1_SHAPE": 2, "MIX": 0, "PW": 50, "L2_DEST": 5, "L2_WAVE": w, "L2_SPEED": 70, "L2_DEPTH": 120,
         "PHASE": 1}, note(48), null=-30.0)
    case(f"lfo-fm-{WAVES[w]}", {**L, "OSC1_SHAPE": 0, "OSC2_SHAPE": 3, "MIX": 0, "FM": 20, "OSC2_PITCH": 76, "L1_DEST": 6, "L1_WAVE": w,
         "L1_SPEED": 65, "L1_DEPTH": 90, "PHASE": 1}, note(60), null=-30.0)
# (DEPTH 127 swings OSC2 +-5 octaves on its note's table: 5 octaves up, the tables' aliasing differs (ours one a 6
# semitones, read linearly; Synth2's one a note, cubic), and the null falls to -15 dB from DEPTH 100's -30. Bounded on
# the spectrum alone)
case("lfo-pitch2-deep", {**L, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 127, "L2_DEST": 1, "L2_WAVE": 5, "L2_SPEED": 60, "L2_DEPTH": 127,
     "PHASE": 1}, note(48), env=2.0)
case("lfo-pitch-fast", {**L, "OSC1_SHAPE": 1, "MIX": 0, "L1_DEST": 2, "L1_WAVE": 5, "L1_SPEED": 127, "L1_DEPTH": 40, "PHASE": 1}, note(48),
     null=-25.0)
case("lfo-pitch-detune-sub", {**L, "OSC1_SHAPE": 1, "MIX": 0, "DETUNE": 50, "SUB_GAIN": 60, "SUB_SHAPE": 3, "SUB_OCT": 1, "L1_DEST": 2,
     "L1_WAVE": 1, "L1_SPEED": 70, "L1_DEPTH": 50, "PHASE": 1}, note(45), null=-25.0)
case("lfo-both-pitch", {**L, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 60, "L1_DEST": 2, "L1_WAVE": 5, "L1_SPEED": 60, "L1_DEPTH": 50,
     "L2_DEST": 1, "L2_WAVE": 0, "L2_SPEED": 75, "L2_DEPTH": 70, "PHASE": 1}, note(50), null=-30.0)
case("lfo-both-pw", {**L, "OSC1_SHAPE": 2, "MIX": 0, "PW": 90, "L1_DEST": 5, "L1_WAVE": 5, "L1_SPEED": 60, "L1_DEPTH": 127,
     "L2_DEST": 5, "L2_WAVE": 2, "L2_SPEED": 80, "L2_DEPTH": 127, "PHASE": 1}, note(48), null=-30.0)
case("lfo-both-amp", {**L, "OSC1_SHAPE": 1, "MIX": 0, "L1_DEST": 4, "L1_WAVE": 1, "L1_SPEED": 70, "L1_DEPTH": 127,
     "L2_DEST": 4, "L2_WAVE": 5, "L2_SPEED": 90, "L2_DEPTH": 80, "PHASE": 1}, note(48), null=-30.0)
case("lfo-both-pan", {**L, "OSC1_SHAPE": 3, "MIX": 0, "PAN": 30, "L1_DEST": 7, "L1_WAVE": 5, "L1_SPEED": 70, "L1_DEPTH": 127,
     "L2_DEST": 7, "L2_WAVE": 1, "L2_SPEED": 85, "L2_DEPTH": 127, "PHASE": 1}, note(55), null=-30.0, side=-30.0)
case("pan-patch", {**OPEN, "OSC1_SHAPE": 3, "MIX": 0, "PAN": 100, "PHASE": 1}, note(55), null=-30.0, side=-30.0)
case("lfo-ring-fm", {**L, "OSC1_SHAPE": 1, "OSC2_SHAPE": 2, "MIX": 110, "RING": 1, "OSC2_PITCH": 71, "L1_DEST": 6, "L1_WAVE": 2,
     "L1_SPEED": 40, "L1_DEPTH": 30, "PHASE": 1}, note(57), null=-30.0)
case("lfo-menv-pitch", {**L, "MENV_ON": 1, "MENV_DEST": 0, "MENV_AMOUNT": 100, "MENV_A": 0, "MENV_D": 60, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1,
     "MIX": 127, "L1_DEST": 1, "L1_WAVE": 5, "L1_SPEED": 70, "L1_DEPTH": 60, "PHASE": 1}, note(48), null=-30.0)
case("lfo-menv-pw", {**L, "MENV_ON": 1, "MENV_DEST": 2, "MENV_AMOUNT": 30, "MENV_A": 20, "MENV_D": 70, "OSC1_SHAPE": 2, "MIX": 0, "PW": 60,
     "L1_DEST": 5, "L1_WAVE": 1, "L1_SPEED": 75, "L1_DEPTH": 100, "PHASE": 1}, note(48), null=-30.0)
case("lfo-menv-fm", {**L, "MENV_ON": 1, "MENV_DEST": 1, "MENV_AMOUNT": 100, "MENV_A": 5, "MENV_D": 70, "OSC1_SHAPE": 0, "OSC2_SHAPE": 3,
     "MIX": 0, "FM": 10, "OSC2_PITCH": 76, "L1_DEST": 6, "L1_WAVE": 5, "L1_SPEED": 70, "L1_DEPTH": 60, "PHASE": 1}, note(60), null=-30.0)
case("lfo-tempo", {**L, "OSC1_SHAPE": 1, "MIX": 0, "FREQ": 60, "L1_DEST": 3, "L1_WAVE": 2, "L1_TEMPO": 1, "L1_SPEED": 64, "L1_DEPTH": 90,
     "L2_DEST": 4, "L2_WAVE": 0, "L2_TEMPO": 1, "L2_SPEED": 100, "L2_DEPTH": 90, "PHASE": 1}, note(48), null=-30.0)
case("lfo-keysync", {**L, "OSC1_SHAPE": 1, "MIX": 0, "FREQ": 60, "L1_DEST": 3, "L1_WAVE": 3, "L1_KEY": 1, "L1_SPEED": 70, "L1_DEPTH": 100,
     "L2_DEST": 2, "L2_WAVE": 4, "L2_KEY": 1, "L2_SPEED": 60, "L2_DEPTH": 40, "PHASE": 1},
     note(48, off=0.4) + note(52, on=0.5, off=0.8) + note(55, on=0.7, off=1.2), seconds=1.6, null=-25.0)
case("lfo-chord", {"OSC1_SHAPE": 1, "OSC2_SHAPE": 2, "MIX": 50, "FREQ": 70, "RES": 50, "AMOUNT": 64, "KBD": 0, "L1_ON": 1, "L2_ON": 1,
     "L1_DEST": 2, "L1_WAVE": 5, "L1_SPEED": 72, "L1_DEPTH": 20, "L2_DEST": 3, "L2_WAVE": 1, "L2_SPEED": 50, "L2_DEPTH": 60},
     sum((note(n, on=0.02 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64))), []), spec_mean=1.0, spec_max=3.0)
# 4b: MONO (1) and LEGATO (2): one voice plays the held-key stack. A key on while one sounds moves it (MONO
# retriggers the envelopes and the oscillators' phases, at the new velocity; LEGATO moves the pitch only); a key off
# falls back to the top held key (MONO retriggers, the velocity kept), or releases when none is left
TRILL = (note(48, on=0.0, off=1.2) + note(55, vel=80, on=0.3, off=0.6) + note(60, vel=110, on=0.45, off=0.9)
         + note(52, vel=60, on=1.0, off=1.4))
for mode, mn in ((1, "mono"), (2, "legato")):
    case(f"{mn}-trill", {**OPEN, "MODE": mode, "OSC1_SHAPE": 1, "MIX": 0, "AENV_A": 30, "AENV_D": 60, "AENV_S": 70, "AENV_R": 50,
         "VEL": 100, "PHASE": 1}, TRILL, seconds=1.8, null=-30.0)
    case(f"{mn}-filter-menv", {"MODE": mode, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 60, "FREQ": 40, "RES": 50, "AMOUNT": 100, "KBD": 0,
         "FENV_A": 20, "FENV_D": 60, "FENV_S": 30, "MENV_ON": 1, "MENV_DEST": 0, "MENV_AMOUNT": 90, "MENV_A": 0, "MENV_D": 50,
         "PHASE": 30}, TRILL, seconds=1.8, null=-30.0)
    case(f"{mn}-free-phase", {**OPEN, "MODE": mode, "OSC1_SHAPE": 2, "MIX": 30, "DETUNE": 40, "SUB_GAIN": 50, "SUB_OCT": 1}, TRILL,
         seconds=1.8, spec_mean=1.0, spec_max=3.0)
    case(f"{mn}-staccato", {**OPEN, "MODE": mode, "OSC1_SHAPE": 1, "MIX": 0, "AENV_R": 60, "PHASE": 1},
         note(48, off=0.2) + note(55, on=0.3, off=0.5) + note(60, on=0.55, off=0.8), seconds=1.4, null=-30.0)
    case(f"{mn}-pedal", {**OPEN, "MODE": mode, "OSC1_SHAPE": 3, "MIX": 0, "AENV_R": 40, "PHASE": 1},
         [("pedal", at(0.05), 1)] + note(48, on=0.1, off=0.3) + note(55, on=0.4, off=0.5) + [("pedal", at(0.8), 0)]
         + note(60, on=1.0, off=1.2), seconds=1.6, null=-30.0)
# 4b: PORTAMENTO: a 16-frame exponential step aimed 1.35 % past the note, played in whole 1/128 octaves; a fresh
# note glides from its voice's own last pitch (a never-used voice from note 0; AUTO: never), a mono change always
for t in (20, 60, 100):
    case(f"porta-mono-{t}", {**OPEN, "MODE": 1, "PORTA": t, "OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 40, "OSC2_PITCH": 71, "PHASE": 1}, TRILL,
         seconds=1.8, null=-30.0)
case("porta-legato-auto", {**OPEN, "MODE": 2, "PORTA": 60, "PORTA_AUTO": 1, "OSC1_SHAPE": 1, "MIX": 0, "PHASE": 1},
     note(48, off=0.3) + note(60, on=0.4, off=0.9) + note(55, on=0.6, off=1.2), seconds=1.6, null=-30.0)
case("porta-mono-auto-staccato", {**OPEN, "MODE": 1, "PORTA": 60, "PORTA_AUTO": 1, "OSC1_SHAPE": 1, "MIX": 0, "AENV_R": 30, "PHASE": 1},
     note(48, off=0.3) + note(60, on=0.4, off=0.7), seconds=1.2, null=-30.0)
case("porta-poly", {**OPEN, "PORTA": 50, "OSC1_SHAPE": 1, "MIX": 0, "AENV_R": 30, "PHASE": 1},
     note(48, off=0.3) + note(60, on=0.4, off=0.7) + note(72, on=0.8, off=1.1), seconds=1.5, null=-30.0)
case("porta-poly-chord", {**OPEN, "PORTA": 40, "POLY": 2, "OSC1_SHAPE": 2, "MIX": 0, "AENV_R": 20, "PHASE": 1},
     note(48, off=0.4) + note(52, off=0.4) + note(55, on=0.5, off=0.9) + note(59, on=0.5, off=0.9), seconds=1.3, null=-30.0)
case("porta-notrack", {**OPEN, "MODE": 1, "PORTA": 50, "OSC1_SHAPE": 1, "OSC2_SHAPE": 2, "MIX": 60, "OSC2_TRACK": 0, "PHASE": 1}, TRILL,
     seconds=1.8, null=-30.0)
# 4b: UNISON: N voices over +-DETUNE cents, SPREAD pans them, odd places at PITCH - 24; a place is its slot's
# (slot % N); PHASE shifts the k-th voice by its slot's start phases. The side checks the pans
U = {**OPEN, "UNI": 1, "OSC1_SHAPE": 1, "MIX": 0, "PHASE": 1}
for nv, det, pan in ((2, 30, 64), (4, 60, 0), (8, 90, 127), (3, 20, 30)):
    case(f"uni-{nv}-{det}-{pan}", {**U, "UNI_VOICES": nv, "UNI_DETUNE": det, "UNI_PAN": pan, "POLY": 8}, note(48), null=-30.0,
         side=-30.0 if pan != 64 else None)
case("uni-pitch", {**U, "UNI_VOICES": 4, "UNI_DETUNE": 10, "UNI_PITCH": 31, "UNI_PAN": 20, "POLY": 8}, note(48), null=-30.0, side=-30.0)
case("uni-phase", {**U, "UNI_VOICES": 4, "UNI_DETUNE": 40, "UNI_PHASE": 90, "OSC1_SHAPE": 2, "OSC2_SHAPE": 1, "MIX": 50,
     "DETUNE": 30, "SUB_GAIN": 40, "SUB_OCT": 1, "POLY": 8}, note(48), null=-30.0)
case("uni-chord-steal", {**U, "UNI_VOICES": 3, "UNI_DETUNE": 40, "UNI_PAN": 100, "POLY": 8, "AENV_R": 40},
     note(48, off=0.8) + note(55, on=0.1, off=0.8) + note(60, on=0.2, off=0.8), seconds=1.4, null=-30.0, side=-30.0)
case("uni-mono-porta", {**U, "MODE": 1, "PORTA": 40, "UNI_VOICES": 4, "UNI_DETUNE": 50, "UNI_PAN": 10, "POLY": 8}, TRILL, seconds=1.8,
     null=-30.0, side=-30.0)
case("uni-legato-lfo-pan", {**U, "MODE": 2, "UNI_VOICES": 2, "UNI_DETUNE": 30, "UNI_PAN": 0, "POLY": 8, "PAN": 90, "L1_ON": 1,
     "L1_DEST": 7, "L1_WAVE": 5, "L1_SPEED": 70, "L1_DEPTH": 80}, TRILL, seconds=1.8, null=-30.0, side=-30.0)
case("uni-free-phase", {**U, "UNI_VOICES": 4, "UNI_DETUNE": 50, "PHASE": 0, "POLY": 8}, note(48, off=0.5) + note(52, on=0.6, off=1.1),
     seconds=1.4, spec_mean=1.0, spec_max=3.0)
# 4b: the CTRL routes: SRC (here the bend, 0xE000: bipolar) adds round(v d 128 / (63 x 127)) knob units to ASSIGN,
# d = SENS - 64 above 64, SENS - 63 below 63; ASSIGN must range 0..127
BENDS = [("bend", at(0.2 + 0.1 * k), v) for k, v in enumerate((10000, 14000, 16383, 5000, 0, 8192))]
case("ctrl-bend-cutoff", {"OSC1_SHAPE": 1, "MIX": 0, "FREQ": 60, "RES": 40, "AMOUNT": 64, "KBD": 0, "BEND": 0, "SRC1": 0xE000, "DST1": 19,
     "SENS1": 110, "SRC2": 0xE000, "DST2": 29, "SENS2": 20, "PHASE": 1}, note(48) + BENDS, null=-30.0)
case("ctrl-bend-fm-lfo", {**OPEN, "OSC1_SHAPE": 0, "OSC2_SHAPE": 3, "MIX": 0, "FM": 30, "OSC2_PITCH": 76, "BEND": 0, "SRC1": 0xE000,
     "DST1": 45, "SENS1": 127, "SRC2": 0xE000, "DST2": 44, "SENS2": 0, "L1_ON": 1, "L1_DEST": 3, "L1_DEPTH": 50, "PHASE": 1},
     note(60) + BENDS, null=-30.0)
case("ctrl-off-and-span", {**OPEN, "OSC1_SHAPE": 1, "MIX": 50, "OSC2_SHAPE": 1, "BEND": 2, "SRC1": 0xE000, "DST1": 9, "SENS1": 127,
     "SRC2": 0xE000, "DST2": 5, "SENS2": 63, "PHASE": 1}, note(48) + BENDS, null=-30.0)
# 4c: LP24: four bilinear one-poles, the feedback (k = (0.0297 res - 0.076) x comp) a frame late through an AGC,
# then the output limiter. Res 127 self-oscillates, bounded by the AGC and the limiter
for freq, res in ((40, 0), (70, 64), (100, 100), (64, 120), (80, 127), (20, 127), (120, 90)):
    case(f"lp24-f{freq}-r{res}", {"OSC1_SHAPE": 1, "MIX": 0, "FLT_TYPE": 1, "FREQ": freq, "RES": res, "AMOUNT": 64, "KBD": 0, "PHASE": 1},
         note(48), null=-30.0 if res < 120 else None, env=1.0 if res < 127 else 2.0)   # (self-oscillation's build-up: its own timing)
case("lp24-env-sat", {"OSC1_SHAPE": 2, "OSC2_SHAPE": 1, "MIX": 50, "FLT_TYPE": 1, "FREQ": 30, "RES": 100, "AMOUNT": 110, "SAT": 80,
     "FENV_A": 20, "FENV_D": 70, "FENV_S": 20, "PHASE": 1}, note(45, off=1.0), null=-30.0)
case("lp24-chord-lfo", {"OSC1_SHAPE": 1, "MIX": 0, "FLT_TYPE": 1, "FREQ": 60, "RES": 110, "AMOUNT": 64, "KBD": 64, "L1_ON": 1, "L1_DEST": 3,
     "L1_WAVE": 1, "L1_SPEED": 60, "L1_DEPTH": 60}, sum((note(n, on=0.02 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64))), []),
     spec_mean=1.0, spec_max=3.0)
# 4c: LPDL: the fitted diode ladder (2 x oversampled trapezoid, its feedback high-passed), the input and the feedback
# through tanh; its 5 x 5 step solved by elimination in Q30 when the cutoff moves 1/8 knob (Synth2: inverted in double)
for freq, res in ((40, 0), (70, 64), (100, 100), (64, 120), (90, 127), (25, 110), (127, 60)):
    case(f"lpdl-f{freq}-r{res}", {"OSC1_SHAPE": 1, "MIX": 0, "FLT_TYPE": 4, "FREQ": freq, "RES": res, "AMOUNT": 64, "KBD": 0, "PHASE": 1},
         note(48), null=-30.0 if res < 127 else -20.0)   # (at 127 the ladder rings at its edge: -24 dB)
case("lpdl-env-acid", {"OSC1_SHAPE": 2, "MIX": 0, "FLT_TYPE": 4, "FREQ": 30, "RES": 115, "AMOUNT": 120, "FENV_A": 0, "FENV_D": 55, "FENV_S": 0,
     "SAT": 60, "MODE": 1, "PORTA": 30, "PHASE": 1}, TRILL, seconds=1.8, null=-20.0)   # (the env's sweep: our coefficients follow it a block at a time)
case("lpdl-loud-chord", {"OSC1_SHAPE": 1, "OSC2_SHAPE": 1, "MIX": 60, "FLT_TYPE": 4, "FREQ": 70, "RES": 90, "AMOUNT": 64, "KBD": 0, "GAIN": 110},
     sum((note(n, on=0.02 * i, off=1.0) for i, n in enumerate((48, 55, 60, 64))), []), spec_mean=1.0, spec_max=3.0)
# 4d: the bus. EFFECT (10 types), EQ's peak, DELAY (ST / X / PP, the tempo's 120 bpm), CHORUS (4 types; with the
# delay on, the echoes through it). Each on a plain saw, then mixes
B = {**OPEN, "OSC1_SHAPE": 1, "MIX": 0, "PHASE": 1, "AENV_R": 30}
for ty, c1, c2, lev in ((0, 40, 64, 100), (0, 100, 100, 64), (1, 30, 80, 100), (1, 110, 40, 127), (2, 20, 90, 100), (2, 90, 60, 80),
                        (3, 6, 60, 127), (3, 0, 30, 90), (4, 70, 0, 100), (4, 20, 0, 64), (5, 60, 40, 100), (5, 127, 100, 127),
                        (6, 60, 64, 90), (7, 100, 80, 30), (8, 40, 50, 110), (9, 127, 30, 64)):
    hot = ty == 2 and c1 >= 64                  # (d.d. folds at x 165 by CTRL1 90: it magnifies the oscillators' own small
    case(f"fx{ty}-{c1}-{c2}-{lev}", {**B, "FX": 1, "FX_TYPE": ty, "FX_C1": c1, "FX_C2": c2, "FX_LEVEL": lev}, note(48, off=0.8), seconds=1.2,
         null=None if hot else -30.0, spec_mean=1.0 if hot else 0.5, spec_max=4.0 if hot else 2.0)   # differences, the tables')
for f, lv, q in ((40, 100, 64), (80, 20, 100), (110, 127, 20), (20, 0, 64), (64, 90, 127)):
    case(f"eq-{f}-{lv}-{q}", {**B, "EQ_FREQ": f, "EQ_LEVEL": lv, "EQ_Q": q}, note(48, off=0.8), seconds=1.2, null=-30.0)
for ty, t, fb, dw, sp, tn in ((0, 8, 60, 64, 64, 64), (1, 5, 90, 90, 64, 30), (2, 12, 70, 50, 64, 100), (0, 3, 40, 64, 20, 64),
                              (1, 7, 100, 64, 110, 64), (0, 0, 80, 127, 64, 64)):
    case(f"dly{ty}-{t}-{fb}-{dw}-{sp}-{tn}", {**B, "DLY": 1, "DLY_TYPE": ty, "DLY_TIME": t, "DLY_FBK": fb, "DLY_DW": dw, "DLY_SPREAD": sp,
         "DLY_TONE": tn}, note(48, off=0.15) + note(55, on=0.4, off=0.5), seconds=2.0, null=-30.0, side=-30.0 if ty == 2 or sp != 64 else None)
for ty, t, d, rt, fb, lev in ((1, 60, 60, 64, 64, 100), (2, 90, 100, 40, 90, 127), (3, 30, 30, 90, 30, 64), (4, 80, 80, 60, 64, 110),
                              (1, 10, 127, 100, 120, 80)):
    case(f"cho{ty}-{t}-{d}-{rt}-{fb}-{lev}", {**B, "CHO": 1, "CHO_TYPE": ty, "CHO_TIME": t, "CHO_DEPTH": d, "CHO_RATE": rt, "CHO_FBK": fb,
         "CHO_LEVEL": lev}, note(48, off=0.8), seconds=1.2, null=-30.0, side=-30.0 if ty != 1 else None)
case("dly-cho", {**B, "DLY": 1, "DLY_TYPE": 2, "DLY_TIME": 8, "DLY_FBK": 70, "DLY_DW": 64, "CHO": 1, "CHO_TYPE": 4, "CHO_TIME": 60, "CHO_DEPTH": 60,
     "CHO_RATE": 64, "CHO_LEVEL": 0}, note(48, off=0.15) + note(55, on=0.4, off=0.5), seconds=2.0, null=-30.0, side=-30.0)
case("bus-all", {"OSC1_SHAPE": 2, "OSC2_SHAPE": 1, "MIX": 50, "FREQ": 70, "RES": 60, "AMOUNT": 64, "KBD": 0, "FX": 1, "FX_TYPE": 1, "FX_C1": 50,
     "FX_C2": 90, "FX_LEVEL": 100, "PAN": 40, "EQ_FREQ": 70, "EQ_LEVEL": 90, "EQ_Q": 64, "EQ_TONE": 50, "DLY": 1, "DLY_TYPE": 1, "DLY_TIME": 9,
     "DLY_FBK": 60, "DLY_DW": 70, "DLY_SPREAD": 90, "CHO": 1, "CHO_TYPE": 2, "CHO_TIME": 50, "CHO_DEPTH": 70, "CHO_RATE": 60, "CHO_LEVEL": 90},
     sum((note(n, on=0.05 * i, off=0.6) for i, n in enumerate((48, 55, 60))), []), seconds=2.0, spec_mean=1.0, spec_max=3.0, side=None)
# 4d: the arp: steps of the tempo's note value (120 bpm), its gate, types 1..4 over RANGE + 1 octaves. Its steps land on
# our 16-frame grid (Synth2: to the frame), so the null is not bounded; the envelope and the spectrum are
CHORD3 = sum((note(n, on=0.01 * i, off=1.5) for i, n in enumerate((48, 52, 55))), [])
for ty, rg, bt, gt in ((1, 0, 14, 100), (2, 1, 11, 64), (3, 2, 16, 127), (4, 1, 14, 90), (1, 3, 18, 40)):
    case(f"arp{ty}-{rg}-{bt}-{gt}", {**B, "ARP": 1, "ARP_TYPE": ty, "ARP_RANGE": rg, "ARP_BEAT": bt, "ARP_GATE": gt, "AENV_A": 5, "AENV_R": 20},
         CHORD3, seconds=2.0, env=3.0 if bt < 18 else 4.0, spec_mean=1.0, spec_max=3.0)   # (1/24 beat: 289-frame notes, 16 frames late or not)
case("arp-mono-porta", {**B, "ARP": 1, "ARP_TYPE": 2, "ARP_RANGE": 1, "ARP_BEAT": 14, "ARP_GATE": 127, "MODE": 1, "PORTA": 30},
     CHORD3, seconds=2.0, env=3.0, spec_mean=1.0, spec_max=3.0)
case("noise-above-127", {**OPEN, "OSC2_SHAPE": 4, "MIX": 127, "OSC2_PITCH": 127, "OSC2_TRACK": 1}, note(100), spec_mean=1.0, spec_max=3.0)


# 4e: whole patches: every factory patch (tools/bank/factory) on one note, and (unison off: a stack of 8 voices a note
# would steal here, 32 there) a 3-note chord. Free phases, random LFOs and delay times by the tempo make a null
# meaningless: level within 0.5 dB, the spectrum within 1 dB on average (proposal 10, Stage 4e)
# Known: an arp's steps land on our 16-frame grid (its notes' envelopes then differ: env and level are not bounded
# for the arp patches); 073's sine through a strong filter env shows its attack's first 0.4 ms (our cutoff ramps
# linearly between 16-frame points, Synth2's moves every frame: -30 dB, then -85 dB), alone in bands 50 dB down
SOFT = {"073": dict(spec_mean=4.0, spec_max=15.0)}
for f in sorted((ROOT / "tools/bank/factory").glob("*.sy1")):
    pv = sy1.parse(f.read_bytes())["values"]
    arp = dict(level=1.5, env=99.0, spec_mean=2.0) if pv[59] else {}
    bnd = {**dict(level=0.5, env=6.0, spec_mean=1.0, spec_max=6.0), **arp, **SOFT.get(f.stem, {})}
    case(f"fac-{f.stem}", {"_values": pv}, note(60, off=1.0), seconds=2.0, **bnd)
    if not pv[73]:
        case(f"fac-{f.stem}-chord", {"_values": pv}, sum((note(n, on=0.03 * i, off=1.0) for i, n in enumerate((48, 55, 64))), []),
             seconds=2.0, **bnd)

def script(patch, events, seconds):
    vals = list(patch["_values"]) if "_values" in patch else list(sy1.tables()["init"])
    for k, v in patch.items():
        if k != "_values":
            vals[P[k]] = v
    lines = [f"p {i} {v}" for i, v in enumerate(vals)]
    for e in sorted(events, key=lambda e: e[1]):
        lines.append(" ".join(str(x) for x in e))
    lines.append(f"len {LEAD + int(seconds * FS) // 256 * 256}")
    return "\n".join(lines) + "\n"


def db(x):
    return 20 * np.log10(max(x, 1e-12))


def rms(x):
    return float(np.sqrt(np.mean(x * x))) if len(x) else 0.0


def thirds(x):
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    out, fc = [], 40.0
    while fc <= 16000:
        m = (f >= fc / 2 ** (1 / 6)) & (f < fc * 2 ** (1 / 6))
        out.append(10 * np.log10(max(float(spec[m].sum()), 1e-30)))
        fc *= 2 ** (1 / 3)
    return np.array(out)


def lowpass(x, hz):
    spec = np.fft.rfft(x)
    spec[np.fft.rfftfreq(len(x), 1 / FS) > hz] = 0
    return np.fft.irfft(spec, len(x))


def measure(ref, our):
    m = {"level": db(rms(our)) - db(rms(ref))}
    w = len(ref) // 256
    r = np.sqrt((ref[:w * 256].reshape(w, 256) ** 2).mean(axis=1))
    o = np.sqrt((our[:w * 256].reshape(w, 256) ** 2).mean(axis=1))
    loud = r > 10 ** (-50 / 20)
    m["env"] = float(np.max(np.abs(20 * np.log10(np.maximum(o[loud], 1e-9) / r[loud])))) if loud.any() else 0.0
    tr, to = thirds(ref), thirds(our)
    keep = tr > tr.max() - 60
    m["spec_mean"] = float(np.mean(np.abs(to[keep] - tr[keep])))
    m["spec_max"] = float(np.max(np.abs(to[keep] - tr[keep])))
    m["null"] = db(rms(lowpass(our - ref, 8000))) - db(rms(lowpass(ref, 8000)))
    return m


def main():
    ref_bin, vs_bin, outdir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    outdir.mkdir(parents=True, exist_ok=True)
    names = sys.argv[4:] or list(CASES)
    failed = 0
    print(f"{'case':<20} {'level':>7} {'env':>6} {'spec':>6} {'worst':>6} {'null':>7} {'side':>6}")
    for name in names:
        patch, events, seconds, bounds = CASES[name]
        s = outdir / f"{name}.txt"
        s.write_text(script(patch, events, seconds))
        for b, tag in ((ref_bin, "ref"), (vs_bin, "vs")):
            subprocess.run([b, str(s), str(outdir / f"{name}.{tag}.f32")], check=True)
        ref2 = np.fromfile(outdir / f"{name}.ref.f32", dtype="<f4").reshape(-1, 2).astype(float)
        our2 = np.fromfile(outdir / f"{name}.vs.f32", dtype="<f4").reshape(-1, 2).astype(float)
        ref, our = ref2.mean(axis=1), our2.mean(axis=1)
        m = measure(ref, our)
        rs, os_ = ref2[:, 0] - ref2[:, 1], our2[:, 0] - our2[:, 1]   # the side: what PAN moves
        m["side"] = db(rms(lowpass(os_ - rs, 8000))) - db(rms(lowpass(rs, 8000))) if rms(rs) > 1e-6 else None
        bad = [k for k in ("env", "spec_mean", "spec_max") if m[k] > bounds[k]]
        if abs(m["level"]) > bounds["level"]: bad.append("level")
        if bounds["null"] is not None and m["null"] > bounds["null"]: bad.append("null")
        if bounds["side"] is not None and (m["side"] is None or m["side"] > bounds["side"]): bad.append("side")
        failed += bool(bad)
        side = f" {m['side']:>6.1f}" if m["side"] is not None else "       "
        print(f"{name:<20} {m['level']:>+7.2f} {m['env']:>6.2f} {m['spec_mean']:>6.2f} {m['spec_max']:>6.2f} {m['null']:>7.1f}{side}"
              + (f"   FAIL {' '.join(bad)}" if bad else ""))
    print(f"compare: {len(names)} cases, {failed} failed")
    sys.exit(1 if failed else 0)


main()
