/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 envelopes (vs_env.h). Built in vs_dsp.c at -O2. */
#include "vs_env.h"
#include "vs_tables.h"

#define ENV_ONE (1 << 30)
#define ENV_AIM (ENV_ONE / 2048)                /* 1/2048: the 11-bit gain's last step */

void vs_env_config(vs_env_cfg_t *c, uint32_t a, uint32_t d, uint32_t s, uint32_t r, int amp_sustain, int quant)
{
    c->atk = VS_ATK_STEP_Q30[a & 127u];
    c->dec = VS_ENV_C16_Q31[d & 127u];
    c->dec1 = VS_ENV_C1_Q31[d & 127u];
    c->rel = VS_ENV_C16_Q31[r & 127u];
    s &= 127u;
    c->sus = amp_sustain ? (int32_t)VS_AMP_SUSTAIN_Q11[s] << 19 : (int32_t)(((uint32_t)ENV_ONE / 127u) * s + (s == 127u ? ENV_ONE % 127 : 0));
    c->quant = (uint8_t)(quant != 0);
}

uint32_t vs_env_step(vs_env_t *e, const vs_env_cfg_t *c)
{
    int32_t aim = c->quant ? 0 : ENV_AIM, stop = c->quant ? ENV_AIM : 0;
    switch (e->stage) {
    case VS_ENV_ATTACK: {
        uint32_t left = (uint32_t)(ENV_ONE - e->level), need = c->atk * 16u;
        if (c->atk >= (1u << 26) || need >= left) {      /* it lands in this block (the first test: no overflow) */
            uint32_t k = (left + c->atk - 1u) / c->atk, j;
            k = k < 1u ? 1u : k > 16u ? 16u : k;
            e->level = ENV_ONE;
            e->stage = VS_ENV_DECAY;
            for (j = k; j < 16u; j++) {         /* the frames left of the block decay, a frame at a time */
                e->level += (int32_t)(((int64_t)(c->sus - aim - e->level) * (int64_t)c->dec1) >> 31);
                if (e->level <= c->sus + stop) e->level = c->sus;
            }
            return k;
        }
        e->level += (int32_t)need;
        break;
    }
    case VS_ENV_DECAY:
        e->level += (int32_t)(((int64_t)(c->sus - aim - e->level) * (int64_t)c->dec) >> 31);
        if (e->level <= c->sus + stop) e->level = c->sus;
        break;
    case VS_ENV_RELEASE:
        e->level += (int32_t)(((int64_t)(-aim - e->level) * (int64_t)c->rel) >> 31);
        if (e->level <= stop) vs_env_reset(e);
        break;
    default:
        break;
    }
    return 16u;
}
