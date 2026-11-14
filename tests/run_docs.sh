#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The documents generated from the firmware's own tables: docs/cheatsheet.html (the pages, the CC map; it fails on a
# parameter on no page, or on two) and docs/library.html (the patch library: the .sy1 rules, Synth2's, and the factory bank in the page). Then the panel's
# screens from the host build (tests/scripts/ui.txt) into build/host/.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_cheatsheet.py
S2=$(python3 -c "import sys; sys.path.insert(0, 'tools'); import synth2; print(synth2.root(False) or '')")
if [ -n "$S2" ]; then python3 tools/gen_library_page.py; else echo "note: Synth2's sources are not here: docs/library.html as committed"; fi
sh tests/run_host.sh tests/scripts/ui.txt >/dev/null && echo "screens: build/host/mvsynth2-*.png"
