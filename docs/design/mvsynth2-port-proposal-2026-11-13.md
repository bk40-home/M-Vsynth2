# M-Vsynth2: Synth2 on the M-VAVE FM-1

**Port proposal, 2026-11-13.** Status: signed off the same day; Stages 0 to 6 are done on the computer (14 Nov: 379 cases against Synth2, every factory patch among them; the panel, the banks and the transfer in). Stage 7's documents are generated (`tests/run_docs.sh`); the FM-1 checks of section 10 remain. The gate's
result is section 3.5: feasible at 8 voices, with the oscillator tables in RAM.

> **Sign-off (Kris, 13 Nov 2026).** Option a for all 13 decisions, with one change to decision 1: the base is
> `dinghy-bk40` (Dinghy 0.4), with M-vance's platform files laid over it (section 2.1). M-vance on the FM-1 does not
> pass 60 % of the buffer with everything on (section 3.4). Identity `FM-1_96809N`, to be checked on the hub.
> **Changed 15 Nov 2026:** `96809N` falls in M-vance's range; the identity is `FM-1_968100N` (dev `FM-1_9681000`), free on the hub.
> Upstream is contacted once the port is shown to be feasible.

M-Vsynth2 would be to [Synth2](https://github.com/chab/oxo_synth2) what M-vance is to the JT-8000: the same engine,
rewritten in C99 fixed point for the FM-1's chip, on the Dinghy platform, with the panel as pages and the patches in
flash. Synth2 is François Chabloz's MIT-licensed recreation of Synth1 (Daichi Laboratory), matched to the original by
measuring its audio output. Its patches are `.sy1` files of 99 native integers; this port keeps those 99 values
exactly, so any `.sy1` loads and goes back unchanged.

Read against: `oxo_synth2` at `6953424` (master, 2026-11-13) and `C:\GitHub\M-vance` at `a460d52` (v2.5.1).

## 0. What I need from you

**Decisions** (my recommendation is option a in every case; the section says why):

| | Decision | a (recommended) | b |
| --- | --- | --- | --- |
| 1 | Where it lives (2.1) | A new repo, started as a copy of M-vance v2.5.1 with the `mv_*` engine and panel taken out | A fresh copy of Dinghy; M-vance's platform fixes re-applied by hand |
| 2 | Name and identity (9) | `M-VSYNTH2` on the screen, `STORE_MAGIC` "MVS2", a hub identity you confirm is free | Another name |
| 3 | Voices (3.4) | 8 built in, confirmed or cut at the Stage 3 gate from a measurement on the FM-1 | Fix a lower number now |
| 4 | Oscillators (4.2) | Synth2's own method: band-limited tables, one every 6 semitones (160 KB of flash) | M-vance's polyBLEP oscillators (no flash, but not Synth2's sound in the detune copies) |
| 5 | Control rate (4.4) | LFOs and cutoff every 16 frames, ramped | Every frame, as Synth2 (dearer; only if the gate shows room) |
| 6 | LPDL filter (4.5) | In v1 behind `VS_LPDL`, built last of the filters, with a voice cut if it needs one | Left out of v1: type 4 plays LP24, marked |
| 7 | Extras Synth1 has not (4.8) | None but an output limiter: a 1:1 port | Add M-vance's plate reverb as a global |
| 6 | Banks (5.3) | FACTORY (read-only) + 8 user banks of 128, one flash slot each | 2 user banks in blobs (a save erases 1 sector, not 4) |
| 9 | Panel CCs (8) | A fixed CC a parameter, M-Vsynth2's own map, generated | No CC map: only the patch's two CTRL routes |
| 10 | Factory bank (5.4) | Synth2's own "Synth2 Factory" (57 patches, MIT) | Your own bank, made on the FM-1 (can follow later either way) |
| 11 | Patch loading (6) | Own SysEx over USB, `docs/bank.html`, a Python CLI | Compile-time only (`make_bank.py`) |
| 12 | Old `.sy1` versions (6.1) | Converted on the computer before sending | Converted in the firmware (+ ~6 KB, a second copy of the tables to keep in step) |
| 13 | SEQ, PLAY, REC (7.1) | SEQ opens the bank browser; PLAY and REC unused | Another use |

**Facts I could not get from the two repositories:**

1. **M-vance's CPU numbers.** The DEBUG page's CPU average and 3 s peak for (i) an 8-voice chord on the INIT patch,
   (ii) the same on a SUPER SAW program, (iii) that with MULTI-FX, DELAY and REVERB on. They give the chip's budget
   per voice, which section 3.4 can only estimate. This is the one number the whole plan leans on.
2. **The `fm1-port/` proposals** your headers cite (`mvance-panel-layout-proposal-2026-11-01.md` and the rest). They
   are not in `C:\GitHub\M-vance`. If you connect that folder I will match their template exactly and file this
   one beside them.
3. **A free hub identity** (`FM-1_9..`; M-vance is `96808N`), and the colour for the update loader's screen.
4. **Whether you want upstream told.** Synth2's README asks people porting it to open an issue first. A fork under MIT needs no
   permission, but section 9 lists two things worth asking François Chabloz anyway.
5. **Scope after v1:** the SMK-37 build and an fm1-dock module. Both are possible later (section 10); neither is in
   the stages below.

## 1. Scope

| Ported (v1) | Not ported |
| --- | --- |
| The engine: `Synth.hpp`, `Voice.hpp`, `Filter.hpp`, `Envelope.hpp`, `Lfo.hpp`, `Effects.hpp`, `WaveTables.hpp`, and the laws in `MeasuredTables.hpp` | The plugin shells (VST3, AUv2, AUv3), the SwiftUI and Win32 editors, themes, update check |
| The 99 parameters, their ranges, defaults and the INIT patch (`Params.hpp`, `InitPatch.hpp`) | Polyphony above 8; sample rates other than 44.1 kHz |
| The `.sy1` codec and the legacy conversion, on the computer (`Sy1Codec.hpp`, `LegacyTables.hpp`) | The file-system patch library (`PatchLibrary.hpp`): replaced by banks in flash |
| Value display (`ParamFormat.hpp`) | MIDI learn (replaced by the fixed CC map, decision 9) |
| The arpeggiator, the two CTRL routes, program change | Zip import on the device (the browser page does it) |
| The measurement corpus as a test reference | Any new measurement of Synth1. The clean-room rule stands: nobody touches the original's binary |

Synth2's own rule for ports is "1:1, no new features". The panel, the banks and the transfer are the FM-1's way of
doing what the plugin's window does; the sound gets nothing added (decision 7).

## 2. Structure

### 2.1 Repository (decision 1)

As signed off: `C:\GitHub\M-Vsynth2` is `dinghy-bk40` at `22acb91` (Dinghy 0.4), taken with `git archive` so every
file has LF line ends, with the platform files M-vance changed laid over it. The two trees differ in few places:

| From M-vance v2.5.1 | Why |
| --- | --- |
| `firmware/src/platform.c`, `usb.c`, `uac_out.c`, `uac_out.h`, `firmware/hal/fm1_usb.h`, `fm1_flash.h`, `fm1_flash_map.h` | The USB audio output, the sample-timed MIDI clock out, the app's SysEx hook |
| `tools/build.py` | The DSP built apart at `-O2` beside the `-Os` platform |
| `tests/host.c`, `tests/run_host.sh` | The host's tick counter (render timing), SysEx in a script |
| `flash.ps1`, `.gitattributes`, `.gitignore`, `vs_fixed.h` (M-vance's `mv_fixed.h`) | |

One platform line is new: `APP_SYSEX_ID` in `platform.c` (section 6.2). Everything else in the platform is
untouched. Dinghy's logo is designburg's and not under the GPL: it is out of the tree.

### 2.2 Files

| M-vance | M-Vsynth2 | From Synth2 |
| --- | --- | --- |
| `mvance.c` | `mvsynth2.c`: the app, the unity build | |
| `mv_build.h` | `vs_build.h` (written) | |
| `mv_fixed.h` | `vs_fixed.h`: M-vance's, renamed | |
| `mv_patch.*`, `gen/mv_params.h` | `vs_patch.*` (header written), `gen/vs_params.h` (generated, written) | `Params.hpp`, `Sy1Codec.hpp`, `InitPatch.hpp`, `ParamFormat.hpp` |
| `mv_osc.*`, `gen/mv_jvtri.h` | `vs_osc.*`, `gen/vs_wave.h` | `WaveTables.hpp`, the oscillator part of `Voice.hpp` |
| `mv_env.*`, `mv_lfo.*` | `vs_env.*`, `vs_lfo.*` | `Envelope.hpp`, `Lfo.hpp` |
| `mv_filter.*` | `vs_filter.*` | `Filter.hpp` |
| `mv_fx.*`, `mv_reverb.*` | `vs_fx.*` (effect, EQ, delay, chorus); no reverb | `Effects.hpp` |
| `mv_arp.*` (Skiff's) | `vs_arp.*` (Synth1's arp) | the arp in `Synth.hpp` |
| `mv_lim.*` | `vs_lim.*`: M-vance's, renamed | |
| `mv_voice.*`, `mv_dsp.c` | `vs_voice.*` (header written), `vs_dsp.c` | `Synth.hpp`, `Voice.hpp` |
| `mv_prog.*`, `gen/mv_factory.h` | `vs_prog.*` (header written), `gen/vs_factory.h` | `PatchLibrary.hpp` (the slot rules only) |
| `mv_xfer.*` | `vs_xfer.*` (header written) | |
| `mv_ui.*`, `mv_home.*`, `mv_debug.*` | `vs_ui.*` (header written), `vs_home.*`, `vs_debug.*` | the panel's sections (`SynthPanelView.swift`, as a list of what goes where: no artwork) |
| `mv_seq`, `mv_roll`, `mv_clip`, `mv_perf` | none | |
| `gen/mv_tables.h` | `gen/vs_tables.h` | `MeasuredTables.hpp`, the coefficient tables of `Filter.hpp` and `Effects.hpp` |
| `tools/gen_params.py`, `mvance-params.yaml` | the same, `vsynth2-params.yaml` (written) | |
| `tools/gen_tables.py` | reads `MeasuredTables.hpp` from a pinned Synth2 checkout, writes Q-format tables | |
| | `tools/gen_wave.py`: the band-limited tables | `WaveTableSet`'s recipe |
| `tools/make_bank.py` | `.sy1` folder to `gen/vs_factory.h` | |
| | `tools/sy1.py`: parse, legacy-convert, clamp, write `.sy1`; `tools/sy1_xfer.py`: the transfer from a terminal | `Sy1Codec.hpp`, `LegacyTables.hpp`, `TextEncoding.hpp` |
| `tools/compare/cmp_*.cpp` | the fixed-point module beside Synth2's float one, including `EngineCore/*.hpp` directly (C++20, the computer only) | |
| `tests/mv_*_test.c`, `tests/scripts/` | `tests/vs_*_test.c`, new scripts | `EngineCoreTests/legacy_fixture.txt` |

Synth2 is pinned as a git submodule at `third_party/oxo_synth2` (read by the generators and the compare tools,
never compiled into the firmware).

## 3. The chip's budgets

The limits are the platform's (`docs/API.md`); the M-vance column is its v2.5.1 build.

### 3.1 Flash (the app slot is 581,564 B)

| | M-Vsynth2 (estimate) | M-vance (measured) |
| --- | --- | --- |
| Platform and app shell | ~60 KB | |
| Oscillator tables: 19 saw + 19 triangle + 1 sine, 2048 points, int16 | 159,744 B | (JvTRI 60 KB) |
| Law tables (the ~25 measured tables, filter coefficients, limiter, LPDL grid, A.D. curves) | ~25 KB | |
| Factory bank: 128 records of 120 B | 15,360 B | |
| Engine code | ~70 KB | |
| Panel, home, save, bank, transfer, debug | ~45 KB | |
| **Total** | **~375 KB** | **270,196 B** |

About 200 KB to spare. The table figure is exact; the code figures are guesses from M-vance's size, and the build
prints the truth.

### 3.2 RAM (96 KB of `.data` + `.bss`)

One bank of 128 records in RAM (15,360 B) where M-vance holds three arrays of 64 patches (48.6 KB), so this is
lighter than M-vance. Voices are small: phases, envelope and filter states, no buffers.

### 3.3 Pool (344,064 B)

| | Bytes |
| --- | --- |
| Delay: 2 lines, int16, 1500 ms (`VS_DLY_MS`) | 264,612 |
| Chorus: 4 lines, int16, 80 ms (L, R, and the two that carry the delay's echoes through the chorus) | 28,256 |
| The canvas (`CV_MAX` 240 x 64) | 30,720 |
| **Total** | **323,588** |

It fits with 20 KB to spare. Synth1's longest delay is 4 beats plus the spread: 2.1 s at 120 BPM. A setting longer
than 1500 ms is held at the line's end, M-vance's rule; at 120 BPM that is every TIME above 3 beats. The build fails
above the limit, so the number cannot drift unseen.

### 3.4 CPU (decision 3)

`app_audio()` has 5.8 ms for 256 frames; RULES.md says stay under half. That is about 11 us a frame for everything.

I have no measurement of this chip, so what follows is a count, not a prediction. A voice at its dearest, a frame:

| | Table loads | Multiplies |
| --- | --- | --- |
| OSC1 pulse, interpolated (two saw reads) | 4 | 2 |
| Its 8 DETUNE copies as pulses (truncated reads) | 16 | 0 |
| SUB square + its 8 copies | 2 + 16 | 1 |
| OSC2 pulse, interpolated | 4 | 2 |
| Mix, SVF (two passes), its limiter, amp, pan | 1 | ~14 |
| **Sum** | **~43** | **~19** |

By its header comments, M-vance's dearest voice is SUPER SAW (seven naive saws: phase adds, no table), a polyBLEP
OSC2 and the 24 dB filter at 11 multiplies. So a full Synth2 voice has a similar count of multiplies and far more
table loads, all from flash. How much that costs depends on the chip's flash cache, which I cannot know from here. LPDL is dearer again: Synth2's own bench has it at a quarter of LP12's speed.

So the plan has a gate. **Stage 3** puts that voice loop on the FM-1 and reads its meter. From that number: `VS_NV`
stays 8, or drops; decision 5b becomes possible or not; LPDL gets a voice cut or not.

**A first figure, from the desk (13 Nov).** M-vance on the FM-1 stays under 60 % of the buffer with everything on.
`tools/bench/gate_vs_mvance.c` runs the gate's loop beside M-vance's whole engine on a computer, both at `-O2`, 8
voices each:

| 8 voices | x M-vance's full stack (SUPER SAW, MULTI-FX, DELAY, REVERB) |
| --- | --- |
| OSC1 + OSC2 pulses, SVF | 0.39 |
| the same, LP24 | 0.51 |
| + DETUNE's 8 copies, SVF | 0.59 |
| + SUB and its 8 copies, SVF | 0.86 |
| + SUB and its 8 copies, LP24 | 1.03 |

If the ratio held on the chip, eight ordinary voices would sit near 25 % and eight of the dearest near 60 %, before
the bus (EFFECT, EQ, DELAY, CHORUS; M-vance's own FX and reverb are about a quarter of its total). So: feasible
at 8 voices for ordinary patches, and tight for a patch with DETUNE, SUB and LP24 on every voice. This is a guess
with one known weakness: a computer's cache is not the chip's flash, and the gate's cost is table reads. The FM-1's
own figure replaces it.

### 3.5 The gate's result (FM-1, 13 Nov 2026)

Measured by Kris on the FM-1 with the gate build (`firmware/src/vs_gate.h`). CPU is the average, as % of the 5.8 ms
buffer; the peak ran 1 to 13 points above it.

| 8 voices unless said | Tables | CPU | A voice |
| --- | --- | --- | --- |
| No voices | | 0 % | |
| OSC1 + OSC2 pulses, SVF | flash | 18 % | 2.3 % |
| + DETUNE's 8 copies, SVF | flash | 42 % | 5.3 % |
| + SUB and its 8 copies, SVF, 6 voices | flash | 46 % | 7.7 % |
| the same, 7 voices | flash | 78 % | |
| the same, 8 voices | flash | 102 to 104 % (over the buffer) | |
| the same, 8 voices, every voice on one note (three tables) | flash | 56 % | 7.0 % |
| the same, 8 voices, the chord | **RAM** | **60 %** | **7.5 %** |
| the same with LP24 | RAM | 64 % | 8.0 % |

What it shows:

1. **The voice loop costs what the desk check said**: 0.86 x M-vance's 60 % predicted 52 %; the chip says 56 to 60 %.
2. **The flash has a cache of about 32 KB.** Reads are cheap while the tables in use fit it (8 tables of 4 KB) and
   very dear past it (9 tables: one voice more cost 32 %). From RAM, or with every voice on one note, the knee is gone.
   So **the oscillator tables are read from RAM**, not from flash (4.2).
3. **Eight voices stand** (decision 3), with a limit: a patch with DETUNE and SUB on every voice takes 60 to 64 % for
   its voices alone, before the bus. Two oscillators and a filter take 18 %.

What follows for the design:

- **Pool.** The 19 saw tables in RAM are 78 KB. The triangle falls as 1 / h^2, so it needs far fewer tables: 5 (one
  per two octaves) are 20 KB. With the sine, 102 KB. That leaves the delay lines 183 KB: `VS_DLY_MS` 1000, not 1500 (agreed, Kris 13 Nov).
- **Voices.** Always 8 (Kris, 13 Nov): no rule by load. A patch that is too dear for 8 is brought down with GLOBAL's
  VOICES, by the DEBUG meter, as on M-vance.
- **Code.** The cache holds code too. The gate's loop is small; the engine's will not be. If the engine shows the
  knee again, its hot loop goes to `.ram_text`, as the flash driver already does.

## 4. The engine

The full statement is the header comment of `firmware/src/vs_voice.h`. This section gives the reasons.

### 4.1 Rules

Synth2's laws are measured facts, listed in its `AGENTS.md` under "Engine facts". The port changes their arithmetic,
never their content. Every table comes through `tools/gen_tables.py` from Synth2's generated `MeasuredTables.hpp`, so
a law Synth2 corrects upstream is one regeneration here. Where the chip forces a difference it is in 4.9, and
nothing is invented to fill a gap.

### 4.2 Oscillators (decision 4)

Synth2 keeps one band-limited table per MIDI note, with a cubic stored per point: 11 MB, plus 3 MB of 2048-point
copy tables. The FM-1 has 568 KB.

**4a (recommended): one 2048-point int16 table per 6 semitones** from note 24 up, one below it, for saw and for
triangle. Each table carries every harmonic that stays under 26.1 kHz at the top note of its range, so what aliases
lands above 18 kHz and the lowest note of a range is still full to 17.7 kHz. Main reads interpolate linearly; pulse
is two saw reads, as in Synth2; the sub's square is two saw reads half a cycle apart.

The DETUNE copies are the reason to keep tables at all. Synth2 measured that the original reads its 8 copies from
2048-point tables with the phase truncated and no interpolation, and that the lines this leaves are part of how
Synth1's detune sounds. A truncated read is also the cheapest read there is: a shift, a load, an add. The
original's weakness is exactly what the FM-1 is good at.

OSC1's phase in the original moves in whole 2^-27-cycle steps (11 index bits, 16 fraction bits). In a Q32 phase
that is a step with its low 5 bits clear: free, and exact.

**4b: M-vance's polyBLEP oscillators.** No flash, proven on the chip. But eight polyBLEP copies a voice cost a
divide per edge each, and they would not have the truncated-table lines. I would only take this if 4a fails the gate.

### 4.3 Envelopes

Linear attack, exponential decay and release aiming 1/2048 below their target, from Synth2's two time tables as
per-block coefficients.

The amp envelope ports unusually well. Synth2 measured that outside the attack the original's amp gain is an
11-bit value taken every 256 frames and ramped linearly between. That is fixed-point control-rate code already, so
here it is exact and nearly free.

### 4.4 Control rate (decision 5)

Synth2 computes the LFO values, the filter cutoff and the oscillators' pitch modulation every frame. M-vance runs
all of these once per 16 frames and ramps. 16 frames is also Synth1's own sub-block: notes start on it, the
portamento steps on it.

**5a (recommended):** the cutoff (knob glide, key track, envelope, LFO), the LFO values and the mod envelope's pitch
move every 16 frames, ramped. Pitch from the bend and the LFOs is quantized to 1/128 octave in the original anyway,
so a step every 0.36 ms loses nothing audible. FM, sync, ring, pulse width and the amp gain stay per frame.

What it costs: an audio-rate LFO (SPEED 127 is 125 Hz) on cutoff is smoothed slightly, and a filter envelope
with attack 0 opens over 16 frames instead of at once.

### 4.5 Filters

| Type | Synth2 | Here |
| --- | --- | --- |
| LP12, HP12, BP12 | Chamberlin SVF, two passes a frame with the input held, f from a table of the knob | The same, states Q18, f Q30 from a table at quarter-knob steps |
| LP24 | Four bilinear one-poles, feedback through a one-frame delay, an AGC on the feedback | The same; the AGC's power law from a table |
| Output limiter (both) | A gain from a table on a follower of the output | The same table at Q15 |
| LPDL | A diode ladder with high-passed feedback, 2 x oversampled, trapezoidal: a 5 x 5 matrix inverted whenever the cutoff moves 1/8 knob | The same equations. The matrix is tridiagonal plus one feedback column, so it is solved by elimination each control block: a handful of divides, not a Gauss-Jordan in fixed point |
| Saturation | y = (1 + a) x / (1 + a abs(x)) | The same; one 32-bit divide a frame, skipped at SAT 0 |

LPDL (decision 6) is the risk in this table: it is the dearest filter, and its fixed-point solve is the one piece
of new numerical work in the port. It goes last, behind `VS_LPDL`, checked against Synth2's float filter on the
computer before it is ever flashed.

### 4.6 The bus

In Synth2's measured order: EFFECT, then the patch pan and LFO pan, EQ, DELAY, CHORUS.

- **EFFECT**, 10 types. The distortions, decimator, ring modulator, compressor and four phasers are filters and
  table curves: plain ports. A.D.1 is Synth2's fitted model with a feedback loop and two curves; the curves become
  257-point tables. Synth2 itself calls A.D.1 "a fitted model", not an identity, so the port inherits its error.
- **EQ**: the tone one-pole (always on: it is the original's output low-pass at 64) and the peak filter. The peak's
  coefficients need an exponential and a cosine; they are worked out in the main loop when a knob moves, from small
  tables, and handed to the ISR.
- **DELAY**: whole-sample taps as measured, the three types (ST, X, PP), the tone one-pole, the 0.215 s glide.
- **CHORUS**: four phase types on one sine LFO; with the delay on, the echoes pass through the chorus only, at
  unity, as measured.

### 4.7 Voices and play

Round-robin allocation, stealing the oldest released voice first; MONO and LEGATO with the key stack; portamento in
16-frame steps landing 1.35 % past its target; UNISON of 2 to 8 voices with detune, spread, pitch and phase; the
phase knobs with Synth2's per-slot start tables for slots 0 to 7; the sustain pedal; the two CTRL routes.

`POLY` and `UNISON VOICES` are played as min(value, `VS_NV`). The patch keeps the value it was given.

### 4.8 What is added (decision 7)

One thing: M-vance's look-ahead output limiter, last in the chain. A plugin has a host with headroom above full
scale; the FM-1's output clips at +-32767. Synth1 patches with unison and resonance go past that.

### 4.9 Not as Synth2

| | Synth2 | M-Vsynth2 | Heard as |
| --- | --- | --- | --- |
| Voices | 32 | 8 (the gate may cut it) | Long releases and big unison chords steal sooner |
| Oscillator tables | One a note, Hermite | One per 6 semitones, linear | The top half-octave of harmonics above 17.7 kHz missing on some notes |
| Control rate | Every frame | 16 frames, ramped | 4.4 |
| Delay length | Any | 1.5 s | Long settings at slow tempos shortened |
| LPDL | 5 x 5 inversion, double | Elimination, fixed point | To be shown equal by the compare tool |
| A.D.1 curves | 1024-point Hermite | 257-point linear | Below the model's own error |
| Sample format | double | Q15 to Q30 | A noise floor near -90 dB |
| Random sequences | xorshift (not Synth1's either) | The same xorshift | Nothing |

## 5. Patches and banks

### 5.1 The patch in RAM

`vs_patch_t`: 99 native values, a 16-character name, a colour. The header comment of `vs_patch.h` has the detail.

### 5.2 The stored record

120 bytes: the name, 95 values as one byte each, the four CTRL routing words as 16 bits each, the colour. Every
byte but the routing words is 7-bit, so the same record goes over MIDI without packing tricks.

### 5.3 Banks (decision 8)

Synth1 thinks in banks of 128 numbered slots. So does this:

- **FACTORY**: bank 0, in the firmware, read-only.
- **U1 to U8**: 128 slots each, one platform flash slot each (15,360 of 16,372 B). 1024 patches of your own.
- One bank is in RAM at a time. Changing bank is a flash read: immediate, no erase.
- A save marks the bank and writes it at the next silence. The slot is 4 sectors, so about 200 ms of erase: the same
  cost as M-vance's performance slot.
- Empty slots are real. PRESETS and program change skip them, as Synth2's arrows do.

8b (two banks in blobs, 32 patches a sector) makes a save four times shorter but leaves 256 patches. With the save
already waiting for silence, the shorter erase buys nothing you would hear.

### 5.4 Factory bank (decision 10)

`tools/make_bank.py` turns a folder of `.sy1` files into `firmware/gen/vs_factory.h`. Slot numbers come from the
file names, as in Synth2 (`012.sy1`, `012_Lead.sy1`).

Synth2 ships two banks. "Synth2 Factory" (57 patches, slot 001 the INIT patch) names no designer and reads as the
project's own, so it would come under the repository's MIT licence; I recommend bundling it, credited, once
upstream confirms that (section 9). "Madame Eljay" (128 patches) carries a designer's name, and Synth2's `AGENTS.md`
says a third-party bank needs its designer's explicit permission. Whatever permission Synth2 has is not ours: it
stays out unless the designer agrees.

## 6. Patch loading

### 6.1 From `.sy1` to a record (decision 12)

`tools/sy1.py` on the computer, and the same logic in JavaScript for the browser page:

1. Parse the text: the name line, `color=`, `ver=`, the `index,value` lines. A missing line takes the original's
   default.
2. Convert old versions (no `ver`, 100, 105 to 108) with Synth2's rules and tables.
3. Clamp every value to its range. The original plays an out-of-range value as the nearest in range.
4. Turn the name into ASCII (old patches are Shift-JIS or Windows-1252), 16 characters.
5. Pack the record.

Going back, a record becomes a `.sy1` with `ver=112`, as Synth2 writes it, so it opens in Synth2 and in Synth1.

The firmware therefore sees only today's encoding and still clamps whatever arrives. Putting the conversion on the
computer keeps about 6 KB of tables out of the firmware and, more to the point, keeps one copy of the rules, tested
against Synth2's own `legacy_fixture.txt`.

### 6.2 On the wire (decision 11)

M-vance borrows the JP-8080's SysEx. Synth1 never had hardware, so there is nothing to borrow: the format is our
own, under the non-commercial id `7D` with an "MV" signature. Write, clear, audition, name, request, identity, and
an acknowledgement for every write. The header comment of `vs_xfer.h` is the specification.

Two differences from M-vance's transfer, both because we own both ends:

- **Acknowledged.** The sender waits for the reply, not for 30 ms. Faster, and a lost message is sent again.
- **One platform line.** `platform.c` passes the app a SysEx whose first byte is Roland's `41`. That test becomes
  `APP_SYSEX_ID`, `7D` here. The updater's own frames always begin with `00` (they are a 7-bit stream of
  `00 59 ..`, `ota_unpack7`), so the two cannot be confused.

### 6.3 On the computer

- **`docs/bank.html`** (Chrome or Edge, WebMIDI), after M-vance's page. Open `.sy1` files, a folder, or a Synth1
  zip bank. **Play** auditions a patch without saving. **Send** writes the ticked patches into a bank from a chosen
  slot, or a whole bank slot for slot. **Read bank** fetches all 128; **Download** saves them as a zip of
  `NNN.sy1`, which is a Synth1 bank folder as it stands.
- **`tools/sy1_xfer.py`** does the same from a terminal: `send FOLDER --bank 2`, `read --bank 2 OUT/`,
  `play FILE.sy1`.

### 6.4 On the FM-1

PRESETS picks the slot; SEQ opens the bank browser; SAVE writes, swaps, clears. MIDI program change selects a slot
of the bank shown and, as in Synth2, is ignored for an empty slot. Bank select (CC 0, values 0 to 8) picks the bank.

## 7. Panel and displays

### 7.1 Buttons

The page layout is `vs_ui.h`'s header comment. The rule is M-vance's: a section button steps through its pages,
KNOB 1 to 4 edit the four columns, SELECT single-steps, ALGORITHM walks every page.

Synth1's panel has more sections than the FM-1 has buttons, so three choices were made:

- **FX SWITCH** gathers the three on/off switches (EFFECT, DELAY, CHORUS) on one page. Each would otherwise push
  its section to a fifth knob.
- **OSC COMMON** takes the mod envelope's switch and destination, leaving MOD ENV with its three knobs.
- **ARP** keeps M-vance's gesture: a tap is on/off, a hold opens the page. Its on/off is parameter 59, so the tap
  edits the patch.

SEQ has no sequencer to open, so it opens the **bank browser** (decision 13). PLAY and REC do nothing in v1.

### 7.2 Pages

Generated from the parameter table and checked: all 99 parameters appear exactly once (98 on pages, ARP's switch on
its button). The number is the `.sy1` index.

| Button | Page | KNOB 1 | KNOB 2 | KNOB 3 | KNOB 4 |
| --- | --- | --- | --- | --- | --- |
| LFO | LFO 1 1/2 | WAVE `42` | SPEED `43` | DEPTH `44` | DEST `41` |
| LFO | LFO 1 2/2 | LFO 1 `57` | TEMPO `67` | KEYSYNC `68` |  |
| LFO | LFO 2 1/2 | WAVE `47` | SPEED `48` | DEPTH `49` | DEST `46` |
| LFO | LFO 2 2/2 | LFO 2 `58` | TEMPO `69` | KEYSYNC `70` |  |
| EDIT | OSC 1 | SHAPE `0` | DETUNE `76` | FM `45` | PHASE `91` |
| EDIT | SUB OSC | GAIN `95` | SHAPE `96` | OCTAVE `97` |  |
| EDIT | OSC 2 | SHAPE `1` | PITCH `2` | FINE `3` | TRACK `4` |
| EDIT | OSC MIX | MIX `5` | P/W `8` | SYNC `6` | RING `7` |
| EDIT | OSC COMMON | KEYSHFT `9` | TUNE `72` | M.ENV `10` | M.DEST `71` |
| ENV | MOD ENV | ATTACK `12` | DECAY `13` | AMOUNT `11` |  |
| ENV | FILTER ENV | ATTACK `15` | DECAY `16` | SUSTAIN `17` | RELEASE `18` |
| ENV | AMP ENV | ATTACK `25` | DECAY `26` | SUSTAIN `27` | RELEASE `28` |
| SEL | FILTER 1/2 | TYPE `14` | FREQ `19` | RES `20` | AMOUNT `21` |
| SEL | FILTER 2/2 | KBDTRK `22` | SAT `23` | VEL SW `24` |  |
| SEL | AMP | GAIN `29` | VEL `30` | PAN `90` |  |
| FX | FX SWITCH | EFFECT `77` | DELAY `65` | CHORUS `66` |  |
| FX | EFFECT | TYPE `78` | CTRL 1 `79` | CTRL 2 `80` | LEVEL `81` |
| FX | EQ | TONE `60` | FREQ `61` | LEVEL `62` | Q `63` |
| FX | DELAY 1/2 | TYPE `82` | TIME `35` | FEEDBK `36` | DRY/WET `37` |
| FX | DELAY 2/2 | SPREAD `83` | TONE `98` |  |  |
| FX | CHORUS 1/2 | TYPE `64` | TIME `52` | DEPTH `53` | RATE `54` |
| FX | CHORUS 2/2 | FEEDBK `55` | LEVEL `56` |  |  |
| GLO | VOICE | MODE `38` | POLY `94` | PORTA `39` | AUTO `74` |
| GLO | UNISON 1/2 | UNISON `73` | VOICES `93` | DETUNE `75` | SPREAD `84` |
| GLO | UNISON 2/2 | PITCH `85` | PHASE `92` |  |  |
| GLO | CONTROL 1/2 | SRC 1 `86` | ASSIGN1 `87` | SENS 1 `50` | BEND `40` |
| GLO | CONTROL 2/2 | SRC 2 `88` | ASSIGN2 `89` | SENS 2 `51` |  |
| ARP | ARP | TYPE `31` | RANGE `32` | BEAT `33` | GATE `34` |

Then GLO continues with **GLOBAL** (MIDI CH, VOICES, TEMPO), **MIDI** (LOCAL, CLOCK, CLK OUT) and **DEBUG**: all
three are M-vance's, and none is in the patch.

28 patch pages. FX is the longest walk, at 7.

### 7.3 What a value shows

Synth2's `ParamFormat.hpp`, ported with integer arithmetic: choices by name (`LP24`, `S&H`, `UP&DOWN`), pitch as
`+12 st`, tune as `-3 ct`, LFO speed as `2.31 Hz` or a note value when TEMPO is on, delay and arp times as note
values (`1/8T`, `1/4.`), centred values signed. The three EFFECT knobs take the name of what they do in the type
playing: on DECI, CTRL 1 reads `RATE`.

### 7.4 Screens

Layouts in pixels are in `vs_ui.h`. In short:

- **A page**: header, four columns (label, value, bar), the last edit in large type, the program at the foot.
- **HOME**: a header drawing of our own, the bank and program, four knobs (FREQ, RES, AMOUNT, GAIN), a cell per
  voice (held bright, releasing dim: Synth2's voice grid), the scope.
- **BANK**: nine rows of slots around the cursor, the bank's name above.
- **SAVE**, **XFER**, **DEBUG**: M-vance's, with the bank added.

The look is the FM-1 family's, as M-vance has it. Nothing of Synth1's panel is drawn: Synth2's clean-room rule
covers its artwork, and Synth2's own name and icons are not under its code licence.

## 8. MIDI (decision 9)

Notes, bend (the patch's BEND range, in the original's 7-bit steps), the sustain pedal, program change, bank select,
clock and START, over USB and the TRS jack, as M-vance.

Synth1 has no CC map of its own, only two assignable routes in each patch. To keep M-vance's "every knob sends and
receives" I propose a fixed map, generated in page order over the CC numbers that have no standard meaning:

| CC | Parameter | | CC | Parameter | | CC | Parameter |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2 | LFO1 WAVE `42` | | 36 | MENV M.DEST `71` | | 75 | EQ Q `63` |
| 3 | LFO1 SPEED `43` | | 37 | MENV ATTACK `12` | | 76 | DELAY TYPE `82` |
| 4 | LFO1 DEPTH `44` | | 39 | MENV DECAY `13` | | 77 | DELAY TIME `35` |
| 5 | LFO1 DEST `41` | | 40 | MENV AMOUNT `11` | | 78 | DELAY FEEDBK `36` |
| 7 | LFO1 LFO 1 `57` | | 41 | FENV ATTACK `15` | | 79 | DELAY DRY/WET `37` |
| 8 | LFO1 TEMPO `67` | | 42 | FENV DECAY `16` | | 80 | DELAY SPREAD `83` |
| 9 | LFO1 KEYSYNC `68` | | 43 | FENV SUSTAIN `17` | | 81 | DELAY TONE `98` |
| 10 | LFO2 WAVE `47` | | 44 | FENV RELEASE `18` | | 82 | CHORUS TYPE `64` |
| 11 | LFO2 SPEED `48` | | 45 | AENV ATTACK `25` | | 83 | CHORUS TIME `52` |
| 12 | LFO2 DEPTH `49` | | 46 | AENV DECAY `26` | | 84 | CHORUS DEPTH `53` |
| 13 | LFO2 DEST `46` | | 47 | AENV SUSTAIN `27` | | 85 | CHORUS RATE `54` |
| 14 | LFO2 LFO 2 `58` | | 48 | AENV RELEASE `28` | | 86 | CHORUS FEEDBK `55` |
| 15 | LFO2 TEMPO `69` | | 49 | FILT TYPE `14` | | 87 | CHORUS LEVEL `56` |
| 16 | LFO2 KEYSYNC `70` | | 50 | FILT FREQ `19` | | 88 | VOICE MODE `38` |
| 17 | OSC1 SHAPE `0` | | 51 | FILT RES `20` | | 89 | VOICE PORTA `39` |
| 18 | OSC1 DETUNE `76` | | 52 | FILT AMOUNT `21` | | 90 | VOICE AUTO `74` |
| 19 | OSC1 FM `45` | | 53 | FILT KBDTRK `22` | | 91 | UNISON UNISON `73` |
| 20 | OSC1 PHASE `91` | | 54 | FILT SAT `23` | | 92 | UNISON VOICES `93` |
| 21 | SUB GAIN `95` | | 55 | FILT VEL SW `24` | | 93 | UNISON DETUNE `75` |
| 22 | SUB SHAPE `96` | | 56 | AMP GAIN `29` | | 94 | UNISON SPREAD `84` |
| 23 | SUB OCTAVE `97` | | 57 | AMP VEL `30` | | 95 | UNISON PITCH `85` |
| 24 | OSC2 SHAPE `1` | | 58 | AMP PAN `90` | | 102 | UNISON PHASE `92` |
| 25 | OSC2 PITCH `2` | | 59 | FX EFFECT `77` | | 103 | CTRL SENS 1 `50` |
| 26 | OSC2 FINE `3` | | 60 | DELAY DELAY `65` | | 104 | CTRL BEND `40` |
| 27 | OSC2 TRACK `4` | | 61 | CHORUS CHORUS `66` | | 105 | CTRL SENS 2 `51` |
| 28 | OSCC MIX `5` | | 62 | FX TYPE `78` | | 106 | ARP TYPE `31` |
| 29 | OSCC P/W `8` | | 63 | FX CTRL 1 `79` | | 107 | ARP RANGE `32` |
| 30 | OSCC SYNC `6` | | 70 | FX CTRL 2 `80` | | 108 | ARP BEAT `33` |
| 31 | OSCC RING `7` | | 71 | FX LEVEL `81` | | 109 | ARP GATE `34` |
| 33 | OSCC KEYSHFT `9` | | 72 | EQ TONE `60` | | 110 | ARP ARP `59` |
| 34 | OSCC TUNE `72` | | 73 | EQ FREQ `61` | |  |  |
| 35 | MENV M.ENV `10` | | 74 | EQ LEVEL `62` | |  |  |

Left out: CC 1 (the init patch's mod wheel route), 6, 32, 38, 64 to 69, 96 to 101, 120 up. The four routing words
and POLY have no CC. If a patch's route names a CC that is also in this map, both act, which is what Synth2's MIDI
learn does.

This map is ours, not Synth1's, and the cheat sheet will say so.

## 9. Name, identity, licence

- **Licence.** Dinghy is GPL-3.0-only, so the firmware is. Synth2 is MIT, which the GPL accepts. The ported files
  are GPL-3.0-only with Synth2's copyright line in each header and its MIT text in `LICENSING.md`: the MIT licence
  asks for that notice to stay with the code. I am not a lawyer; this is the same arrangement M-vance already uses for its
  MPL-2.0 USB audio files.
- **Names.** Synth2's README excludes "Oxo", "Oxo Music" and "Oxo Synth2" and the icons from the code licence, so
  none appear. "Synth1" belongs to Daichi Laboratory and appears only in the credits, with the same "not affiliated"
  line Synth2 carries.
- **Two things to ask upstream**, if you choose to write: that the 57 factory patches are his own to pass on under
  MIT, and whether "Synth2" inside our name is welcome. Neither blocks the work.
- **Identity.** `APP_NAME "M-VSYNTH2"`, `STORE_MAGIC 0x3253564D` ("MVS2"), `DATA_FAMILY "MVSYN2"`, `PRODUCT`
  `FM-1_968100N` (was `96809N`, which falls in M-vance's range; 15 Nov), the loader's screen in sea green
  (`#00C8A0`), all in `tools/build.py`.

## 10. Stages

Each stage ends with the firmware building, the tests passing and one thing to check on the FM-1.

| Stage | What | Tests on the computer | Check on the FM-1 |
| --- | --- | --- | --- |
| 0 (done) | The repo: `dinghy-bk40` with M-vance's platform files, names and identity in | `tests/run_tests.sh`: the platform's own tests | Installs, plays a sine, installs back to M-vance |
| 1 (done) | Patch: `gen_params.py`, `vs_patch`, `tools/sy1.py`, `make_bank.py` | Every Synth2 factory and starter `.sy1` round-trips text to record to text; `legacy_fixture.txt` passes; out-of-range values clamp as Synth2's tests say | None (no sound yet) |
| 2 (done) | Tables: `gen_tables.py` (the voice's 24 law tables; the bus's and LPDL's come with their code in 4c, 4d), `gen_wave.py` | Each table read back as the engine reads it, against Synth2's double: bounds asserted in the generator (worst 5e-4 relative); `tests/vs_tables_test.c`; table spectra checked in `gen_wave.py` | None |
| **3 (gate; done: 3.5)** | One worst-case voice loop (4.2, 4.5 SVF) times `VS_NV`, hard-wired | The loop's output against the same patch in Synth2 | **DEBUG's CPU reading: decides decisions 3, 5, 6** |
| 4a (done: 51 cases against Synth2, level within 0.03 dB, nulls of 40 to 65 dB) | Voice v0: OSC1, OSC2, mix, P/W, noise, SVF, both envelopes, gain, velocity, POLY | `tools/compare`: each module beside Synth2's (envelope times within one block; SVF response within 0.2 dB) | The INIT patch plays on the keys and over MIDI |
| 4b | Modulation: LFOs on all 7 destinations, mod env, FM, ring, sync, sub, DETUNE, phase knobs, unison, mono, legato, portamento, bend, CTRL routes | Synth2's own cases replayed: pitch-step laws, the sum laws, unison places | Factory patches by ear against Synth2 on the computer |
| 4c | LP24, the limiter, saturation, LPDL | Transfer functions against Synth2's filter at small and full level | CPU with 8 LPDL voices |
| 4d | The bus: EFFECT, pan, EQ, DELAY, CHORUS, the arp, the output limiter | Impulse responses (delay and chorus taps to the sample), EQ within 0.1 dB | CPU with everything on |
| 4e | Whole patches | Every factory patch through the host build and through Synth2's `s2render` with the same job: level within 0.5 dB, 1/3-octave spectrum within 1 dB mean. Then Synth2's `compare.py presets` against the Synth1 corpus | None |
| 5 | Panel and displays | Host scripts with screenshots; the cheat sheet builds, and fails on an undocumented knob | Every page by hand, once |
| 6 | Banks and transfer: `vs_prog`, `vs_xfer`, `bank.html`, `sy1_xfer.py` | `tests/xfer_test.py` on the host build: write, read back, compare; a bad sum refused; a full bank round trip | A zip bank sent, played, read back, opened in Synth2 |
| 7 | Release: README, cheat sheet, web page, simulator, the hub | `run_docs.sh`; CI on a tag | Install from the hub |

### 10.1 Where it stands (14 Nov 2026)

| Stage | Done | Deviations, and why |
| --- | --- | --- |
| 4b | sub, detune, FM / ring / sync, mod env, both LFOs on all 7 destinations, PAN, MONO / LEGATO, portamento, unison, CTRL routes | The mod env and the LFOs' pitch, P/W and FM run a frame at a time (the proposal said every 16 frames): ramped, an instant attack and a square LFO's step missed the null by 20 dB, and the cost is small |
| 4c | LP24 (AGC), LPDL (solved by elimination in Q30, 2 x oversampled, states Q24), the limiter, saturation | LPDL's coefficients follow the cutoff a block at a time (Synth2: a frame); at res 127 its ringing nulls to -24 dB |
| 4d | EFFECT (10 types), EQ peak (coefficients worked out on the chip), DELAY, CHORUS, the arp, the output limiter (M-vance's) | The arp's steps land on the 16-frame grid; delay and chorus lines are 16-bit at half scale; delay 1 s (VS_DLY_MS) |
| 4e | every factory patch, one note and a chord: level within 0.5 dB, spectrum within 1 dB on average | Arp patches: timing (above). 073: its attack's first 0.4 ms (the cutoff ramp) |
| 5 | the pages, HOME, BANK, SAVE, GLOBAL, the LEDs, the CC map, MIDI clock as the tempo | SAVE writes only (rename, swap, clear: from the computer page or `sy1_xfer.py`); no MIDI or DEBUG page (GLOBAL shows the clock; HOME the CPU) |
| 6 | `vs_prog` (FACTORY + U1..U8), `vs_xfer` (acknowledged), `docs/bank.html`, `tools/sy1_xfer.py`, `tests/vs_xfer_test.c` | `bank.html` opens files and folders, not zips; it writes zips |
| 7 | README, `docs/cheatsheet.html`, `tests/run_docs.sh` | The web page, simulator and hub entry wait for the FM-1 checks |

The gate comes before the engine on purpose. If eight full voices do not fit, the answer changes how the voice loop
is written, and it is cheaper to learn that from 200 lines than from 2000.

**Later, not in these stages:** the SMK-37 build (the platform already builds for it; velocity then reaches AMP VEL
and the filter's VEL SW, which the FM-1's own keys cannot), and an fm1-dock module (272 KiB of RAM: the delay lines
would drop to about 700 ms).

## 11. Risks

| Risk | Sign | Answer |
| --- | --- | --- |
| Eight voices do not fit | Stage 3's meter | Cut `VS_NV`, or take decision 4b for the copies. Fewer than 8 copies is not offered: it changes the sound |
| LPDL in fixed point is unstable or too dear | Stage 4c's compare | `VS_LPDL 0` ships, LPDL follows in a point release |
| Flash reads are slow enough to dominate | Stage 3 | Shorten the delay lines (`VS_DLY_MS`) and keep the tables of the notes sounding in the pool |
| A.D.1's model drifts in fixed point at high drive | Stage 4d | Wider state format for that effect alone |
| The 7D SysEx id meets another FM-1 firmware's | Unlikely: a firmware only hears its own | The "MV" signature |
| Upstream changes a law | Any time | The submodule pin; regenerate, rerun 4e |

## 12. Documentation

In line with M-vance, file for file:

| M-vance | M-Vsynth2 | State |
| --- | --- | --- |
| Header comments as the specification | `vs_build.h`, `vs_patch.h`, `vs_prog.h`, `vs_xfer.h`, `vs_voice.h`, `vs_ui.h` | Written with this proposal; they pass a C99 syntax check, nothing more |
| `tools/mvance-params.yaml`, `gen/mv_params.h` | `tools/vsynth2-params.yaml`, `gen/vs_params.h` | Written (generated from Synth2's `Params.hpp`, checked) |
| `README.md` | `README.md` | Drafted with this proposal |
| `LICENSING.md` | `LICENSING.md` | Drafted with this proposal |
| `docs/API.md`, `RULES.md`, `MAKE-IT-YOURS.md`, `SMK37.md` | The same files: they describe the platform, which does not change. One line added to API.md for `APP_SYSEX_ID` | Carried over at Stage 0 |
| `docs/cheatsheet.html` | Generated from the page table | Stage 5 |
| `docs/index.html`, `docs/img/`, `fmhub.json` | The same | Stage 7 |
| `docs/bank.html` | The same, for `.sy1` | Stage 6 |
| `fm1-port/*-proposal-*.md` | This file | Needs the folder (section 0, item 2) |
