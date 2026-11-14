#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The online simulator's platform (sim/mv_websim.c) tested natively: the app + the sim backend built with cc, with
# tests/websim_stub/emscripten.h in place of Emscripten's (no emcc needed). Exit 0 = all checks passed.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wno-unused-function -Wno-unused-parameter -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
   -Itests/websim_stub -Isim -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
   -o build/host/websim firmware/src/mvsynth2.c firmware/src/vs_dsp.c tests/websim_test.c
build/host/websim
