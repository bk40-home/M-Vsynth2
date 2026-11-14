#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The firmware's engine against Synth2's own, on this computer (no FM-1, no JieLi toolchain). Needs Synth2's sources
# (tools/synth2.py says where), a C++20 compiler (g++ 11 or later) and numpy.
#   sh tests/run_compare.sh [case ..]        (all cases, or the ones named; the table says what each column is)
# It builds tools/compare/ref_render (Synth2's EngineCore, compiled as it is) and tests/vs_render (the firmware's
# engine), plays every case of tools/compare/compare.py through both and compares what comes out. The renders stay
# in build/compare/ (stereo float32, 44100 Hz) for a listen: NAME.ref.f32 and NAME.vs.f32.
set -e
cd "$(dirname "$0")/.."
OUT=build/compare
mkdir -p "$OUT"
S2=$(python3 -c "import sys; sys.path.insert(0, 'tools'); import synth2; print(synth2.root())")
${CXX:-g++} -std=c++20 -O2 -I "$S2/EngineCore" -o "$OUT/ref_render" tools/compare/ref_render.cpp
cc -std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware/src -Ifirmware/gen -o "$OUT/vs_render" tests/vs_render.c
python3 tools/compare/compare.py "$OUT/ref_render" "$OUT/vs_render" "$OUT" "$@"
