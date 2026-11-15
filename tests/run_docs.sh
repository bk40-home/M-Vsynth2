#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The documents generated from the firmware's own tables: docs/cheatsheet.html (the pages, the CC map; it fails on a
# parameter on no page, or on two), docs/library.html (the patch library: the .sy1 rules, Synth2's, and the factory bank in the page) and docs/flash/
# (the firmware page: fm1-kit's flash.js and fm1ota.js) and docs/img/ (the front page's screens). Then the panel's screens from the host build (tests/scripts/ui.txt) into build/host/.
set -e
cd "$(dirname "$0")/.."
python3 tools/gen_cheatsheet.py
S2=$(python3 -c "import sys; sys.path.insert(0, 'tools'); import synth2; print(synth2.root(False) or '')")
if [ -n "$S2" ]; then python3 tools/gen_library_page.py; else echo "note: Synth2's sources are not here: docs/library.html as committed"; fi
sh flash/build.sh
# the web page's screens (docs/img/), from every screen of the firmware (tests/scripts/ui_all.txt)
sh tests/run_host.sh tests/scripts/ui_all.txt >/dev/null
set -- 700 home 1700 osc1 2100 osc2 2900 filter-env 3300 filter 4300 effect 4700 delay 5100 chorus 5500 voice 6500 arp 7700 themes 10000 bank
while [ $# -gt 1 ]; do cp "build/host/mvsynth2-$1.png" "docs/img/$2.png"; shift 2; done
sh tests/run_host.sh tests/scripts/ui.txt >/dev/null && echo "screens: build/host/mvsynth2-*.png"
