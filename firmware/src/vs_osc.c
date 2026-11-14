/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 oscillators (vs_osc.h). Built in vs_dsp.c at -O2. */
#include "vs_osc.h"
#include "vs_wave.h"
#include "vs_tables.h"

/* the tables in RAM (the platform's pool: zeroed at boot, filled by vs_osc_init) */
static int16_t ram_saw[VS_WAVE_NT][VS_WAVE_N + 1u] __attribute__((section(".pool")));
static int16_t ram_tri[VS_TRI_NT][VS_WAVE_N + 1u] __attribute__((section(".pool")));
static int16_t ram_sine[VS_WAVE_N + 1u] __attribute__((section(".pool")));

void vs_osc_init(void)
{
    uint32_t t, k;
    for (t = 0; t < VS_WAVE_NT; t++)
        for (k = 0; k <= VS_WAVE_N; k++) ram_saw[t][k] = VS_SAW[t][k];
    for (t = 0; t < VS_TRI_NT; t++)
        for (k = 0; k <= VS_WAVE_N; k++) ram_tri[t][k] = VS_TRI[t][k];
    for (k = 0; k <= VS_WAVE_N; k++) ram_sine[k] = VS_SINE[k];
}

const int16_t *vs_osc_table(uint32_t shape, uint32_t note)
{
    if (shape == VS_SH_SINE) return ram_sine;
    if (shape == VS_SH_TRI) return ram_tri[vs_tri_of(note)];
    return ram_saw[vs_wave_of(note)];
}

uint32_t vs_pitch_inc(int32_t cents)
{
    uint32_t note, rem, x, i, r, inc;
    if (cents < 0) cents = 0;
    if (cents > VS_PITCH_MAX) cents = VS_PITCH_MAX;
    note = (uint32_t)cents / 100u;
    rem = (uint32_t)cents % 100u;
    inc = VS_NOTE_INC_Q32[note % 12u] >> (10u - note / 12u);
    if (!rem) return inc;
    x = rem * 16777216u / 1200u;                /* the cents as a fraction of an octave, Q24: under 1/12 */
    i = x >> 16;
    r = VS_EXP2_Q30[i] + (uint32_t)(((uint64_t)(VS_EXP2_Q30[i + 1u] - VS_EXP2_Q30[i]) * (x & 65535u)) >> 16);
    return (uint32_t)(((uint64_t)inc * r) >> 30);
}

uint32_t vs_pitch_steps(uint32_t inc, int32_t steps)
{
    int32_t oct = steps >= 0 ? steps / 128 : -((127 - steps) / 128);   /* floor */
    uint32_t rem = (uint32_t)(steps - oct * 128);                       /* 0..127 */
    uint64_t v = ((uint64_t)inc * VS_EXP2_Q30[rem * 2u]) >> 30;         /* (a whole step is a table entry: exact) */
    if (oct >= 0) v = oct > 8 ? 0x7FFFFFFFu : v << oct;
    else v = oct < -31 ? 0u : v >> -oct;
    return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)v;
}

#define rd vs_osc_rd

void vs_osc_render_mod(int32_t *out, uint32_t n, uint32_t shape, const int16_t *table, uint32_t *ph, const uint32_t *incv,
                       uint32_t inc, const uint32_t *wv, uint32_t width, uint32_t *noise)
{
    uint32_t p = *ph, i;
    if (!incv && !wv) { vs_osc_render(out, n, shape, table, ph, inc, width, noise); return; }
    if (shape == VS_SH_NOISE) {
        for (i = 0; i < n; i++) p += incv ? incv[i] : inc;
        vs_osc_render(out, n, shape, table, ph, 0u, width, noise);
        *ph = p;
        return;
    }
    for (i = 0; i < n; i++) {
        const uint32_t w = wv ? wv[i] : width;
        out[i] = shape == VS_SH_PULSE ? (rd(table, p) - rd(table, p + w)) >> 1 : rd(table, p);
        p += incv ? incv[i] : inc;
    }
    *ph = p;
}

void vs_osc_render(int32_t *out, uint32_t n, uint32_t shape, const int16_t *table, uint32_t *ph, uint32_t inc,
                   uint32_t width, uint32_t *noise)
{
    uint32_t p = *ph, i;
    switch (shape) {
    case VS_SH_PULSE:
        for (i = 0; i < n; i++) { out[i] = (rd(table, p) - rd(table, p + width)) >> 1; p += inc; }
        break;
    case VS_SH_NOISE: {
        uint32_t s = *noise;
        for (i = 0; i < n; i++) {
            uint32_t bit = (s >> 12 ^ s >> 17 ^ s >> 18 ^ s >> 19) & 1u;
            s = (s << 1 | bit) & 0xFFFFFu;
            out[i] = bit ? VS_NOISE_Q14 : -VS_NOISE_Q14;
        }
        *noise = s;
        p += inc * n;
        break;
    }
    default:                                    /* SINE, SAW, TRIANGLE: the table says which */
        for (i = 0; i < n; i++) { out[i] = rd(table, p); p += inc; }
        break;
    }
    *ph = p;
}
