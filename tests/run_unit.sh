#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# M-Vsynth2 unit tests on this computer (no FM-1, no JieLi toolchain). Strict flags: what passes here also has to
# pass the chip's C compiler. Stages 1 to 4a: the patch, the .sy1 tool, the factory bank; the law tables; the
# oscillator tables and the gate; the voice engine. How the engine sounds against Synth2's: tests/run_compare.sh.
# The generators that read Synth2 (tools/synth2.py says where it is) run only when it is there: firmware/gen/ is
# committed, so the tests need no checkout; tests/sy1_test.py does (Synth2's own test data) and is skipped without.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/unit}
mkdir -p "$OUT"
CF="-std=c99 -O2 -Wall -Wextra -Werror -pedantic -Ifirmware/src -Ifirmware/gen -Ifirmware/hal"
S2=$(python3 -c "import sys; sys.path.insert(0, 'tools'); import synth2; print(synth2.root(False) or '')")
python3 tools/gen_wave.py
python3 tools/gen_words.py firmware/words.tsv docs/words firmware/gen/ann_words.h   # the screen reader's words (vs_speak.c)
if [ -n "$S2" ]; then
    python3 tools/gen_params.py
    python3 tools/gen_tables.py | head -1
    python3 tools/gen_bus.py | head -1
    python3 tools/make_bank.py
else
    echo "note: Synth2's sources are not here (tools/synth2.py): firmware/gen/ is used as committed"
fi
if git rev-parse --git-dir >/dev/null 2>&1; then
    git diff --quiet -- firmware/gen/ || echo "note: firmware/gen/ changed: commit it"
fi
# the device build's source checks that need no JieLi toolchain (register literals, volatile casts, asm outside
# hal/): caught here first. The ROM-constant check reads the chip's disassembly, so only ./build.sh can run it.
python3 -c "import sys; sys.path.insert(0, 'tools'); import build; e = build.mmio_check(); print(*e, sep='\n'); sys.exit(1 if e else 0)"
echo "mmio_check: ok"
for t in vs_patch_test vs_tables_test vs_gate_test vs_voice_test vs_xfer_test; do
    cc $CF -o "$OUT/$t" "tests/$t.c" -lm
    "$OUT/$t"
done
if [ -n "$S2" ]; then
    python3 tests/sy1_test.py "$OUT/vs_patch_test"   # the .sy1 tool against Synth2's fixture, and against the C
else
    echo "note: tests/sy1_test.py skipped (it needs Synth2's sources)"
fi
# the library page (docs/library.html): fm1-kit's model, and M-Vsynth2's adapter against tools/sy1.py and the
# pretend FM-1. Needs Node; the adapter test needs Synth2's sources (its data comes from tools/sy1.py)
if command -v node >/dev/null 2>&1; then
    node third_party/fm1-kit/library/test/model_test.js
    node sim/link-test.mjs                    # the simulator's Link FM-1 (sim/link.js) with pretend devices
    node third_party/fm1-kit/web/test/fm1ota_test.mjs .   # the firmware page's installer: a pretend FM-1, and as tools/fm1_install.py
    if [ -n "$S2" ]; then
        python3 tools/gen_library_page.py --data "$OUT/library_data.json" >/dev/null
        node tests/library_test.js "$OUT/library_data.json"
    fi
else
    echo "note: the library tests skipped (no node)"
fi
