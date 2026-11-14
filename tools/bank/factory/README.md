# The factory bank

These `.sy1` files are Synth2's "Synth2 Factory" bank, copied unchanged from
[chab/oxo_synth2](https://github.com/chab/oxo_synth2) `Shared/Resources/Synth2 Factory/` at commit `6953424`
(MIT, Copyright (c) 2026 Francois Chabloz; see `LICENSING.md`). He confirmed by email (14 Nov 2026) that this bank is his and may be shipped under the MIT license, with the
"Madame Eljay" bank kept out (it is not in this tree).

`python3 tools/make_bank.py` turns this folder into `firmware/gen/vs_factory.h`. A file's slot is the number its
name starts with (`012.sy1`, `012_Lead.sy1`: slot 12); files without one fill the free slots in name order; slots
no file names stay EMPTY. `001.sy1` is the INIT patch.
