# Filter type 5: SH101 (M-Vsynth2's own)

FILTER TYPE 5 is a model of the Roland SH-101's VCF (IR3109) and its resonance circuit, in single-precision float on the AC791N's FPU. It isn't one of Synth1's filters. Synth2 (and Synth1) play a type 5 patch as their top type, LPDL. Types 0–4 are unchanged: the host renders are bit-for-bit the same as before.

| | |
|:---|:---|
| Code | `firmware/src/sh101_vcf.h`: one header, no libm, no division per sample. It is shared with S1Vave and the Teensy work |
| Build switch | `VS_SH101` (vs_build.h): 1 by default; 0 = type 5 plays LP24 |
| FPU | `tools/build.py` adds `-mcpu=r3 -mfprev1 -ffp-contract=off -Werror=double-promotion`. `firmware/hal/fm1_irq.h` masks the FP exception traps on CPU0; CPU1 already did (`fm1_multicore.h`) |
| Range | `tools/vsynth2-params.yaml` gives FILTER TYPE `hi: 5` (M-Vsynth2's own range end). `gen_params.py` and `sy1.py` both take it, so the firmware, the banks and `.sy1` files agree |

## What is modelled

Values come from the SH-101 service manual:
- **Four poles:** IR3109 OTA stages, each `dy/dt = w tanh(x − y)` (TPT, the tanh of each stage's own argument).
- **Resonance:** VR18 → TR26/TR27 → D30/D31 → stage 1. The diodes set the oscillation level.
- **Output:** the 101's own level compensation, `y4 (1 + 0.202 k)` (R130 from the filter output plus R132 from the resonance wiper).

Drive, diode level and knee, and resonance against the slider are fitted to TAL-BassLine-101 captures, with the circuit kept as drawn. The module is checked against a double-precision, 8× oversampled, exact-Newton reference of the same circuit (the `sh101-ref` kit):
- response within 0.25 dB;
- singing pitch within 4.2 cents up to 4 kHz;
- level within 0.04 dB;
- the same onset;
- aliasing −62 dB at 1×.

## How it sits in the voice

| | |
|:---|:---|
| Place | MIX → SH101 → SATURATION → AMP. No per-voice output limiter, as LPDL: the diodes limit it. With the limiter, its follower (7 ms release) rode the singing wave: 9.4% THD at 17 Hz, 5.2% at 46 Hz, against 0.60% without (on the FM-1 A/B and the host). The bus limiter at the end still protects the output |
| Levels | Q14 in, Q20 out; 1.0 = a full-scale oscillator. Passband gain 1 at RES 0 |
| FREQ | The SH-101's own slider: exponential, 1.25 octaves per slider unit, 268 Hz singing at the middle. Limits 7 Hz to 11.55 kHz (`SH101_CUT_MIN_HZ`, `SH101_CUT_MAX_HZ`), as TAL-BassLine-101. KBD TRACK, the envelope AMOUNT and the LFO move it in the same knob units as the other types |
| RES | 0..127 to the SH-101 slider 0..10 by `SH101_RES_TAPER` = 2.0 (vs_voice.h): it starts singing near RES 108, and the full range is still at 127. `SH101_RES_MAX` caps the top. 1.0 is the 101's linear slider (singing from RES 92) |
| Singing level | `SH101_SING_TRIM` = 0.7 (vs_voice.h): above its onset the output eases down to −3 dB at full RES (0.4, −8 dB, was too tame). The circuit sings +5.6 dB over a full oscillator, right for the mono 101 but too hot for chords: 3–4 notes at RES 120 ran into the output limiter (heard as crunch and clicks, with DROPPED at 0). Now a 4-note chord at RES 120 peaks at 0.70 before the limiter, near LP24's 0.55. Below the onset nothing changes |
| Coefficients | Worked out at each 16-frame block's end and cached while FREQ and RES are unchanged (no ramp inside a block, as LPDL) |
| Start of oscillation | The circuit's own noise (1e-4 rms at the input), from a fixed seed: the same notes give the same samples |
| CC 49 (TYPE) | The six types are spread over 0..127: CC 127 is now SH101 (it was LPDL) |

## Settings

Type 5's settings are build defines: set them with `.\flash.ps1 -Defs "-DNAME=value"` (Windows) or
`DINGHY_DEFS="-DNAME=value" ./build.sh`, no code to edit. The defaults are in `firmware/src/vs_voice.h`, and the full list is in the README's "Build settings".

| Setting | Default | |
|:---|:---|:---|
| `SH101_SING_TRIM` | `0.7f` | Level when it self-oscillates, eased in above the onset: `1.0f` = the circuit (+5.6 dB over a full oscillator), `0.4f` = −8 dB. Lower for big resonant chords, higher for solo lines |
| `SH101_RES_TAPER` | `2.0f` | RES knob curve: `1.0f` = the 101's linear slider (sings from RES 92), `2.0f` from ~108 |
| `SH101_RES_MAX` | `10.0f` | RES at knob 127 (0..10); below `7.2f` it never self-oscillates |
| `SH101_CUT_MIN_HZ` / `SH101_CUT_MAX_HZ` | `7.0f` / `11550.0f` | FREQ range |

## Host checks

- **Unit tests:** `tests/run_unit.sh` passes.
- **Types 0–4:** the default host script and `tests/scripts/play.txt` render bit-for-bit as before.
- **`tests/scripts/sh101.txt`** plays RES 0/64/100 on one note, a singing FREQ sweep at RES 127, then an 8-note chord at RES 100 on SH101 and on LP24:

| Take | SH101 | LP24 |
|:---|---:|---:|
| RES 0 | −22.6 dB | −20.3 dB |
| RES 64 | −23.1 dB | −26.5 dB |
| RES 100 | −23.5 dB | −27.6 dB |
| RES 127, FREQ sweep | −14.3 dB | −12.2 dB |
| 8-note chord, RES 100 | −11.8 dB (peak 0.91) | −18.5 dB |

The SH101 chord is louder than LP24's because the 101 keeps its bass as resonance rises, while LP24 thins out. It doesn't clip.

## On the FM-1 (to do)

1. **CPU:** the DEBUG page with an 8-voice chord on the INIT patch, on LP24 and then on SH101. In S1Vave this filter cost about 1900 µs per half buffer for 8 voices on one core; here the voices are shared between the two cores.
2. **RES sweep on a held note:** singing should start near 108, with the bass held as it rises.
3. **Self-oscillation (RES 127, oscillators down) across the keyboard with KBD TRACK at full:** it should track and be in tune.
4. **Low FREQ with quiet notes:** no thump, no DC.
5. **A chord at high RES:** no distortion.
6. **Save and load a type 5 patch, then send it through the library page:** it should stay type 5.
