/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 LFOs, fixed point: Synth2's Lfo (EngineCore/Lfo.hpp: MIT, Copyright (c) 2026 Francois Chabloz), as
 * measured on Synth1.
 *
 *   two, global (not per voice), free running from power-on, bipolar +-1; KEY SYNC restarts one on the first note
 *   of a phrase (a note on with no key held)
 *   WAVE     0 saw (falling, +1 at phase 0), 1 triangle (-1 at phase 0), 2 square (+1 the first half), 3 S&H,
 *            4 smooth random, 5 sine, 6 ramp (rising saw, -1 at phase 0: M-Vsynth2's own, not in Synth1/.sy1)
 *   random   one value a cycle, r (a xorshift from a fixed seed): S&H plays r; smooth ramps linearly from the last
 *            cycle's r to r. Routed to AMP, smooth ramps from -r to minus the last r instead. A restart draws a new
 *            r as a cycle's start does
 *   SPEED    Hz = 2^(speed / 12.0367) / 12 (12 s a cycle at 0); TEMPO on: a cycle of SPEED x 30 / 128's note value
 *            (16 bars .. 1/64T) at the tempo, not locked to the beat
 *   on pitch an LFO plays whole 1/128-octave steps of its own, N = VS_LFO_STEPS[DEPTH] at full swing, by its WAVE
 *            (v its value): saw floor((v + 1) / 2 (2N + 1)) - N held to N; triangle floor(N v) rising, ceil(N v)
 *            falling; square +-N; S&H and smooth floor(N v); sine sign(v) floor(N |v| + 1/4); ramp floor((v + 1) /
 *            2 (2N + 1)) - N held to N (saw's mirror)
 * Value Q30, phase Q32. Integers only. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t ph;                                /* phase, Q32 */
    uint32_t st;                                /* the random values' xorshift state */
    int32_t prev, cur;                          /* the last cycle's random value and this one's, Q30 */
} vs_lfo_t;

void vs_lfo_reset(vs_lfo_t *l);                 /* power-on: phase 0, the seed, two values drawn */
void vs_lfo_restart(vs_lfo_t *l);               /* KEY SYNC: phase 0, a new value drawn */
/* the phase step a frame: SPEED, or with TEMPO on SPEED's note value at bpm_q8 (beats a minute, Q8) */
uint32_t vs_lfo_inc(uint32_t speed, int tempo, uint32_t bpm_q8);
/* n frames: out (Q30); steps (when not 0) its pitch steps for N = VS_LFO_STEPS[DEPTH]; amp: routed to AMP */
void vs_lfo_render(vs_lfo_t *l, uint32_t n, uint32_t wave, uint32_t inc, int32_t *out, int32_t *steps, int32_t nsteps,
                   int amp);
