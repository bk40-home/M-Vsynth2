/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 bus: what comes after the voices, in Synth2's measured order (EngineCore/Effects.hpp, Synth.hpp: MIT,
 * Copyright (c) 2026 Francois Chabloz):
 *
 *   voices -> EFFECT -> PAN (the patch's and the LFOs') -> EQ (peak, then TONE) -> DELAY -> CHORUS -> out
 *
 *   EFFECT  10 types (FX ON): 0 a.d.1 (Synth2's fitted model: two asymmetric soft limits, a 95 Hz loop, three
 *           high-passes), 1 a.d.2 (a smooth odd curve), 2 d.d. (a triangle fold), each through an RBJ high-pass and
 *           the RBJ low-pass at 400 Hz x 40^(CTRL2 / 127), Q 3, then x (LEVEL + 2) / 129 (no dry); 3 deci. (sample
 *           and hold every CTRL1 + 1 frames, a quantiser by CTRL2, a 14 kHz one-pole; LEVEL / 128 wet); 4 r.m. (x a
 *           sine, 1 Hz .. 8 kHz; LEVEL / 127 wet); 5 comp. (a detector a side, the leveller (Gmax^-4 + (e /
 *           0.2604)^4)^(-1/4), out -30 .. 0 dB by LEVEL); 6..9 phasers (2 / 4 / 8 / 12 allpasses swept between 2.32
 *           +- H octaves above 254.6 Hz, feedback 0.99 (LEVEL - 64) / 64, out (x + w) / 2; the sweep moves only
 *           while sound flows)
 *   EQ      the peak (LEVEL not 64): Synth2's matched peaking biquad, f0 = 50 Hz x 320^(FREQ / 127), gain (LEVEL -
 *           64) x 50 / 127 dB, the poles' Q = 3.18 x 2^((Q - 64) / 38.16) (x G for a cut), poles and zeros by
 *           impulse invariance, the gain set at f0. Its coefficients are worked out here, in fixed point, when a
 *           knob moves. TONE: the one-pole, always on (64: the original's 22 kHz output low-pass)
 *   DELAY   the time by the tempo (TIME's note value, down to a multiple of 4 frames, at least 0.1 ms), SPREAD one
 *           side later by up to 100 ms, FEEDBACK / 127 a repeat, DRY/WET (both at unity at 64), TONE a one-pole in
 *           the echo path, types ST / X / PP. A new time glides there (tau 0.215 s, read between frames). A program
 *           change fades the echoes out over 15 ms and empties the lines. The lines hold VS_DLY_MS
 *   CHORUS  a sine LFO (free running, also when off), voice delay = base (1 + D sin), 4 types (1: L and R in phase;
 *           2, 3: R a quarter later; 4: four taps crossing), comb feedback 0.99 (FB - 64) / 64, wet LEVEL / 127,
 *           dry at unity. With DELAY on, the echoes reach the output only through the chorus, at unity
 *
 * Not as Synth2: the lines hold 16-bit samples at half scale (+-2 full scale; a noise floor near -84 dB under the
 * echoes); the delay is VS_DLY_MS long (Synth2: 9 s); the phasers' "is sound flowing" test is a control block's.
 * Signals Q24 (1.0 = full scale = 2^24), coefficients Q29 / Q30, phases Q32. Integers only, no 64-bit division. */
#pragma once
#include <stdint.h>
#include "vs_build.h"

#define VS_BUS_N 16u                            /* frames a call (the engine's control block) */
#define VS_DLY_LEN (VS_DLY_MS * 441u / 10u + 4u) /* a delay line, frames */
#define VS_CHO_LEN 3532u                        /* a chorus line: 80 ms + 4 */

typedef struct { int32_t x1, x2, y1, y2; } vs_bq_t;   /* a biquad's direct-form-1 states, Q24 */

typedef struct {
    /* EFFECT */
    vs_bq_t hp[2], lp[2];
    int32_t ad1_fb[2], ad1_h[2][4];
    int32_t ap[2][12], ap_fb[2];
    int32_t hold[2], held[2], dlp[2];
    uint32_t hold_n;
    int32_t env[2];
    uint32_t rm_ph;
    int32_t ph_pos, ph_dir;                     /* the phasers' stage frequency (octaves Q24) and its direction */
    /* EQ */
    int32_t eq_key;                             /* FREQ, LEVEL, Q the peak's coefficients are for (-1: none yet) */
    int32_t eq_b[3], eq_a[2];                   /* b0 b1 b2, a1 a2, Q26 */
    vs_bq_t eq[2];
    int32_t tone_x[2], tone_y[2];               /* EQ TONE, Q24 */
    /* DELAY */
    uint32_t dpos, dwritten;
    int64_t dsmooth[2];                         /* the read times, frames Q24 (-1: none yet) */
    uint32_t d16[2], dtl, dtr;                  /* .. as read (Q16, rounded up); the targets they glide to */
    uint32_t dglide;                            /* the times still gliding (vs_bus.c delay) */
    int32_t dtone_x[2], dtone_y[2];
    uint32_t dfade;                             /* frames left of a clear's fade-out (0: none) */
    int32_t din;                                /* the input's fade-in after a clear, Q30 */
    uint8_t dwas_on;
    /* CHORUS */
    uint32_t cpos, cphase;
    uint8_t cechoes;
} vs_bus_t;

void vs_bus_init(vs_bus_t *b);
void vs_bus_clear(vs_bus_t *b);                 /* a program change: the delay's echoes fade out and the lines empty */
/* a control block: l / r (Q24) in place, from the voices to the output. p: the patch as played (99 values),
 * pan: the pan gains a frame (Q16, the patch's and the LFOs'), bpm_q8: the tempo */
void vs_bus_block(vs_bus_t *b, const int32_t *p, int32_t *l, int32_t *r, const int32_t pan[2][VS_BUS_N], uint32_t n,
                  uint32_t bpm_q8);
