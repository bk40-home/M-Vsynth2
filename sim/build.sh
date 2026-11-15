#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the M-Vsynth2 online simulator (WebAssembly): the firmware itself with sim/mv_websim.c as its platform.
# Needs Emscripten (emcc) on PATH: on Windows, in WSL (Ubuntu), or let GitHub build it (.github/workflows/pages.yml).
#   sh sim/build.sh
# Output: docs/sim/ (mvsynth2.js, mvsynth2.wasm, index.html, sim.js, link.js, audio-worklet.js, the kit's theme.css, theme.js, count.js,
# sim.css). Serve docs/ and open it in Chrome or Edge:
#   python3 -m http.server --directory docs 8000    # then http://localhost:8000/sim/
# The same platform tested without emcc: sh tests/run_websim.sh
set -e
cd "$(dirname "$0")/.."
command -v emcc >/dev/null || { echo "emcc not found: install Emscripten (emsdk) or build on GitHub (pages.yml)"; exit 1; }
OUT=docs/sim
KIT=third_party/fm1-kit/web
mkdir -p "$OUT"
cp sim/index.html sim/sim.js sim/link.js sim/audio-worklet.js "$KIT/theme.css" "$KIT/theme.js" "$KIT/count.js" "$KIT/sim.css" "$OUT"/
BUILD=$(date +%Y%m%d%H%M%S)                            # the build stamp: ?v=<stamp>, so no stale cached script survives
sed -i "s/__BUILD__/$BUILD/g" "$OUT/index.html" "$OUT/sim.js"
emcc -O2 -DDINGHY_WEBSIM -DAPP_VER='"sim"' \
    -Isim -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    firmware/src/mvsynth2.c firmware/src/vs_dsp.c \
    -sASYNCIFY -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 \
    -sMODULARIZE=1 -sEXPORT_NAME=MVsynth2 -sENVIRONMENT=web \
    -sEXPORTED_RUNTIME_METHODS=cwrap,ccall,HEAPU8,HEAPF32,HEAPU16 \
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_wsim_pull,_wsim_fb,_wsim_keys,_wsim_btn,_wsim_enc,_wsim_midi,_wsim_sysex_in \
    -o "$OUT/mvsynth2.js"
echo "built -> $OUT/ (open http://localhost:8000/sim/ after: python3 -m http.server --directory docs 8000)"
