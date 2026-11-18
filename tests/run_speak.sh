#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The screen reader (vs_speak.c; fm1-reader, issue #1) on this computer, no FM-1:
#   1. the words file is current: tools/gen_words.py from firmware/words.tsv gives the committed firmware/gen/ann_words.h
#   2. a session (tests/scripts/speak.txt): what the device says, against tests/scripts/speak.expect
#   3. every screen, knob, value and message (MVS2_SPEAKDUMP) through fm1-reader's tools/audit.mjs with the words file:
#      its errors fail; its warnings are listed. Expected: the factory patches' own names ("PAD JX-3P Square": not
#      words), and a CONTROL route to a long name ("ASSIGN1 TO LFO 1 KEYSYNC": longer than a readout should be)
# fm1-reader: FM1_READER, or ../fm1-reader next to this repository (step 3 is skipped without it, or without Node).
# A dictionary for the audit's not-a-word check: DICT (default /usr/share/dict/words, if there).
set -e
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
fail=0

python3 tools/gen_words.py firmware/words.tsv docs/words "$OUT/ann_words.h" >/dev/null
if ! cmp -s "$OUT/ann_words.h" firmware/gen/ann_words.h; then
    echo "FAIL: firmware/gen/ann_words.h is not from firmware/words.tsv: python3 tools/gen_words.py firmware/words.tsv docs/words firmware/gen/ann_words.h"
    fail=1
fi
W=docs/words/$(sed -n 's|.*/words/\(.*\.json\)".*|\1|p' firmware/gen/ann_words.h)
[ -f "$W" ] || { echo "FAIL: $W missing (commit docs/words/)"; fail=1; }
echo "words: $W"

cc -O1 -Wall -Wno-unused-function -DDINGHY_HOST -DAPP_VER='"dev"' -Ifirmware/hal -Ifirmware/src -Ifirmware/gen \
    -o "$OUT/mvsynth2-speak" firmware/src/mvsynth2.c firmware/src/vs_dsp.c
MVS2_SAID_LOG="$OUT/said.tsv" "$OUT/mvsynth2-speak" "$OUT/speak" tests/scripts/speak.txt >/dev/null
python3 - "$OUT/said.tsv" tests/scripts/speak.expect <<'PY' || fail=1
import sys
said = open(sys.argv[1]).read().splitlines()
want = [l for l in open(sys.argv[2]).read().splitlines() if l and not l.startswith("#")]
i = 0
for w in want:
    while i < len(said) and not said[i].startswith(w):
        i += 1
    if i == len(said):
        print("FAIL: said log has no", repr(w), "in order. The log:", *said, sep="\n  ")
        sys.exit(1)
    i += 1
if any(l.startswith("MSG") for l in said[:1]):
    print("FAIL: said something before the first HELLO"); sys.exit(1)
print(f"session: {len(said)} said, {len(want)} expected, in order: ok")
PY

MVS2_SPEAKDUMP="$OUT/speak.tsv" "$OUT/mvsynth2-speak" "$OUT/speak" >/dev/null
echo "dump: $(wc -l < "$OUT/speak.tsv") messages -> $OUT/speak.tsv"
R="${FM1_READER:-../fm1-reader}"
if [ -f "$R/tools/audit.mjs" ] && command -v node >/dev/null; then
    D="${DICT:-/usr/share/dict/words}"
    node "$R/tools/audit.mjs" "$OUT/speak.tsv" --words "$W" --allow tests/speak_allow.txt ${D:+$( [ -f "$D" ] && echo --dict "$D")} > "$OUT/audit.txt" 2>&1 || fail=1
    tail -1 "$OUT/audit.txt"
    node "$R/tools/audit.mjs" "$OUT/speak.tsv" --words "$W" --spoken > "$OUT/spoken.tsv" 2>/dev/null || true
    echo "audit: $OUT/audit.txt; as heard: $OUT/spoken.tsv"
else
    echo "note: no fm1-reader at $R (FM1_READER) or no Node: the audit is skipped"
fi
[ $fail = 0 ] && echo "run_speak: ok" || echo "run_speak: FAILED"
exit $fail
