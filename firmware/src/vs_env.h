/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 envelopes, fixed point: Synth2's Adsr (EngineCore/Envelope.hpp: MIT, Copyright (c) 2026 Francois
 * Chabloz), as measured on the original's amp envelope.
 *
 *   attack   linear to 1.0, a step a frame (VS_ATK_STEP_Q30)
 *   decay    exponential toward the sustain level; release exponential toward 0; one time-constant law for both
 *            (VS_ENV_C16_Q31: the coefficient of a 16-frame block, which is 16 frames of the per-frame one exactly)
 *   landing  PLAIN (the filter env, the mod env): they aim 1/2048 below their target and stop on it, so they
 *            land in finite time. QUANTIZED (the amp env): plain exponentials that stop within 1/2048 of the
 *            target, where the voice's 11-bit gain lands on it (vs_voice.c)
 *   sustain  the amp env's follows the measured curve (VS_AMP_SUSTAIN_Q11), the filter env's is linear, S / 127
 * Runs once a control block of 16 frames. The attack's end is found to the frame: the step that would pass 1.0
 * says how many frames it took (vs_env_step's return), so a voice can bend its gain ramp there, and the decay
 * runs for the rest of the block. The landings are found to the block. Level Q30. Integers only. */
#pragma once
#include <stdint.h>

enum { VS_ENV_IDLE, VS_ENV_ATTACK, VS_ENV_DECAY, VS_ENV_RELEASE };

typedef struct {
    uint32_t atk;                               /* attack step a frame, Q30 */
    uint32_t dec, rel;                          /* decay / release coefficient a block, Q31 */
    uint32_t dec1;                              /* decay coefficient a frame, Q31: the frames left after the attack lands */
    int32_t sus;                                /* sustain level, Q30 */
    uint8_t quant;                              /* 1: the amp env's landing */
} vs_env_cfg_t;

typedef struct {
    int32_t level;                              /* Q30 */
    uint8_t stage;                              /* VS_ENV_* */
} vs_env_t;

/* knobs 0..127; amp_sustain: the curved sustain (else S / 127) */
void vs_env_config(vs_env_cfg_t *c, uint32_t a, uint32_t d, uint32_t s, uint32_t r, int amp_sustain, int quant);
static inline void vs_env_trigger(vs_env_t *e) { e->stage = VS_ENV_ATTACK; }   /* from where it is: no jump */
static inline void vs_env_release(vs_env_t *e) { if (e->stage != VS_ENV_IDLE) e->stage = VS_ENV_RELEASE; }
static inline void vs_env_reset(vs_env_t *e) { e->stage = VS_ENV_IDLE; e->level = 0; }
/* one block of 16 frames. Returns 16, or, when the attack reached 1.0 inside the block, the frames it took
 * (1..16): the stage is then DECAY, and the level has decayed for the frames left of the block */
uint32_t vs_env_step(vs_env_t *e, const vs_env_cfg_t *c);
