/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 oscillators, fixed point: Synth2's table oscillators (EngineCore/WaveTables.hpp and the oscillator part
 * of Voice.hpp: MIT, Copyright (c) 2026 Francois Chabloz), on the tables of firmware/gen/vs_wave.h.
 *
 *   tables   band-limited saw (19, one per 6 semitones) and triangle (6, one per 24), a sine; 2048 points, Q14.
 *            They are read from RAM: vs_osc_init copies them into the platform's pool (107 KB). In flash a
 *            chord's tables do not fit the chip's 32 KB cache, and a voice then costs many times more (the gate's
 *            measurement on the FM-1, proposal 3.5)
 *   a read   linear between two points: 11 bits of index, 15 of fraction (Synth2: a cubic)
 *   shapes   SINE, SAW, TRIANGLE: one read. PULSE: (saw(p) - saw(p + w)) / 2, w the width as a phase. NOISE: the
 *            original's 1-bit sequence s[n] = s[n-13] ^ s[n-18] ^ s[n-19] ^ s[n-20], played as +-VS_NOISE_Q14
 *   pitch    in cents from MIDI note 0 (note x 100 + ...): vs_pitch_inc gives the phase step, f x 2^32 / 44100, from
 *            the top octave's steps and 2^x (firmware/gen/vs_tables.h); vs_pitch_steps moves it by whole 1/128
 *            octaves (the bend's and the LFOs' steps), exactly
 * Phase is Q32 (one cycle = 2^32, wraps by itself); output Q14 (1.0 = 16384; the saw's overshoot reaches 1.18).
 * Integers only. render functions write n frames (=, not +=) and run in the audio ISR. */
#pragma once
#include <stdint.h>

enum { VS_SH_SINE, VS_SH_SAW, VS_SH_PULSE, VS_SH_TRI, VS_SH_NOISE };   /* what vs_osc_render draws */
#define VS_PITCH_MAX 13199                      /* cents: the top of note 131 (the tables' last note) */

/* an interpolated read of a table: 11 bits of index, 15 of fraction, ROUNDED (not floored: under FM a floor's
 * -1/2 LSB on average is a steady pitch error in OSC1, and its phase walks away; the cross path of vs_voice.c) */
static inline int32_t vs_osc_rd(const int16_t *t, uint32_t p)
{
    uint32_t i = p >> 21;
    int32_t a = t[i], b = t[i + 1u];
    return a + (((b - a) * (int32_t)((p >> 6) & 32767u) + 16384) >> 15);
}

void vs_osc_init(void);                         /* once, before the first render: the tables into RAM */
/* the table a shape reads at a note (0..131; above: the last): the saw's for SAW and PULSE, the triangle's, the sine */
const int16_t *vs_osc_table(uint32_t shape, uint32_t note);
uint32_t vs_pitch_inc(int32_t cents);           /* held to 0..VS_PITCH_MAX */
uint32_t vs_pitch_steps(uint32_t inc, int32_t steps);   /* inc x 2^(steps / 128), held under half the sample rate */
/* n frames of a shape from *ph on, stepping inc; width: PULSE's (Q32). table: vs_osc_table's (not used by NOISE,
 * which runs on *noise, its 20-bit state, and leaves *ph alone but for the stepping) */
void vs_osc_render(int32_t *out, uint32_t n, uint32_t shape, const int16_t *table, uint32_t *ph, uint32_t inc,
                   uint32_t width, uint32_t *noise);
/* the same with a step (incv) and a pulse width (wv) a frame, either 0 for the constant (an LFO or the mod env on
 * pitch or P/W, nothing else crossing the oscillators) */
void vs_osc_render_mod(int32_t *out, uint32_t n, uint32_t shape, const int16_t *table, uint32_t *ph, const uint32_t *incv,
                       uint32_t inc, const uint32_t *wv, uint32_t width, uint32_t *noise);
