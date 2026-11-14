/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 LFOs (vs_lfo.h). Built in vs_dsp.c at -O2. */
#include "vs_lfo.h"
#include "vs_tables.h"

#define ONE30 (1 << 30)
#define LFO_SEED 0x2545F491u                    /* Lfo::kSeed */

static int32_t draw(vs_lfo_t *l)                /* the next random value: state x 2 / 2^32 - 1, Q30 */
{
    uint32_t s = l->st;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    l->st = s;
    return (int32_t)(s >> 1) - ONE30;
}
static void next(vs_lfo_t *l) { l->prev = l->cur; l->cur = draw(l); }

void vs_lfo_reset(vs_lfo_t *l)
{
    l->ph = 0;
    l->st = LFO_SEED;
    l->prev = draw(l);
    l->cur = draw(l);
}
void vs_lfo_restart(vs_lfo_t *l) { l->ph = 0; next(l); }

uint32_t vs_lfo_inc(uint32_t speed, int tempo, uint32_t bpm_q8)
{
    speed &= 127u;
    if (!tempo) return VS_LFO_INC_Q32[speed];
    return (uint32_t)(((uint64_t)bpm_q8 * VS_LFO_BEAT_Q48[speed * 30u / 128u]) >> 24);
}

void vs_lfo_render(vs_lfo_t *l, uint32_t n, uint32_t wave, uint32_t inc, int32_t *out, int32_t *steps, int32_t nsteps,
                   int amp)
{
    uint32_t i, ph = l->ph;
    const int64_t sn = nsteps;
    for (i = 0; i < n; i++) {
        int32_t v;
        uint32_t np;
        switch (wave) {
        case 0: v = ONE30 - (int32_t)(ph >> 1); break;                              /* 1 - 2p */
        case 1: v = ph < 0x80000000u ? (int32_t)ph - ONE30 : (int32_t)(((int64_t)3 << 30) - ph); break;   /* -1 + 4p, 3 - 4p */
        case 2: v = ph < 0x80000000u ? ONE30 : -ONE30; break;
        case 3: v = l->cur; break;
        case 4: {
            int32_t r = (int32_t)(((int64_t)(amp ? l->prev - l->cur : l->cur - l->prev) * (ph >> 1)) >> 31);   /* (|diff| up to 2.0) */
            v = amp ? -(l->cur + r) : l->prev + r;
            break;
        }
        default: {                              /* sin(2 pi p) */
            uint32_t j = ph >> 22, fr = (ph >> 6) & 65535u;
            v = VS_SIN_Q30[j] + (int32_t)(((int64_t)(VS_SIN_Q30[j + 1u] - VS_SIN_Q30[j]) * fr) >> 16);
            break;
        }
        }
        out[i] = v;
        if (steps) {
            int64_t x = sn * v, s;              /* N v, Q30 */
            switch (wave) {
            case 0:                             /* floor((1 - p) (2N + 1)) - N, held to N */
                s = (int64_t)((((uint64_t)1 << 32) - ph) * (uint64_t)(2 * sn + 1) >> 32) - sn;
                if (s > sn) s = sn;
                break;
            case 1: s = ph < 0x80000000u ? x >> 30 : -((-x) >> 30); break;          /* rising floor, falling ceil */
            case 2: s = v > 0 ? sn : -sn; break;
            case 3:
            case 4: s = x >> 30; break;
            default: s = x >= 0 ? (x + (1 << 28)) >> 30 : -((-x + (1 << 28)) >> 30); break;   /* sign x floor(|N v| + 1/4) */
            }
            steps[i] = (int32_t)s;
        }
        np = ph + inc;
        if (np < ph) next(l);                   /* a cycle's end: the next random value */
        ph = np;
    }
    l->ph = ph;
}
