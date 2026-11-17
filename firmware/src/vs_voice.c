/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 voice engine (vs_voice.h), Stage 4a. Built in vs_dsp.c at -O2.
 * Ported from Synth2's EngineCore/Synth.hpp, Voice.hpp and Filter.hpp (MIT, Copyright (c) 2026 Francois Chabloz). */
#include "vs_voice.h"
#include "vs_osc.h"
#include "vs_tables.h"
#include "vs_bus_tables.h"
#include "vs_fixed.h"

#define ONE30 (1 << 30)
#define GRID 256u                               /* the amp gain's and the voice end's grid (frames) */
#define Q11_MASK (~((ONE30 / 2048) - 1))        /* a Q30 level floored to 1/2048 */
#define KNOB(x) ((int32_t)(x) * 65536)          /* knob units, Q16 */
#define STATE_MAX (1 << 29)                     /* the SVF's states are held here (512 x full scale): no wrap */
/* the limiter's follower in 32 bits: its coefficients as Q16 (the Q31 ones of vs_tables.h, rounded) */
#define LIMIT_ATK_Q16 ((int32_t)((VS_LIMIT_ATK_Q31 + 16384u) >> 15))
#define LIMIT_REL_Q16 ((int32_t)((VS_LIMIT_REL_Q31 + 16384u) >> 15))

enum { EV_ON = 1, EV_OFF, EV_PEDAL, EV_ALLOFF };

/* ---- events: the main loop's side */
static void ev_put(vs_engine_t *e, uint32_t word)
{
    uint32_t w = e->ev_w, next = (w + 1u) & (VS_EVQ - 1u);
    if (next == e->ev_r) return;                /* full: dropped (64 events in 0.36 ms) */
    e->evq[w] = word;
    e->ev_w = next;
}
void vs_engine_note_on(vs_engine_t *e, uint32_t note, uint32_t vel)
{
    if (!vel) { vs_engine_note_off(e, note); return; }
    ev_put(e, (uint32_t)EV_ON << 16 | (note & 127u) << 8 | (vel > 127u ? 127u : vel));
}
void vs_engine_note_off(vs_engine_t *e, uint32_t note) { ev_put(e, (uint32_t)EV_OFF << 16 | (note & 127u) << 8); }
void vs_engine_sustain(vs_engine_t *e, int down) { ev_put(e, (uint32_t)EV_PEDAL << 16 | (down ? 1u : 0u)); }
void vs_engine_all_off(vs_engine_t *e) { ev_put(e, (uint32_t)EV_ALLOFF << 16); }

/* the key LEDs (main loop: a read of the voices, no lock; a stale bit only lights a key one frame late): on, a bit a
 * note, a voice with its gate open (a key, MIDI, the arp); rel, a voice sounding on in its release */
void vs_engine_lit(const vs_engine_t *e, uint32_t on[4], uint32_t rel[4])
{
    uint32_t i;
    for (i = 0; i < 4u; i++) on[i] = rel[i] = 0;
    for (i = 0; i < VS_NV; i++) {
        const vs_voice_t *v = &e->v[i];
        if (!v->active) continue;
        if (v->gate) on[v->note >> 5 & 3u] |= 1u << (v->note & 31u);
        else rel[v->note >> 5 & 3u] |= 1u << (v->note & 31u);
    }
}

int vs_engine_load(vs_engine_t *e, const vs_patch_t *p)
{
    if (e->load_req) return -1;
    e->pending = *p;
    vs_patch_sanitize(&e->pending);
    e->load_req = 1;
    return 0;
}

static void routes_apply(vs_engine_t *e);
#ifndef VS_TICKS
#define VS_TICKS() 0u                           /* (no timer: the tests, the host build) */
#endif
#ifndef VS_SHED_TICKS
#define VS_SHED_TICKS(n) 0u                     /* the overload guard's budget for n frames (vs_dsp.c); 0: none */
#endif
static void sh_reset(vs_voice_t *v)             /* SH-101's states cleared, its circuit noise from a fixed seed (the same
                                                   notes twice give the same samples) */
{
#if VS_SH101
    sh101_reset(&v->shs, 0x2545F491u);
#else
    (void)v;
#endif
}

static void diode_reset(vs_voice_t *v)          /* LPDL's states cleared, its coefficients due */
{
#if VS_LPDL
    uint32_t i;
    for (i = 0; i < 5u; i++) v->dd[i] = 0;
    v->dx = 0; v->dres = -1;
#else
    (void)v;
#endif
}

void vs_engine_init(vs_engine_t *e)
{
    uint32_t i, k;
    vs_osc_init();
#if VS_SH101
    sh101_init();                               /* SH-101's diode and resonance tables (float, once) */
#endif
    vs_patch_init(&e->patch);
    e->bend14 = 0;
    e->nv = VS_NV;
    e->ev_w = e->ev_r = 0;
    e->load_req = 0;
    e->last = -1;
    e->now = 0;
    e->bpos = 0;
    e->cut = e->cut_tgt = KNOB(e->patch.v[VS_P_FLT_FREQ]);
    e->cut_rate = 0;
    e->pedal = 0;
    for (i = 0; i < 128u; i++) e->cc[i] = 0;
    e->pressure = 0;
    e->nkeys = 0;
    e->arp_n = 0; e->arp_play = -1; e->arp_on = 0; e->arp_due = 0; e->arp_acc = 0; e->arp_rng = 0x9E3779B9u;
    e->act = 0;
    vs_bus_init(&e->bus);
    vs_lim_init(&e->lim);
    e->limit = 1;
    vs_lfo_reset(&e->lfo[0]); vs_lfo_reset(&e->lfo[1]);
    e->bpm_q8 = 120u << 8;
    for (i = 0; i < 4u; i++) e->down[i] = e->held[i] = 0;
    for (i = 0; i < VS_NV; i++) {
        vs_voice_t *v = &e->v[i];
        v->p1 = VS_SLOT_OSC1_Q32[i % VS_NSLOT];   /* a slot's oscillators carry on from here (OSC PHASE 0) */
        v->p2 = VS_SLOT_OSC2_Q32[i % VS_NSLOT];
        v->ps = 0;                              /* (Synth2: 0 until a note locks it) */
        for (k = 0; k < 8u; k++) v->cs[k] = VS_SLOT_COPY_Q32[(i % VS_NSLOT) * 8u + k];
        v->chalf = 0; v->cdet = 0; v->cinc_for = 0xFFFFFFFFu;
        v->inc1 = v->inc2 = v->inc1b = v->inc2b = 0;
        v->uc8 = v->upan = 0; v->usemis = 0; v->uk = 0; v->ush = 0;
        v->pitch_key[0] = v->pitch_key[1] = v->pitch_key[2] = v->pitch_key[3] = 0xFFFFFFFFu;
        v->gr = v->gaim = 0;                    /* (a never-used voice glides from note 0) */
        v->tn1 = v->tn2 = 0; v->mute1 = v->mute2 = 0;
        v->noise = VS_NOISE_SEED;
        vs_env_reset(&v->aenv); vs_env_reset(&v->fenv); vs_env_reset(&v->menv);
        v->ag = v->ag_step = 0; v->a_left = 0; v->a_since = 0; v->a_stage = VS_ENV_IDLE;
        v->low = v->band = v->lenv = 0; v->f = -1;
        v->la = -1; v->lk = 0; v->lad[0] = v->lad[1] = v->lad[2] = v->lad[3] = v->lfb = 0; v->agc = 0; v->agg = 65536; diode_reset(v);
        sh_reset(v);
        v->vel_g = 32768; v->vel_key = 0xFFFFu;
        v->note = v->vel = v->gate = v->active = 0;
        v->age = 0;
    }
    routes_apply(e);
}

/* ---- the LFOs' modulation of a control block, frame by frame, for all voices (Voice.hpp LfoRoute): both LFOs on
 * one destination add (pitch: their steps; cutoff, FM, P/W, pan: the depth-weighted sum of their values; amp: the
 * product of their gains) */
typedef struct {
    int32_t sa[VS_CTL], s2[VS_CTL];             /* pitch steps: OSC1+2's (both oscillators); OSC2's alone */
    int32_t pw[VS_CTL];                         /* P/W: sum of depth / 127 x v, Q28 */
    int32_t fm[VS_CTL];                         /* FM: sum of depth / 127 x (v + 1) / 2, Q28 */
    int32_t cut;                                /* FILTER: the same, at the block's last frame, held to 1, Q28 */
    int32_t amp[VS_CTL];                        /* AMP: product of 1 - depth / 127 x (1 - v) / 2, Q15 */
} lfo_mod_t;

/* ---- what a control block needs from the patch, worked out once for all voices */
typedef struct {
    uint32_t sh1, sh2;                          /* VS_SH_* */
    uint32_t width;                             /* PULSE's width, Q32 */
    int32_t mix1, mix2;                         /* OSC1's (with the SUB in it) and OSC2's weights in the mix, Q15 */
    uint32_t sub_on, sub_sh, sub_oct;           /* the SUB: heard; VS_SH_*; an octave down */
    int32_t sub_w, main_w;                      /* its weight and OSC1's beside it, Q15 */
    uint32_t det;                               /* DETUNE, 0..127 (0: no copies) */
    uint32_t cross, ring, sync, ring_full;      /* the cross path; RING; SYNC; RING with an FM source in the patch */
    uint32_t fm_dev;                            /* FM's deviation a frame at OSC2 = 1.0, Q32 (0: none) */
    uint32_t menv_on, menv_dest;                /* M.ENV on; its destination (0 OSC2, 1 FM, 2 P/W) */
    int32_t menv_amt;                           /* AMOUNT as -1..+1, Q15 */
    int32_t menv_oct;                           /* OSC2's octaves at level 1 (5 x AMOUNT), Q28: Q15 is too coarse for a pitch */
    vs_env_cfg_t mcfg;
    uint32_t pw_raw;                            /* P/W / 254 before its limits, Q32 (the mod env moves it from here) */
    uint32_t fm_knob;                           /* FM, 0..127 */
    int32_t dgain;                              /* a detuned stack's gain, Q15 */
    uint32_t ftype;                             /* 0 LP, 2 HP, 3 BP (1 and 4 play as 0 until Stage 4c) */
    int32_t damp, gin;                          /* SVF damping 1 - r and input gain 1 - 0.7 r, Q15 */
    int32_t lad_kr;                             /* LP24: max(0, 0.0297 res - 0.076), Q16 */
    uint32_t sat_num;                           /* SATURATION: (1 + a) << 9, 0 = off */
    int32_t sat_a;                              /* a, Q16 */
    vs_env_cfg_t acfg, fcfg;
    int32_t gain;                               /* 0.2 x GAIN, Q16 */
    uint32_t sens;                              /* AMP VEL */
    int32_t kbd;                                /* KBD TRACK: knob units a semitone, Q16 */
    int32_t depth;                              /* the filter env's swing at env 1, knob units Q16 (before velocity) */
    uint32_t velsw;
    int32_t shift_c, tune_c, o2_c;              /* cents: KEY SHIFT + TUNE; TUNE; OSC2's PITCH + FINE */
    uint32_t track;
    int32_t bend_steps, bend_c;                 /* the bend: whole 1/128 octaves; cents (for the table) */
    uint32_t phase;                             /* OSC PHASE */
    uint32_t porta, porta_a;                    /* PORTAMENTO on; its 16-frame coefficient a, Q31 */
    uint32_t lfo_dest[2];                       /* LFO k's destination 1..7 (OSC2 OSC1+2 FILTER AMP P/W FM PAN), 0: none */
    int32_t lfo_sc[2];                          /* its DEPTH / 127, Q16 */
    uint32_t r_p1, r_p2, r_cut, r_amp, r_pw, r_fm, r_pan;   /* an LFO on: OSC1's pitch, OSC2's, FILTER, AMP, P/W, FM, PAN */
    const lfo_mod_t *lm;                        /* the LFOs' values this block (vs_engine_render's) */
} blk_t;

static void blk_make(const vs_engine_t *e, blk_t *b)
{
    const int32_t *p = e->ev;
    static const uint8_t SH2[4] = {VS_SH_SAW, VS_SH_PULSE, VS_SH_TRI, VS_SH_NOISE};
    int32_t res = vs_clamp(p[VS_P_FLT_RES], 0, 127), pw = vs_clamp(p[VS_P_OSC_PW], 0, 127), bend, range;
    uint32_t w, a;
    b->sh1 = (uint32_t)vs_clamp(p[VS_P_OSC1_SHAPE], 0, 3);          /* SINE SAW PULSE TRI: VS_SH_* in that order */
    b->sh2 = SH2[vs_clamp(p[VS_P_OSC2_SHAPE], 1, 4) - 1];
    w = (uint32_t)pw * 16909320u + (uint32_t)pw / 2u;                /* pw / 254 of a cycle */
    b->width = w < 21136651u ? 21136651u : w > 4271716980u ? 4271716980u : w;   /* 1.25 / 254 .. 1 - 1.375 / 254 */
    {   /* the mix; with the SUB on, OSC2 takes OSC1's weight too, and the sum is scaled back to unity */
        static const uint8_t SUB_SH[4] = {VS_SH_SINE, VS_SH_TRI, VS_SH_SAW, VS_SH_PULSE};
        int32_t m2 = (vs_clamp(p[VS_P_OSC_MIX], 0, 127) * 32768 + 63) / 127, g = vs_clamp(p[VS_P_SUB_GAIN], 0, 127), den;
        b->sub_on = g > 0;
        b->sub_sh = SUB_SH[vs_clamp(p[VS_P_SUB_SHAPE], 0, 3)];
        b->sub_oct = p[VS_P_SUB_OCT] != 0;
        b->sub_w = (g * 32768 + (g + 32) / 2) / (g + 32);
        b->main_w = 32768 - b->sub_w;
        den = 32768 - ((m2 * b->sub_w) >> 15);
        b->mix1 = ((32768 - m2) * 32768 + den / 2) / den;
        b->mix2 = (((m2 * b->main_w) >> 15) * 32768 + den / 2) / den;
    }
    b->ftype = (uint32_t)vs_clamp(p[VS_P_FLT_TYPE], 0, 5);
#if !VS_LPDL
    if (b->ftype == 4u) b->ftype = 0u;          /* (built without LPDL: it plays as LP12) */
#endif
#if !VS_SH101
    if (b->ftype == 5u) b->ftype = 1u;          /* (built without SH-101: it plays as LP24) */
#endif
    /* LP24's k before its compensation: max(0, 0.0297 res - 0.076), Q16 (from Q26: 0.0297 and 0.076 x 2^26) */
    b->lad_kr = res * 1993097 > 5100274 ? (res * 1993097 - 5100274) >> 10 : 0;
    b->damp = ((127 - res) * 32768 + 63) / 127;
    b->gin = 32768 - (res * 22938 + 63) / 127;                      /* 0.7 x 32768 = 22938 */
    a = VS_SAT_A_Q16[vs_clamp(p[VS_P_FLT_SAT], 0, 127)];
    b->sat_a = (int32_t)a;
    b->sat_num = a ? (65536u + a) << 9 : 0u;
    vs_env_config(&b->acfg, (uint32_t)p[VS_P_AENV_A], (uint32_t)p[VS_P_AENV_D], (uint32_t)p[VS_P_AENV_S], (uint32_t)p[VS_P_AENV_R], 1, 1);
    vs_env_config(&b->fcfg, (uint32_t)p[VS_P_FENV_A], (uint32_t)p[VS_P_FENV_D], (uint32_t)p[VS_P_FENV_S], (uint32_t)p[VS_P_FENV_R], 0, 0);
    b->gain = (int32_t)((VS_OSC_AMP_Q15 * (uint32_t)VS_AMP_GAIN_Q12[vs_clamp(p[VS_P_AMP_GAIN], 0, 127)]) >> 11);
    b->sens = (uint32_t)vs_clamp(p[VS_P_AMP_VEL], 0, 127);
    b->kbd = (int32_t)VS_KBD_TRACK_Q16[vs_clamp(p[VS_P_FLT_TRACK], 0, 127)];
    b->depth = (vs_clamp(p[VS_P_FLT_AMOUNT], 0, 127) - 64) * 130023;   /* x 1.984 knob units, Q16 */
    b->velsw = p[VS_P_FLT_VELSW] != 0;
    b->tune_c = VS_FINE_CENTS[vs_clamp(p[VS_P_OSC_TUNE], 0, 127)];
    b->shift_c = vs_clamp(p[VS_P_KEY_SHIFT], -24, 24) * 100 + b->tune_c;
    b->o2_c = VS_OSC2_SEMIS[vs_clamp(p[VS_P_OSC2_PITCH], 0, 127)] * 100 + VS_FINE_CENTS[vs_clamp(p[VS_P_OSC2_FINE], 0, 127)];
    b->track = p[VS_P_OSC2_TRACK] != 0;
    /* only the bend's top 7 bits count: trunc(v / 128) held to +-63, / 63 = the full range (Synth::Render) */
    bend = vs_clamp(e->bend14 / 128, -63, 63);
    range = vs_clamp(p[VS_P_BEND_RANGE], 0, 24);
    b->bend_steps = bend * range * 32 / 189;                        /* trunc(semis x 100 / 9.375) */
    b->bend_c = bend * range * 100 / 63;
    b->phase = (uint32_t)vs_clamp(p[VS_P_OSC_PHASE], 0, 127);
    b->porta = p[VS_P_PORTA_TIME] > 0;
    b->porta_a = VS_PORTA_A_Q31[vs_clamp(p[VS_P_PORTA_TIME], 0, 127)];
    b->det = (uint32_t)vs_clamp(p[VS_P_OSC1_DETUNE], 0, 127);
    b->ring = p[VS_P_OSC2_RING] != 0;
    b->sync = p[VS_P_OSC2_SYNC] != 0;
    b->fm_dev = VS_FM_DEV_Q32[vs_clamp(p[VS_P_OSC1_FM], 0, 127)];
    b->ring_full = p[VS_P_OSC1_FM] > 0;
    b->menv_on = p[VS_P_MENV_ON] != 0;
    b->menv_dest = (uint32_t)vs_clamp(p[VS_P_MENV_DEST], 0, 2);
    {   /* (amount - 64) / 63 above 64, / 64 below */
        int32_t a = vs_clamp(p[VS_P_MENV_AMOUNT], 0, 127) - 64;
        b->menv_amt = a > 0 ? (a * 32768 + 31) / 63 : a * 512;
        if (a > 0) {                            /* 5 a / 63 in Q28, a 32-bit division in two steps */
            uint32_t q = ((uint32_t)a * 5u << 22) / 63u, rm = ((uint32_t)a * 5u << 22) % 63u;
            b->menv_oct = (int32_t)((q << 6) + ((rm << 6) + 31u) / 63u);
        } else b->menv_oct = a * 5 * (1 << 22);
    }
    vs_env_config(&b->mcfg, (uint32_t)p[VS_P_MENV_A], (uint32_t)p[VS_P_MENV_D], 0u, 0u, 0, 0);
    b->pw_raw = (uint32_t)pw * 16909320u + (uint32_t)pw / 2u;
    b->fm_knob = (uint32_t)vs_clamp(p[VS_P_OSC1_FM], 0, 127);
    {   /* the LFOs' routes: on, with DEPTH above 0 (DEPTH 0 is no modulation at all) */
        uint32_t k;
        b->r_p1 = b->r_p2 = b->r_cut = b->r_amp = b->r_pw = b->r_fm = b->r_pan = 0;
        for (k = 0; k < 2u; k++) {
            int32_t on = p[k ? VS_P_LFO2_ON : VS_P_LFO1_ON], d = vs_clamp(p[k ? VS_P_LFO2_DEPTH : VS_P_LFO1_DEPTH], 0, 127);
            uint32_t dest = on && d > 0 ? (uint32_t)vs_clamp(p[k ? VS_P_LFO2_DEST : VS_P_LFO1_DEST], 1, 7) : 0u;
            b->lfo_dest[k] = dest;
            b->lfo_sc[k] = (d * 65536 + 63) / 127;
            if (dest == 1u) b->r_p2 = 1;
            else if (dest == 2u) b->r_p1 = b->r_p2 = 1;
            else if (dest == 3u) b->r_cut = 1;
            else if (dest == 4u) b->r_amp = 1;
            else if (dest == 5u) b->r_pw = 1;
            else if (dest == 6u) b->r_fm = 1;
            else if (dest == 7u) b->r_pan = 1;
        }
    }
    b->lm = 0;
    /* RING takes OSC2's pulse and noise at full scale with an FM source in the patch: the knob, an LFO on FM, or the
       mod env on FM at AMOUNT 67 or more (at 65, 66: while its offset is above 0, which osc_cross sees) */
    if ((b->menv_on && b->menv_dest == 1u && p[VS_P_MENV_AMOUNT] >= 67) || b->r_fm) b->ring_full = 1;
    b->cross = b->ring || b->sync || b->fm_dev != 0u || b->menv_on || b->r_p2 || b->r_pw || b->r_fm;
    b->dgain = b->sh1 == VS_SH_SINE ? VS_COPY_GAIN_SINE_Q15 : VS_COPY_GAIN_Q15;
}

/* ---- DETUNE's copies (Voice.hpp CopiesSum) */
static void copies_steps(vs_voice_t *v, const blk_t *b, uint32_t inc1)   /* the copies' steps for OSC1's step inc1, when it, DETUNE or the rate moved */
{
    uint32_t k;
    if (b->sub_oct != v->chalf) {               /* SUB OCTAVE moved under a sounding voice: the copies re-lock to it */
        for (k = 0; k < 8u; k++) {
            uint32_t c = v->chalf ? v->cs[k] << 1 : v->cs[k];
            v->cs[k] = b->sub_oct ? c >> 1 : c;
        }
        v->chalf = (uint8_t)b->sub_oct;
        v->cinc_for = 0xFFFFFFFFu;
    }
    if (v->cinc_for == inc1 && v->cdet == b->det) return;
    v->cinc_for = inc1;
    v->cdet = (uint8_t)b->det;
    for (k = 0; k < 8u; k++) {
        uint32_t s = (uint32_t)(((uint64_t)inc1 * VS_DETUNE_Q30[b->det * 8u + k]) >> 30);
        v->cinc[k] = v->chalf ? s >> 1 : s;
    }
}

/* OSC1 and its 8 copies (and the SUB and its 8, when so is given) x the stack's gain, n frames in place. A copy is
 * one table point, the phase truncated to 11 bits (a pulse: two, the width apart; the SUB's square: two, half a
 * cycle apart) */
static void copies_add(vs_voice_t *v, const blk_t *b, int32_t *o1, int32_t *so, const int16_t *t1, const int16_t *ts,
                       uint32_t n, const uint32_t *wv)   /* wv: the width a frame (0: the patch's) */
{
    const uint32_t sh = v->chalf, pulse = b->sh1 == VS_SH_PULSE, square = b->sub_sh == VS_SH_PULSE;
    const uint32_t g = (uint32_t)b->dgain;
    uint32_t wp = b->width >> 21;
    uint32_t i, k;
    copies_steps(v, b, v->inc1);
    for (i = 0; i < n; i++) {
        int32_t c1 = 0, c2 = 0, cs = 0, cq = 0;
        if (wv) wp = wv[i] >> 21;
        for (k = 0; k < 8u; k++) {
            uint32_t ph = v->cs[k], i1 = (ph << sh) >> 21, is = ph >> 21;
            c1 += t1[i1];
            if (pulse) c2 += t1[(i1 + wp) & 2047u];
            if (so) { cs += ts[is]; if (square) cq += ts[(is + 1024u) & 2047u]; }
            v->cs[k] = ph + v->cinc[k];
        }
        if (pulse) c1 = (c1 - c2) >> 1;
        o1[i] = ((o1[i] + c1) * (int32_t)g) >> 15;
        if (so) {
            if (square) cs = (cs - cq) >> 1;
            so[i] = ((so[i] + cs) * (int32_t)g) >> 15;
        }
    }
}

/* ---- the mod env's three destinations (Voice.hpp), at a level e (Q30) */
static uint32_t menv_pitch(uint32_t inc, int32_t e, int32_t k)   /* inc x 2^(e k): +-60 semitones, not quantized */
{
    int32_t q = (int32_t)(((int64_t)e * k) >> 34), oct;                 /* octaves, Q24 (e Q30 x k Q28 = Q58) */
    uint32_t x, r, j;
    uint64_t y;
    oct = q >> 24;                              /* floor */
    x = (uint32_t)q & 0xFFFFFFu;
    j = x >> 16;
    r = VS_EXP2_Q30[j] + (uint32_t)(((uint64_t)(VS_EXP2_Q30[j + 1u] - VS_EXP2_Q30[j]) * (x & 65535u)) >> 16);
    y = ((uint64_t)inc * r) >> 30;
    y = oct >= 0 ? y << oct : y >> -oct;
    return y > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)y;
}
/* the mod env's level after each frame of the next block (vs_env_step's frame law; it never releases) */
static void menv_frames(const vs_env_t *e, const vs_env_cfg_t *c, int32_t *lv, uint8_t *sv)
{
    int32_t l = e->level, st = e->stage;
    uint32_t i;
    for (i = 0; i < VS_CTL; i++) {
        if (st == VS_ENV_ATTACK) {
            l += (int32_t)c->atk;
            if (l >= (1 << 30) || l < 0) { l = 1 << 30; st = VS_ENV_DECAY; }
        } else if (st == VS_ENV_DECAY && l > 0) {
            l += (int32_t)(((int64_t)(-(1 << 19) - l) * (int64_t)c->dec1) >> 31);   /* aims 1/2048 below 0 */
            if (l <= 0) l = 0;
        }
        lv[i] = l;
        sv[i] = (uint8_t)st;
    }
}
/* P/W a frame (Voice.hpp): the duty from P/W / 254, moved by an LFO (l, Q28: toward 1 for l > 0, toward 0 below, by
 * |l| of the room left on that side), then by the mod env the same way (m = e a, held to +-1), held to its limits */
static uint32_t pw_at(const blk_t *b, int32_t l, int32_t e, int menv)
{
    int64_t w = b->pw_raw;
    if (b->r_pw) w += l > 0 ? ((((int64_t)1 << 32) - w) * l) >> 28 : (w * l) >> 28;
    if (menv) {
        int32_t m = vs_clamp((int32_t)(((int64_t)e * b->menv_amt) >> 30), -32768, 32768);
        w += m > 0 ? ((((int64_t)1 << 32) - w) * m) >> 15 : (w * m) >> 15;
    }
    return w < 21136651 ? 21136651u : w > 4271716980LL ? 4271716980u : (uint32_t)w;
}
/* FM's deviation a frame: the FM knob moved toward 127 by an LFO (u = its unipolar sum, Q28), then by the mod env
 * toward 127 (m > 0) or 0 by m of the room left, m = a g: g = e in the attack, then 1 + log2(e) / log2(1e4) (the
 * decay reaches FM in dB), 0 below 1e-4; held to 0..127, the deviation between knobs linear */
static uint32_t fm_at(const blk_t *b, int32_t u, int32_t e, uint32_t stage, int menv)
{
    int64_t k = (int64_t)b->fm_knob << 16;
    uint32_t j, fr;
    if (b->r_fm) k += (((int64_t)127 << 16) - k) * u >> 28;
    if (menv) {
        int32_t g, m;
        if (stage == VS_ENV_ATTACK) g = e >> 14;   /* Q16 */
        else if (e > 107374) {                  /* 1e-4 */
            uint32_t msb = 30, mant;
            int32_t lg;
            while (!((uint32_t)e >> msb)) msb--;
            mant = (uint32_t)e << (30u - msb);  /* 2^30 .. 2^31 */
            j = (mant >> 22) & 255u; fr = (mant >> 14) & 255u;
            lg = ((int32_t)msb - 30) * 65536 + (int32_t)(VS_LOG2_Q16[j] + (((VS_LOG2_Q16[j + 1u] - VS_LOG2_Q16[j]) * fr) >> 8));
            g = 65536 + (int32_t)(((int64_t)lg * VS_MENV_FM_K_Q16) >> 16);
            if (g < 0) g = 0;
        } else g = 0;
        m = vs_clamp((int32_t)(((int64_t)b->menv_amt * g) >> 16), -32768, 32768);   /* Q15 */
        k += (m * (m > 0 ? ((int64_t)127 << 16) - k : k)) >> 15;
    }
    k = k < 0 ? 0 : k > ((int64_t)127 << 16) ? (int64_t)127 << 16 : k;
    j = (uint32_t)(k >> 16); fr = (uint32_t)k & 65535u;
    if (j > 126u) { j = 126u; fr = 65536u; }
    return VS_FM_DEV_Q32[j] + (uint32_t)(((uint64_t)(VS_FM_DEV_Q32[j + 1u] - VS_FM_DEV_Q32[j]) * fr) >> 16);
}

/* ---- the oscillators each in its own loop (nothing crosses them): mo, when not 0, moves OSC1's step (no copies, no
 * SUB then), OSC2's, or the pulse width, a frame at a time */
/* what an LFO or the mod env moves a frame at a time: OSC1's step, OSC2's, the width, FM's deviation (0: constant) */
typedef struct { const uint32_t *i1, *i2, *w, *d; } mod_t;
static void osc_plain(vs_voice_t *v, const blk_t *b, int32_t *o1, int32_t *o2, uint32_t n, const mod_t *mo)
{
    const uint32_t *i1 = mo ? mo->i1 : 0, *i2 = mo ? mo->i2 : 0, *w = mo ? mo->w : 0;
    uint32_t k;
    if (!b->mix1) {                             /* OSC_MIX at OSC2 only: OSC1, its SUB and copies only run their phases */
        if (i1) for (k = 0; k < n; k++) { v->p1 += i1[k]; v->ps += b->sub_oct ? i1[k] >> 1 : i1[k]; }
        else { v->p1 += v->inc1 * n; v->ps += (b->sub_oct ? v->inc1 >> 1 : v->inc1) * n; }
        if (b->det) { copies_steps(v, b, v->inc1); for (k = 0; k < 8u; k++) v->cs[k] += v->cinc[k] * n; }
        if (b->sh1 == VS_SH_NOISE) vs_osc_render(o1, n, VS_SH_NOISE, 0, &k, 0u, 0u, &v->noise);   /* (its LFSR runs) */
        for (k = 0; k < n; k++) o1[k] = 0;
    } else {
    vs_osc_render_mod(o1, n, b->sh1, vs_osc_table(b->sh1, v->tn1), &v->p1, i1, v->inc1, w, b->width, &v->noise);
    {   /* the SUB: at OSC1's step or half of it; its table by OSC1's note, or 12 below. Its phase runs either way */
        uint32_t inc_s = b->sub_oct ? v->inc1 >> 1 : v->inc1, tn = b->sub_oct ? (v->tn1 >= 12u ? v->tn1 - 12u : 0u) : v->tn1;
        int32_t so[VS_CTL];
        if (i1) { for (k = 0; k < n; k++) v->ps += b->sub_oct ? i1[k] >> 1 : i1[k]; }   /* (no SUB here: its phase) */
        else if (b->sub_on) vs_osc_render(so, n, b->sub_sh, vs_osc_table(b->sub_sh, tn), &v->ps, inc_s, 1u << 31, &v->noise);
        else v->ps += inc_s * n;
        if (b->det && !i1) copies_add(v, b, o1, b->sub_on ? so : 0, vs_osc_table(b->sh1, v->tn1), vs_osc_table(b->sub_sh, tn), n, w);
        else if (b->det) { copies_steps(v, b, v->inc1); for (k = 0; k < 8u; k++) v->cs[k] += v->cinc[k] * n; }
        if (b->sub_on && !i1)
            for (k = 0; k < n; k++) o1[k] = (o1[k] * b->main_w + so[k] * b->sub_w) >> 15;
    }
    }
    if (!b->mix2 && b->sh2 != VS_SH_NOISE) {    /* OSC_MIX at OSC1 only: OSC2 only runs its phase */
        if (i2) for (k = 0; k < n; k++) v->p2 += i2[k]; else v->p2 += v->inc2 * n;
        for (k = 0; k < n; k++) o2[k] = 0;
    } else
    vs_osc_render_mod(o2, n, b->sh2, vs_osc_table(b->sh2, v->tn2), &v->p2, i2, v->inc2, w, b->width, &v->noise);
    if (v->mute1) for (k = 0; k < n; k++) o1[k] = 0;
    if (v->mute2 && b->sh2 != VS_SH_NOISE) for (k = 0; k < n; k++) o2[k] = 0;   /* (noise has no pitch to lose) */
}

/* ---- FM, RING, SYNC: the oscillators a frame at a time, OSC2 first (Voice.hpp RenderChunkAs, kOscCross).
 * o1: OSC1 with its copies and the SUB mixed in (MIX's OSC1 side); o2: OSC2, or the ring product */
static inline int32_t osc_read(uint32_t sh, const int16_t *t, uint32_t ph, uint32_t width)
{
    return sh == VS_SH_PULSE ? (vs_osc_rd(t, ph) - vs_osc_rd(t, ph + width)) >> 1 : vs_osc_rd(t, ph);
}
/* the same at twice the scale (Q15: no halving, so no rounding bias): OSC2 as FM's source */
static inline int32_t osc_read2(uint32_t sh, const int16_t *t, uint32_t ph, uint32_t width)
{
    return sh == VS_SH_PULSE ? vs_osc_rd(t, ph) - vs_osc_rd(t, ph + width) : 2 * vs_osc_rd(t, ph);
}


static void osc_cross(vs_voice_t *v, const blk_t *b, int32_t *o1, int32_t *o2, uint32_t n, const mod_t *mo)
{
    const uint32_t tn = b->sub_oct ? (v->tn1 >= 12u ? v->tn1 - 12u : 0u) : v->tn1, half = b->sub_oct, chalf = v->chalf;
    const int16_t *t1 = vs_osc_table(b->sh1, v->tn1), *t2 = vs_osc_table(b->sh2, v->tn2), *ts = vs_osc_table(b->sub_sh, tn);
    const uint32_t pulse1 = b->sh1 == VS_SH_PULSE, square = b->sub_sh == VS_SH_PULSE, noise2 = b->sh2 == VS_SH_NOISE;
    const uint32_t g = (uint32_t)b->dgain;
    uint32_t i, k, inc1 = mo->i1 ? mo->i1[0] : v->inc1, inc2 = v->inc2, width = b->width, dev = b->fm_dev;
    if (b->det) copies_steps(v, b, inc1);
    for (i = 0; i < n; i++) {
        int32_t a2, a2x2 = 0, a1, sub = 0, bit = 0;
        uint32_t wp;
        if (mo->i1 && mo->i1[i] != inc1) {      /* OSC1's pitch moved: its copies with it */
            inc1 = mo->i1[i];
            if (b->det) copies_steps(v, b, inc1);
        }
        if (mo->i2) inc2 = mo->i2[i];
        if (mo->w) width = mo->w[i];
        if (mo->d) dev = mo->d[i];
        wp = width >> 21;
        int64_t d = 0, sraw, s1, p1n;
        /* OSC2 */
        if (noise2) {
            uint32_t s = v->noise, nb = (s >> 12 ^ s >> 17 ^ s >> 18 ^ s >> 19) & 1u;
            v->noise = (s << 1 | nb) & 0xFFFFFu;
            bit = (int32_t)nb;
            a2 = nb ? VS_NOISE_Q14 : -VS_NOISE_Q14;
        } else {
            a2x2 = v->mute2 ? 0 : osc_read2(b->sh2, t2, v->p2, width);
            a2 = a2x2 >> 1;
        }
        /* FM: a fixed deviation a frame, x OSC2 (its noise at 0.65); OSC1's step in whole 2^-27 cycles toward 0 */
        if (!b->ring && dev)
            d = ((int64_t)dev * (noise2 ? (bit ? 2 * VS_NOISE_FM_Q14 : -2 * VS_NOISE_FM_Q14) : a2x2) + 16384) >> 15;
        sraw = (int64_t)inc1 + d;
        s1 = sraw >= 0 ? (int64_t)((uint64_t)sraw & ~(uint64_t)31) : -(int64_t)((uint64_t)(-sraw) & ~(uint64_t)31);
        /* OSC1, the SUB, the copies */
        a1 = osc_read(b->sh1, t1, v->p1, width);
        if (b->sub_on) sub = osc_read(b->sub_sh, ts, v->ps, 1u << 31);
        if (b->det) {
            int32_t c1 = 0, c2 = 0, cs = 0, cq = 0;
            uint32_t dk = (uint32_t)(chalf ? d >> 1 : d);       /* the deviation, at the copies' rate (mod a cycle) */
            for (k = 0; k < 8u; k++) {
                uint32_t ph = v->cs[k], i1 = (ph << chalf) >> 21, is = ph >> 21;
                c1 += t1[i1];
                if (pulse1) c2 += t1[(i1 + wp) & 2047u];
                if (b->sub_on) { cs += ts[is]; if (square) cq += ts[(is + 1024u) & 2047u]; }
                v->cs[k] = ph + v->cinc[k] + dk;
            }
            if (pulse1) c1 = (c1 - c2) >> 1;
            a1 = ((a1 + c1) * (int32_t)g) >> 15;
            if (b->sub_on) {
                if (square) cs = (cs - cq) >> 1;
                sub = b->ring ? sub + cs : ((sub + cs) * (int32_t)g) >> 15;   /* RING: the SUB's copies skip the gain */
            }
        }
        if (v->mute1) a1 = sub = 0;
        o1[i] = b->sub_on ? (int32_t)(((int64_t)a1 * b->main_w + (int64_t)sub * b->sub_w) >> 15) : a1;
        /* RING: OSC2 x OSC1 (with an FM source in the patch: the pulse x 2, the noise at full scale) */
        if (b->ring) {
            const uint32_t full = b->ring_full || dev > 0u;   /* an FM source in the patch, or FM moved above 0 */
            if (full && noise2) a2 = bit ? a1 : -a1;
            else {
                a2 = (int32_t)(((int64_t)a2 * a1) >> 14);
                if (full && b->sh2 == VS_SH_PULSE) a2 *= 2;
            }
        }
        o2[i] = a2;
        /* the phases; SYNC when OSC1's phase drops: a forward wrap, or FM driving it backward without a wrap */
        p1n = (int64_t)v->p1 + s1;
        v->p1 = (uint32_t)p1n;
        v->ps += (uint32_t)(half ? sraw >> 1 : sraw);
        if (b->sync && (p1n >= ((int64_t)1 << 32) || (sraw < 0 && p1n >= 0))) {
            v->p2 = p1n >= ((int64_t)1 << 32) ? VS_SYNC_RESET_Q32 : 0u;
            v->noise = VS_NOISE_SEED;
        } else {
            v->p2 += inc2;
        }
    }
}

/* ---- voices (the ISR's) */
/* the oscillators' start phases (a note on; a MONO retrigger too). OSC PHASE 0: the slot's carry on; above 0: OSC1
 * at 0, OSC2 at (v - 1) / 126 of half a cycle, copy k at deg_k x (v - 1) / 126 of a cycle. relock: the SUB and the
 * copies re-lock to OSC1 (a stolen free-phase voice keeps them: no click) */
static uint32_t slot_shift(uint32_t ph, uint32_t sh)   /* a slot table's phase x UNISON PHASE / 127 (sh, Q16) */
{
    return (uint32_t)(((uint64_t)ph * sh) >> 16);
}
static void start_phases(vs_voice_t *v, const blk_t *b, int relock)
{
    uint32_t c[8], past = 0, k;
    const uint32_t u = v->uk % VS_NSLOT, sh = v->ush;   /* UNISON: the k-th stack voice moves every oscillator by its
                                                           slot k's phase x PHASE / 127 (voice 0 too) */
    if (b->phase > 0u) {
        v->p1 = slot_shift(VS_SLOT_OSC1_Q32[u], sh);
        v->p2 = (b->phase - 1u) * 17043521u + slot_shift(VS_SLOT_OSC2_Q32[u], sh);
    }
    for (k = 0; k < 8u; k++) {
        c[k] = v->chalf ? v->cs[k] << 1 : v->cs[k];
        if (b->phase > 0u) {                    /* deg x (v - 1) x 2^32 / (360 x 126), with 2^36 / 45360 = 1514986 */
            uint64_t x = (((uint64_t)(VS_COPY_PHASE_DEG[k] * (b->phase - 1u)) * 1514986u) >> 4)
                       + slot_shift(VS_SLOT_COPY_Q32[u * 8u + k], sh);
            c[k] = (uint32_t)x;
            if (x >> 32) past |= 1u << k;       /* past one cycle: an octave-down SUB's copy starts half a cycle on */
        }
    }
    if (relock || b->phase > 0u) {
        v->ps = b->sub_oct ? v->p1 >> 1 : v->p1;
        v->chalf = (uint8_t)b->sub_oct;
        for (k = 0; k < 8u; k++) v->cs[k] = b->sub_oct ? (c[k] >> 1) + (past >> k & 1u ? 0x80000000u : 0u) : c[k];
    }
}

/* PORTAMENTO: a voice's note, gliding from its own last pitch (glide) or not. The glide's gap, in 1/128 octaves
 * Q16 from the new note; it aims 1.35 % of the gap past the note, so it lands (Voice::SetNote) */
static void set_note(vs_voice_t *v, uint32_t note, int glide)
{
    if (glide) {                                /* (old - new) semitones x 128 / 12 x 2^16 */
        int32_t d = ((int32_t)v->note - (int32_t)note) * 2097152;
        v->gr += d >= 0 ? (d + 1) / 3 : -((-d + 1) / 3);
    } else v->gr = 0;
    v->gaim = (int32_t)(((int64_t)(v->gr < 0 ? -v->gr : v->gr) * 452985) >> 25);   /* 0.0135 x |gap| (452985 / 2^25) */
    v->note = (uint8_t)note;
}

/* a voice's place in a UNISON stack (0: none): its detune (cents Q8), its pan (-1..1 Q16), its pitch (semitones),
 * k (the k-th voice started) and PHASE / 127 (Q16) */
typedef struct { int32_t c8, pan, semis; uint32_t k, sh; } uni_t;

static void voice_start(vs_engine_t *e, vs_voice_t *v, const blk_t *b, uint32_t note, uint32_t vel, int glide, const uni_t *u)
{
    int stolen = v->aenv.stage != VS_ENV_IDLE;
    v->uc8 = u ? u->c8 : 0; v->upan = u ? u->pan : 0; v->usemis = (int8_t)(u ? u->semis : 0);
    v->uk = (uint8_t)(u ? u->k : 0); v->ush = u ? u->sh : 0;
    set_note(v, note, glide);
    v->vel = (uint8_t)vel; v->gate = 1; v->active = 1;
    v->age = e->now;
    vs_env_trigger(&v->aenv);
    vs_env_trigger(&v->fenv);
    vs_env_trigger(&v->menv);                  /* (from where it is, as the others) */
    v->a_since = 0; v->a_left = 0; v->a_stage = VS_ENV_ATTACK;
    start_phases(v, b, !stolen);
    if (!stolen) {                              /* a stolen voice keeps its filter under the live signal: no click */
        v->low = v->band = v->lenv = 0;
        v->la = -1; v->lk = 0; v->lad[0] = v->lad[1] = v->lad[2] = v->lad[3] = v->lfb = 0; v->agc = 0; v->agg = 65536; diode_reset(v);
        sh_reset(v);
        v->ag = 0;
        v->f = -1;
    }
    v->pitch_key[0] = 0xFFFFFFFFu;
    v->vel_key = 0xFFFFu;
}
/* MONO / LEGATO: a sounding voice takes another note. retrig (MONO): its envelopes restart from where they are, at
 * vel (-1: the velocity kept); LEGATO only moves the pitch */
static void voice_change(vs_voice_t *v, uint32_t note, int32_t vel, int retrig, int glide)
{
    set_note(v, note, glide);
    v->gate = 1;
    if (retrig) {
        if (vel >= 0) v->vel = (uint8_t)vel;
        vs_env_trigger(&v->aenv);
        vs_env_trigger(&v->fenv);
        vs_env_trigger(&v->menv);
    }
}
static void voice_release(vs_voice_t *v)
{
    v->gate = 0;
    vs_env_release(&v->aenv);
    vs_env_release(&v->fenv);
}
static void voice_kill(vs_voice_t *v)
{
    vs_env_reset(&v->aenv); vs_env_reset(&v->fenv); vs_env_reset(&v->menv);
    v->gate = 0; v->active = 0; v->ag = 0; v->a_left = 0;
    v->low = v->band = v->lenv = 0;
    v->lad[0] = v->lad[1] = v->lad[2] = v->lad[3] = v->lfb = 0; v->agc = 0; v->agg = 65536; diode_reset(v);
}

static uint32_t voice_limit(const vs_engine_t *e)
{
    uint32_t nv = e->nv < 1u ? 1u : e->nv > VS_NV ? VS_NV : e->nv, poly = (uint32_t)vs_clamp(e->ev[VS_P_POLY], 1, 32);
    return poly < nv ? poly : nv;
}

/* the next idle voice after the last one started; at the limit the oldest RELEASED voice, then the oldest held;
 * never one started in this control block (0: none left, the note is dropped) */
static vs_voice_t *voice_alloc(vs_engine_t *e, int keep_same)
{
    uint32_t limit = voice_limit(e), k;
    vs_voice_t *oldest = 0, *released = 0, *pick = 0;
    for (k = 1; k <= limit && !pick; k++) {
        vs_voice_t *v = &e->v[(uint32_t)(e->last + (int32_t)k) % limit];
        if (!v->active) pick = v;
        else if (keep_same && v->age == e->now) continue;
        else if (v->gate) { if (!oldest || e->now - v->age > e->now - oldest->age) oldest = v; }
        else if (!released || e->now - v->age > e->now - released->age) released = v;
    }
    if (!pick) pick = released ? released : oldest;
    if (pick) e->last = (int32_t)(pick - e->v);
    return pick;
}

/* ---- keys. The held-key stack follows every key in every mode (a key off removes it, a key on goes on top); MONO
 * and LEGATO play from it (Synth::VoiceNoteOn / VoiceNoteOff) */
static void keys_remove(vs_engine_t *e, uint32_t note)
{
    uint32_t i, n = 0;
    for (i = 0; i < e->nkeys; i++)
        if (e->keys[i] != note) e->keys[n++] = e->keys[i];
    e->nkeys = (uint8_t)n;
}
static void retrigger_phases(vs_engine_t *e, const blk_t *b)   /* MONO: a retrigger restarts the oscillators' phases */
{
    uint32_t i;
    for (i = 0; i < VS_NV; i++)
        if (e->v[i].active && e->v[i].gate) start_phases(&e->v[i], b, 1);
}
/* a key comes up (after the pedal, if it held it). POLY: its voice is released. MONO / LEGATO: with keys still held,
 * the voice falls back to the top one (MONO retriggers its envelopes, the velocity kept); else it is released */
static void key_release(vs_engine_t *e, const blk_t *b, uint32_t note)
{
    const int32_t mode = e->ev[VS_P_PLAY_MODE];
    const int porta = e->ev[VS_P_PORTA_TIME] > 0;
    uint32_t i;
    keys_remove(e, note);
    if (mode != 0) {
        if (e->nkeys) {
            uint32_t back = e->keys[e->nkeys - 1u], changed = 0;
            for (i = 0; i < VS_NV; i++)
                if (e->v[i].active && e->v[i].note == note) { voice_change(&e->v[i], back, -1, mode == 1, porta); changed = 1; }
            if (changed && mode == 1) retrigger_phases(e, b);
            return;
        }
        for (i = 0; i < VS_NV; i++)
            if (e->v[i].active && e->v[i].gate) voice_release(&e->v[i]);
        return;
    }
    for (i = 0; i < VS_NV; i++)
        if (e->v[i].active && e->v[i].gate && e->v[i].note == note) voice_release(&e->v[i]);
}

/* a note on a voice (Synth::VoiceNoteOn): the key stack, then MONO / LEGATO's note change, or a voice (UNISON: N) */
static void note_on(vs_engine_t *e, const blk_t *b, uint32_t note, uint32_t vel, int keysync)
{
    uint32_t i;
    vs_voice_t *v;
    uint32_t any = 0;
    for (i = 0; i < VS_NV; i++) any |= e->v[i].active && e->v[i].gate;
    if (!any && keysync) {              /* KEY SYNC: the first note of a phrase restarts the LFO (not an arp step) */
        if (e->ev[VS_P_LFO1_KEYSYNC]) vs_lfo_restart(&e->lfo[0]);
        if (e->ev[VS_P_LFO2_KEYSYNC]) vs_lfo_restart(&e->lfo[1]);
    }
    const int32_t mode = e->ev[VS_P_PLAY_MODE];
    /* a fresh note glides from its voice's own last pitch (AUTO: never); a mono note change always glides */
    const int porta = e->ev[VS_P_PORTA_TIME] > 0, glide = porta && !e->ev[VS_P_PORTA_AUTO];
    keys_remove(e, note);
    if (e->nkeys < 32u) e->keys[e->nkeys++] = (uint8_t)note;
    else e->keys[31] = (uint8_t)note;
    if (mode != 0) {
        if (any) {                      /* a voice sounds: it takes the note (MONO retriggers, LEGATO glides on) */
            for (i = 0; i < VS_NV; i++)
                if (e->v[i].active) voice_change(&e->v[i], note, (int32_t)vel, mode == 1, porta);
            if (mode == 1) retrigger_phases(e, b);
            return;
        }
    } else {                            /* POLY: pressed again while held, its voice is released, a fresh one starts */
        for (i = 0; i < VS_NV; i++)
            if (e->v[i].active && e->v[i].gate && e->v[i].note == note) voice_release(&e->v[i]);
    }
    if (!e->ev[VS_P_UNI_ON]) {     /* MONO / LEGATO play voice 0; POLY the round robin */
        if (mode != 0) { e->last = 0; v = &e->v[0]; }
        else v = voice_alloc(e, 1);
        if (v) voice_start(e, v, b, note, vel, glide, 0);
        return;
    } else {                            /* UNISON: N voices over +-DETUNE cents, SPREAD, odd ones at PITCH - 24 */
        const uint32_t n = (uint32_t)vs_clamp(e->ev[VS_P_UNI_VOICES], 2, 8), lim = voice_limit(e);
        const uint32_t sounding = n < lim ? n : lim;
        const int32_t d = (int32_t)VS_UNI_DETUNE_Q8[vs_clamp(e->ev[VS_P_UNI_DETUNE], 0, 127)];
        const int32_t q = vs_clamp(e->ev[VS_P_UNI_PAN], 0, 127) - 64;
        uni_t u;
        uint32_t kk;
        u.sh = (uint32_t)vs_clamp(e->ev[VS_P_UNI_PHASE], 0, 127) * 65536u / 127u;
        for (kk = 0; kk < sounding; kk++) {
            uint32_t i2;
            int32_t t;
            if (mode != 0) { e->last = (int32_t)kk; v = &e->v[kk]; }   /* MONO: voices 0..N-1 */
            else v = voice_alloc(e, 0);  /* (a stack may steal from a note of the same block) */
            if (!v) return;
            i2 = (uint32_t)e->last % n;  /* a voice's place is its slot's: slot % N (a stack may hold one twice) */
            t = (int32_t)(2u * i2 * 65536u / (n - 1u)) - 65536;   /* -1..1, Q16 */
            u.c8 = (t * d) >> 16;
            u.pan = (t * q) >> 6;       /* t x (SPREAD - 64) / 64, Q16 */
            u.semis = (i2 & 1u) ? vs_clamp(e->ev[VS_P_UNI_PITCH], 0, 48) - 24 : 0;
            u.k = kk;
            voice_start(e, v, b, note, vel, glide, &u);
        }
        return;
    }
}

/* ---- the arp (Synth::ArpNoteOn / ArpNoteOff / ArpNextKey / ArpAdvance): the held keys sorted, a step a note
 * value of the tempo (BEAT), its gate GATE / 127 of the step (0: silent), types 1 up & down, 2 up, 3 down, 4 random,
 * over RANGE + 1 octaves. Not locked to a beat: steps start at the key. Its notes take voices as keys do (no KEY
 * SYNC: the key that starts a phrase restarts the LFOs). Steps land on the 16-frame grid (Synth2: to the frame) */
static void arp_note_on(vs_engine_t *e, uint32_t note, uint32_t vel)
{
    uint32_t i;
    if (!e->arp_n) {
        e->arp_step = -1; e->arp_count = 0; e->arp_dir = 1;
        if (e->ev[VS_P_LFO1_KEYSYNC]) vs_lfo_restart(&e->lfo[0]);
        if (e->ev[VS_P_LFO2_KEYSYNC]) vs_lfo_restart(&e->lfo[1]);
    }
    for (i = 0; i < e->arp_n; i++) if (e->arp_keys[i] == note) return;
    if (e->arp_n < 32u) {                       /* sorted in */
        for (i = e->arp_n; i > 0u && e->arp_keys[i - 1u] > note; i--) e->arp_keys[i] = e->arp_keys[i - 1u];
        e->arp_keys[i] = (uint8_t)note;
        e->arp_n++;
    }
    e->arp_vel = (uint8_t)vel;
}
static void arp_note_off(vs_engine_t *e, const blk_t *b, uint32_t note)
{
    uint32_t i, n = 0;
    for (i = 0; i < e->arp_n; i++) if (e->arp_keys[i] != note) e->arp_keys[n++] = e->arp_keys[i];
    e->arp_n = (uint8_t)n;
    if (!n && e->arp_play >= 0) { key_release(e, b, (uint32_t)e->arp_play); e->arp_play = -1; }
}
/* a key comes up (after the pedal): the arp's, or a voice's (Synth::ReleaseKey) */
static void release_key(vs_engine_t *e, const blk_t *b, uint32_t note)
{
    uint32_t i, mine = 0;
    for (i = 0; i < e->arp_n; i++) mine |= e->arp_keys[i] == note;
    if (mine) arp_note_off(e, b, note);
    if (e->ev[VS_P_ARP_ON] && mine) return;     /* the arp plays it */
    if (!e->ev[VS_P_ARP_ON] || (int32_t)note != e->arp_play) key_release(e, b, note);
}
static uint32_t arp_next(vs_engine_t *e)
{
    const int32_t total = (int32_t)e->arp_n * (vs_clamp(e->ev[VS_P_ARP_RANGE], 0, 3) + 1), type = e->ev[VS_P_ARP_TYPE];
    if (type == 4) {                            /* random: Synth2's xorshift */
        uint32_t x = e->arp_rng;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        e->arp_rng = x;
        e->arp_step = (int32_t)(x % (uint32_t)total);
    } else if (type == 3) e->arp_step = e->arp_step <= 0 ? total - 1 : e->arp_step - 1;
    else if (type == 2) e->arp_step = (e->arp_step + 1) % total;
    else if (total == 1) e->arp_step = 0;
    else {
        if (e->arp_step + e->arp_dir >= total || e->arp_step + e->arp_dir < 0) e->arp_dir = -e->arp_dir;
        e->arp_step = vs_clamp(e->arp_step + e->arp_dir, 0, total - 1);
    }
    return e->arp_keys[e->arp_step % e->arp_n] + 12u * (uint32_t)(e->arp_step / (int32_t)e->arp_n);
}
static void arp_advance(vs_engine_t *e, const blk_t *b, int32_t elapsed)
{
    const int32_t gate = vs_clamp(e->ev[VS_P_ARP_GATE], 0, 127);
    if (!e->ev[VS_P_ARP_ON] || (!e->arp_n && e->arp_play < 0)) return;
    e->arp_count -= elapsed;
    e->arp_gate -= elapsed;
    if (e->arp_play >= 0 && e->arp_gate <= 0 && gate < 127) { key_release(e, b, (uint32_t)e->arp_play); e->arp_play = -1; }
    if (e->arp_count <= 0 && e->arp_n) {
        const uint32_t bpm = e->bpm_q8 < (20u << 8) ? 20u << 8 : e->bpm_q8;
        int32_t frames;
        e->arp_acc += (int32_t)vs_udiv64_32((uint64_t)VS_ARP_STEP[vs_clamp(e->ev[VS_P_ARP_BEAT], 0, 18)] << 8, bpm);   /* frames Q8 */
        frames = e->arp_acc >> 8 < 1 ? 1 : e->arp_acc >> 8;
        e->arp_acc -= frames << 8;
        if (e->arp_play >= 0) key_release(e, b, (uint32_t)e->arp_play);
        e->arp_play = (int32_t)arp_next(e);
        if (e->arp_play > 127) e->arp_play = 127;
        if (gate > 0) note_on(e, b, (uint32_t)e->arp_play, e->arp_vel, 0);
        else e->arp_play = -1;
        e->arp_count += frames;
        e->arp_gate = frames * gate / 127 < 1 ? 1 : frames * gate / 127;
    }
}
/* ARP ON moved with keys down (a program change): on, the held keys become the arp's; off, its keys play voices */
static void arp_switched(vs_engine_t *e, const blk_t *b)
{
    uint32_t k;
    if (e->ev[VS_P_ARP_ON]) {
        for (k = 0; k < 128u; k++)
            if ((e->down[k >> 5] | e->held[k >> 5]) >> (k & 31u) & 1u) { key_release(e, b, k); arp_note_on(e, k, e->kvel[k]); }
        return;
    }
    {
        uint8_t keys[32];
        uint32_t n = e->arp_n;
        for (k = 0; k < n; k++) keys[k] = e->arp_keys[k];
        if (e->arp_play >= 0) key_release(e, b, (uint32_t)e->arp_play);
        e->arp_play = -1; e->arp_n = 0;
        for (k = 0; k < n; k++) note_on(e, b, keys[k], e->kvel[keys[k]], 1);
    }
}

static void events(vs_engine_t *e, const blk_t *b)
{
    while (e->ev_r != e->ev_w) {
        uint32_t w = e->evq[e->ev_r], note = (w >> 8) & 127u, vel = w & 127u, bit = 1u << (note & 31u), i;
        e->ev_r = (e->ev_r + 1u) & (VS_EVQ - 1u);
        switch (w >> 16) {
        case EV_ON:
            e->down[note >> 5] |= bit;
            e->held[note >> 5] &= ~bit;
            e->kvel[note] = (uint8_t)vel;
            if (e->ev[VS_P_ARP_ON]) arp_note_on(e, note, vel);   /* the arp plays it */
            else note_on(e, b, note, vel, 1);
            break;
        case EV_OFF:
            e->down[note >> 5] &= ~bit;
            if (e->pedal) e->held[note >> 5] |= bit;
            else release_key(e, b, note);
            break;
        case EV_PEDAL:
            e->pedal = (uint8_t)(vel != 0u);
            if (!e->pedal)
                for (i = 0; i < 128u; i++)
                    if (e->held[i >> 5] >> (i & 31u) & 1u) {
                        e->held[i >> 5] &= ~(1u << (i & 31u));
                        if (!(e->down[i >> 5] >> (i & 31u) & 1u)) release_key(e, b, i);
                    }
            break;
        default:                                /* EV_ALLOFF */
            for (i = 0; i < VS_NV; i++) voice_kill(&e->v[i]);
            for (i = 0; i < 4u; i++) e->down[i] = e->held[i] = 0;
            e->pedal = 0;
            e->nkeys = 0;
            e->arp_n = 0; e->arp_play = -1;
            break;
        }
    }
}

/* ---- the amp gain of a block (Voice::AmpGain): up to two straight pieces */
typedef struct { uint32_t n; int32_t step; } seg_t;

static uint32_t amp_plan(vs_voice_t *v, const vs_env_cfg_t *c, uint32_t pos, uint32_t n, seg_t s[2])
{
    uint8_t stage = v->aenv.stage;
    if (v->a_since < GRID) v->a_since = (uint16_t)(v->a_since + n);
    if (stage == VS_ENV_ATTACK || stage == VS_ENV_IDLE || (v->a_left <= 0 && (v->a_since < GRID || pos != 0u))) {
        /* the gain follows the envelope: the attack, the first 256 frames of a note, up to the next boundary */
        uint32_t k = vs_env_step(&v->aenv, c);
        v->a_stage = stage;
        v->a_left = 0;
        if (k < n) {                            /* the attack ended k frames in: up to 1.0, then the decay's start */
            s[0].n = k; s[0].step = (ONE30 - v->ag) / (int32_t)k;
            s[1].n = n - k; s[1].step = (v->aenv.level - ONE30) / (int32_t)(n - k);
            return 2u;
        }
        s[0].n = n; s[0].step = (v->aenv.level - v->ag) / (int32_t)n;
        return 1u;
    }
    if (pos == 0u || stage != v->a_stage || v->a_left <= 0) {
        /* a boundary, or the stage changed inside a ramp: aim at the 11-bit gain of the next boundary */
        vs_env_t ahead = v->aenv;
        uint32_t left = GRID - pos, j;
        if (pos == 0u) v->ag = v->aenv.level & Q11_MASK;
        for (j = 0; j < left; j += VS_CTL) vs_env_step(&ahead, c);
        v->ag_step = ((ahead.level & Q11_MASK) - v->ag) / (int32_t)left;
        v->a_left = (int32_t)left;
        v->a_stage = stage;
    }
    vs_env_step(&v->aenv, c);
    v->a_left -= (int32_t)n;
    s[0].n = n; s[0].step = v->ag_step;
    return 1u;
}

/* a voice held at 4 x full scale (Q20), and a little: -(4 << 20) is 0xFFC00000, a value tools/build.py's ROM check
 * reads as an address (it can not tell a constant from a pointer); -(4 << 20) - 4096 loads as one lui outside it */
#define VOICE_HOLD ((4 << 20) + 4096)
static inline int32_t mulr_q30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b + (1 << 29)) >> 30); }

#if VS_LPDL
/* ---- LPDL (Filter.hpp, fitted): a linear diode ladder, 2 x oversampled, trapezoidal, with a high-passed feedback,
 *   dy1 = w(-2 y1 + y2 + u), dy2 = w(y1 - 2 y2 + y3), dy3 = w(y2 - 2 y3 + y4), dy4 = w(y3 - y4),
 *   u = x - k (y4 - z), dz = 2 pi 100 (y4 - z), out = g (y3 + y4) / 2,
 * the input through tanh (its drive falling with resonance: x 1.3 / g in, undone out) and the feedback through
 * tanh(2 k f) / 2 (as an explicit extra input k f (1 - G) from the state before the step, so the linear part stays
 * implicit). A step is x' = P x + q (u0 + u1) (P, q Q27), P = (I - hA)^-1 (I + hA), q = (I - hA)^-1 e1 h w: Synth2 inverts the
 * 5 x 5 matrix in double; here P and q are solved for by elimination (partial pivoting) in Q30 whenever the cutoff
 * moved 1/8 knob or the resonance moved, once a control block at most. States Q20, held to +-50 as Synth2's */
#define DIODE_MAX (50 << 24)                /* (states Q24) */
static int32_t tanhx(int32_t x)                 /* tanh(x) / x for x Q20, Q30 */
{
    int64_t x2 = ((int64_t)x * x) >> 24;        /* Q16 */
    uint32_t i, fr;
    if (x2 >= ((int64_t)32 << 16)) {            /* 1 / |x| */
        uint32_t ax = (uint32_t)(x < 0 ? -x : x);
        return (int32_t)vs_udiv64_32((uint64_t)1 << 50, ax);
    }
    i = (uint32_t)x2 >> 11; fr = (uint32_t)x2 & 2047u;
    return VS_TANHX_Q30[i] + (int32_t)(((int64_t)(VS_TANHX_Q30[i + 1u] - VS_TANHX_Q30[i]) * fr) >> 11);
}
static int32_t exp2_q24(int32_t l, int out_q)   /* 2^(l / 2^24) in Q out_q (no overflow checks: callers bound l) */
{
    int32_t ip = l >> 24, sh;
    uint32_t x = (uint32_t)l & 0xFFFFFFu, j = x >> 16, r;
    r = VS_EXP2_Q30[j] + (uint32_t)(((uint64_t)(VS_EXP2_Q30[j + 1u] - VS_EXP2_Q30[j]) * (x & 65535u)) >> 16);
    sh = 30 - out_q - ip;                       /* r Q30 x 2^ip -> Q out_q */
    return sh >= 0 ? (int32_t)(r >> (sh > 31 ? 31 : sh)) : (int32_t)(r << -sh);
}
static void lpdl_coef(vs_voice_t *v, int32_t knob, int32_t res)
{
    static const uint8_t RES[9] = {0, 16, 32, 48, 64, 80, 96, 112, 127};
    int64_t a[5][5], n5[5][5];
    int32_t hw, k, g, rt, ct, ci, c4, row, i, j, rr, piv;
    int32_t wr[2], kr[2], gr[2];
    uint32_t ri = 0;
    v->dres = (int16_t)res; v->dcut = knob;
    while (ri < 7u && res > RES[ri + 1u]) ri++;
    rt = (int32_t)(((uint32_t)vs_clamp(res - RES[ri], 0, RES[ri + 1u] - RES[ri]) << 16) / (uint32_t)(RES[ri + 1u] - RES[ri]));
    c4 = vs_clamp(knob, 0, 127 << 16) >> 2;    /* cutoff / 4, Q16 */
    ci = c4 >> 16 > 30 ? 30 : c4 >> 16;
    ct = c4 - (ci << 16);                       /* > 1 above knob 124: the last step extrapolated (w only) */
    for (row = 0; row < 2; row++) {
        const uint32_t o = (ri + (uint32_t)row) * 32u + (uint32_t)ci;
        const int32_t ctl = ct > 65536 ? 65536 : ct;
        int32_t lw = VS_LPDL_LW_Q24[o] + (int32_t)(((int64_t)(VS_LPDL_LW_Q24[o + 1u] - VS_LPDL_LW_Q24[o]) * ct) >> 16);
        wr[row] = exp2_q24(lw + VS_LPDL_LOG2H_Q24, 30);   /* h w, Q30 */
        kr[row] = VS_LPDL_K_Q16[o] + (int32_t)(((int64_t)(VS_LPDL_K_Q16[o + 1u] - VS_LPDL_K_Q16[o]) * ctl) >> 16);
        gr[row] = VS_LPDL_G_Q16[o] + (int32_t)(((int64_t)(VS_LPDL_G_Q16[o + 1u] - VS_LPDL_G_Q16[o]) * ctl) >> 16);
    }
    hw = wr[0] + (int32_t)(((int64_t)(wr[1] - wr[0]) * rt) >> 16);
    k = kr[0] + (int32_t)(((int64_t)(kr[1] - kr[0]) * rt) >> 16);
    g = gr[0] + (int32_t)(((int64_t)(gr[1] - gr[0]) * rt) >> 16);
    if (g < 656) g = 656;                       /* (0.01: the tables' gains are well above) */
    v->dk = k;
    v->dsc = (int32_t)vs_udiv64_32((uint64_t)85197 << 16, (uint32_t)g);   /* 1.3 / g, Q16 */
    v->dgo = (int32_t)(((int64_t)g * g * 50412) >> 32);                    /* g / sc = g^2 / 1.3, Q16 */
    /* h A, Q30 */
    for (i = 0; i < 5; i++) for (j = 0; j < 5; j++) a[i][j] = 0;
    {
        const int64_t kh = ((int64_t)k * hw) >> 16;
        a[0][0] = -2 * (int64_t)hw; a[0][1] = hw; a[0][3] = -kh; a[0][4] = kh;
        a[1][0] = hw; a[1][1] = -2 * (int64_t)hw; a[1][2] = hw;
        a[2][1] = hw; a[2][2] = -2 * (int64_t)hw; a[2][3] = hw;
        a[3][2] = hw; a[3][3] = -(int64_t)hw;
        a[4][3] = VS_LPDL_WH_Q30;               /* a[4][4] = -wh h: put in below as 1 -+ wh h (a constant -wh h would sit
                                                   in 0xFFC.. ...: tools/build.py's ROM check reads it as an address) */
    }
    for (i = 0; i < 5; i++) for (j = 0; j < 5; j++) n5[i][j] = (i == j ? (int64_t)1 << 30 : 0) + a[i][j];
    n5[4][4] = ((int64_t)1 << 30) - VS_LPDL_WH_Q30;
    /* solve (I - hA) X = [N | e1 h w], all six right-hand columns in one elimination */
    {
        int64_t mm[5][11];
        for (i = 0; i < 5; i++) {
            for (j = 0; j < 5; j++) { mm[i][j] = (i == j ? (int64_t)1 << 30 : 0) - a[i][j]; mm[i][5 + j] = n5[i][j]; }
            if (i == 4) mm[4][4] = ((int64_t)1 << 30) + VS_LPDL_WH_Q30;
            mm[i][10] = i == 0 ? hw : 0;
        }
        for (i = 0; i < 5; i++) {
            int64_t p, rcp;
            piv = i;
            for (rr = i + 1; rr < 5; rr++) if ((mm[rr][i] < 0 ? -mm[rr][i] : mm[rr][i]) > (mm[piv][i] < 0 ? -mm[piv][i] : mm[piv][i])) piv = rr;
            if (piv != i) for (j = 0; j < 11; j++) { int64_t t = mm[i][j]; mm[i][j] = mm[piv][j]; mm[piv][j] = t; }
            p = mm[i][i];
            {   /* 1 / p, Q30: the pivots stay near 1 (the matrix is I - hA with h w < 1) */
                uint64_t ap = (uint64_t)(p < 0 ? -p : p);
                if (ap < ((uint64_t)1 << 27)) ap = (uint64_t)1 << 27;
                rcp = (int64_t)vs_udiv64_32((uint64_t)1 << 60, (uint32_t)ap);
                if (p < 0) rcp = -rcp;
            }
            for (j = 0; j < 11; j++) mm[i][j] = (mm[i][j] * rcp) >> 30;
            for (rr = 0; rr < 5; rr++)
                if (rr != i && mm[rr][i] != 0) {
                    const int64_t f = mm[rr][i];
                    for (j = 0; j < 11; j++) mm[rr][j] -= (f * mm[i][j]) >> 30;
                }
        }
        {   /* a frame is two steps on the same def (lpdl_tick): x'' = P (P x + q sA) + q sB = P^2 x + (P q) sA + q sB.
               P (Q27, up to 8.9), P^2 (up to 7.9) and P q are made here, once per change, so a frame is one
               5x5 product, not two (the states' clamp, a guard, now once a frame) */
            int32_t pm[5][5];
            for (i = 0; i < 5; i++) {
                for (j = 0; j < 5; j++) pm[i][j] = (int32_t)(mm[i][5 + j] >> 3);
                v->dq[i] = (int32_t)(mm[i][10] >> 3);
            }
            for (i = 0; i < 5; i++) {
                int64_t aq = 0;
                for (j = 0; j < 5; j++) {
                    int64_t a2 = 0;
                    for (rr = 0; rr < 5; rr++) a2 += (int64_t)pm[i][rr] * pm[rr][j];
                    v->dp[i][j] = (int32_t)((a2 + (1 << 26)) >> 27);
                    aq += (int64_t)pm[i][j] * v->dq[j];
                }
                v->dq1[i] = (int32_t)((aq + (1 << 26)) >> 27);
            }
        }
    }
}
static inline void lpdl_step2(vs_voice_t *v, int32_t sa, int32_t sb)   /* a frame's two steps (lpdl_coef): Q24 */
{
    const int32_t d0 = v->dd[0], d1 = v->dd[1], d2 = v->dd[2], d3 = v->dd[3], d4 = v->dd[4];
    int32_t i;
    for (i = 0; i < 5; i++) {
        const int32_t *p = v->dp[i];
        const int64_t acc = (int64_t)p[0] * d0 + (int64_t)p[1] * d1 + (int64_t)p[2] * d2 + (int64_t)p[3] * d3
                          + (int64_t)p[4] * d4 + (int64_t)v->dq1[i] * sa + (int64_t)v->dq[i] * sb + (1 << 26);
        const int64_t t = acc >> 27;
        int32_t y = (int32_t)t;
        if (t != (int64_t)y) y = t < 0 ? -DIODE_MAX : DIODE_MAX;   /* (a 32-bit clamp after a cheap range test) */
        v->dd[i] = vs_clamp(y, -DIODE_MAX, DIODE_MAX);
    }
}
static inline int32_t lpdl_tick(vs_voice_t *v, int32_t x)   /* a frame: x Q20 in, Q20 out (inside: Q24) */
{
    const int32_t u = (int32_t)(((int64_t)x * v->dsc) >> 12);                     /* Q24 */
    const int32_t xs = (int32_t)(((int64_t)u * tanhx(u >> 4)) >> 30);
    const int32_t mid = (int32_t)(((int64_t)v->dx + xs) >> 1);
    const int32_t kf = (int32_t)vs_clamp64(((int64_t)v->dk * (v->dd[3] - v->dd[4])) >> 16, -DIODE_MAX, DIODE_MAX);
    const int32_t def = (int32_t)(((int64_t)kf * ((1 << 30) - tanhx(kf >> 3))) >> 30);   /* tanh(2 k f): (2 kf) Q20 */
    lpdl_step2(v, (int32_t)vs_clamp64((int64_t)v->dx + mid + 2 * (int64_t)def, -0x7FFFFFFF, 0x7FFFFFFF),
               (int32_t)vs_clamp64((int64_t)mid + xs + 2 * (int64_t)def, -0x7FFFFFFF, 0x7FFFFFFF));
    v->dx = xs;
    return (int32_t)(((int64_t)v->dgo * (((int64_t)v->dd[2] + v->dd[3]) >> 1)) >> 20);
}
#endif

/* mix -> SVF (twice a frame, the input held) -> limiter -> saturation -> amp, n frames added to the mono bus `out`,
 * the gain stepping.
 * ftype and sat are constants at each call site: each case is its own loop */
static inline __attribute__((always_inline)) void voice_out(vs_voice_t *v, const blk_t *b, const int32_t *o1, const int32_t *o2,
                                                            int32_t *out, uint32_t n, int32_t fstep, int32_t kstep, int32_t gstep,
                                                            int32_t g_amp, const int32_t *am, const uint32_t ftype, const int sat)
{
    int32_t low = v->low, band = v->band, lenv = v->lenv, f = v->f, ag = v->ag;
    int32_t la = v->la, lk = v->lk, s0 = v->lad[0], s1 = v->lad[1], s2 = v->lad[2], s3 = v->lad[3], fb = v->lfb;
    int32_t agc = v->agc, agg = v->agg;
    const int32_t mix1 = b->mix1, mix2 = b->mix2, damp = b->damp, gin = b->gin;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t x = (o1[i] * mix1 + o2[i] * mix2) >> 15;            /* Q14 */
        int32_t high, y, a, ge;
        if (ftype == 5u) {                      /* SH-101 (sh101_vcf.h, float): Q14 in, Q20 out; no output limiter (its
                                                   diodes limit it, as LPDL's feedback does) */
#if VS_SH101
            float fy = sh101_process(&v->shc, &v->shs, (float)x * (1.0f / 16384.0f)) * 1048576.0f;
            fy = fy > (float)STATE_MAX ? (float)STATE_MAX : fy < -(float)STATE_MAX ? -(float)STATE_MAX : fy;
            y = (int32_t)fy;
#else
            y = 0;
#endif
            high = 0; (void)high;
        } else if (ftype == 4u) {                      /* LPDL (no output limiter: its feedback saturates) */
#if VS_LPDL
            y = lpdl_tick(v, x * 64);
#else
            y = 0;
#endif
            high = 0; (void)high;
        } else if (ftype == 1u) {                      /* LP24: four bilinear one-poles, the feedback a frame late through the AGC */
            int32_t u = x * 64 - (int32_t)(((int64_t)(((int64_t)lk * agg) >> 16) * fb) >> 16), vv, yy;   /* no input gain */
            la += fstep; lk += kstep;
            vv = (int32_t)(((int64_t)(u - s0) * la) >> 31); yy = vv + s0; s0 = yy + vv; u = yy;
            vv = (int32_t)(((int64_t)(u - s1) * la) >> 31); yy = vv + s1; s1 = yy + vv; u = yy;
            vv = (int32_t)(((int64_t)(u - s2) * la) >> 31); yy = vv + s2; s2 = yy + vv; u = yy;
            vv = (int32_t)(((int64_t)(u - s3) * la) >> 31); yy = vv + s3; s3 = yy + vv; u = yy;
            s0 = vs_clamp(s0, -STATE_MAX, STATE_MAX); s1 = vs_clamp(s1, -STATE_MAX, STATE_MAX);
            s2 = vs_clamp(s2, -STATE_MAX, STATE_MAX); s3 = vs_clamp(s3, -STATE_MAX, STATE_MAX);
            u = vs_clamp(u, -STATE_MAX, STATE_MAX);
            fb = u;
            a = u < 0 ? -u : u;                 /* the AGC's follower (1.2 ms up, 202 ms down) and its gain above T */
            agc += (int32_t)(((int64_t)(a - agc) * (a > agc ? (int32_t)VS_AGC_ATK_Q31 : (int32_t)VS_AGC_REL_Q31)) >> 31);
            if (agc > VS_AGC_T_Q14 * 64) {
                uint32_t ei = (uint32_t)agc >> 17, fr = ((uint32_t)agc >> 1) & 65535u;   /* steps of 1/8 (Q20 >> 17) */
                if (ei >= 512u) { ei = 511u; fr = 65535u; }
                agg = (int32_t)VS_AGC_Q16[ei] + (int32_t)((((int32_t)VS_AGC_Q16[ei + 1u] - (int32_t)VS_AGC_Q16[ei]) * (int32_t)(fr >> 1)) >> 15);
            } else agg = 65536;
            y = u;
        } else {
        int32_t in = ((x * gin) >> 15) * 64;    /* the input gain; Q14 -> Q20 */
        f += fstep;
        low += mulr_q30(f, band);
        high = in - low - vs_mul_q15(band, damp);
        band += mulr_q30(f, high);
        low += mulr_q30(f, band);
        high = in - low - vs_mul_q15(band, damp);
        band += mulr_q30(f, high);
        low = vs_clamp(low, -STATE_MAX, STATE_MAX);
        band = vs_clamp(band, -STATE_MAX, STATE_MAX);
        y = ftype == 0u ? low : ftype == 2u ? high : band;
        }
        a = y < 0 ? -y : y;                     /* the output limiter: a gain on a follower of |y| (not LPDL, not SH-101:
                                                   their feedback limits itself; on SH-101's singing the follower rode
                                                   the wave, 9 % THD at 17 Hz) */
        if (ftype != 4u && ftype != 5u) {
        lenv += (((a - lenv) >> 8) * (a > lenv ? LIMIT_ATK_Q16 : LIMIT_REL_Q16)) >> 8;
        if (lenv > VS_LIMIT_FLOOR_Q14 * 64) {
            uint32_t ei = (uint32_t)lenv >> 15, fr = (uint32_t)lenv & 32767u;
            int32_t g0, g1;
            if (ei > 2048u) { ei = 2048u; fr = 0; }
            g0 = VS_LIMIT_Q16[ei]; g1 = VS_LIMIT_Q16[ei + 1u];
            y = (((y >> 6) * ((g0 + (((g1 - g0) * (int32_t)fr) >> 15)) >> 2)) >> 14) * 64;   /* (32-bit: it is loud here) */
        }
        }
        if (sat) {                              /* y (1 + a) / (1 + a |y|) */
            uint32_t ay = (uint32_t)(y < 0 ? -y : y);
            uint32_t den = 65536u + (uint32_t)(((uint64_t)ay * (uint32_t)b->sat_a) >> 20);
            y = (int32_t)(((int64_t)y * (int32_t)(b->sat_num / (den >> 7))) >> 16);
        }
        ag += gstep;
        ge = (g_amp * (ag >> 15)) >> 15;        /* 0.2 x GAIN x velocity x the envelope, Q16 */
        if (am) ge = (int32_t)(((int64_t)ge * am[i]) >> 15);   /* x an LFO on AMP */
        y = (int32_t)(((int64_t)vs_clamp(y, -VOICE_HOLD, VOICE_HOLD) * ge) >> 17);   /* Q20 (held at 4 x full scale) x Q16 -> Q19 */
        out[i] += y;
    }
    v->low = low; v->band = band; v->lenv = lenv; v->f = f; v->ag = ag;
    v->la = la; v->lk = lk; v->lad[0] = s0; v->lad[1] = s1; v->lad[2] = s2; v->lad[3] = s3; v->lfb = fb;
    v->agc = agc; v->agg = agg;
}

/* inc x 2^(f / 256 / 1200): a fraction of a cent (UNISON's detune), f 0..255; 2^30 ln2 / 307200 = 2422.8 */
static uint32_t cents_frac(uint32_t inc, uint32_t f)
{
    return f ? inc + (uint32_t)(((uint64_t)inc * (f * 2423u)) >> 30) : inc;
}

/* the cutoff in knob units (Q16) at the filter env's level lv (Q30): FREQ (its glide) + KBD TRACK about key 48; the env
 * sweeps from base - 2.1 to the target base + AMOUNT's swing (x the velocity scale with VEL SW) held to [-2, 125];
 * an LFO on FILTER pulls it toward 125 */
static int32_t cut_knob(const vs_engine_t *e, const vs_voice_t *v, const blk_t *b, int32_t lv)
{
    int32_t c = e->cut + b->kbd * ((int32_t)v->note - 48), lo, tgt;
    const int32_t depth = b->velsw ? vs_mul_q15(b->depth, VS_FLT_VEL_Q15[v->vel & 127u]) : b->depth;
    lo = c - 137626;                            /* base - 2.1 */
    tgt = vs_clamp(c + depth, KNOB(-2), KNOB(125));
    c = lo + vs_mul_q30(tgt - lo, lv);
    if (b->r_cut && b->lm && c < KNOB(125))     /* an LFO: toward 125 by its unipolar sum (held to 1) of the room left */
        c += (int32_t)(((int64_t)(KNOB(125) - c) * b->lm->cut) >> 28);
    return vs_clamp(c, 0, KNOB(127) - 1);
}
static int32_t svf_coef(int32_t c)              /* the SVF's f at c, Q30 */
{
    const uint32_t idx = (uint32_t)c >> 14, fr = (uint32_t)c & 16383u;
    return (int32_t)VS_SVF_F_Q30[idx] + (int32_t)(((int64_t)((int32_t)VS_SVF_F_Q30[idx + 1u] - (int32_t)VS_SVF_F_Q30[idx]) * (int32_t)fr) >> 14);
}
static void lad_coef(const blk_t *b, int32_t c, int32_t *a, int32_t *k)   /* LP24's a (Q31) and k (Q16) at c */
{
    const uint32_t idx = (uint32_t)c >> 14, fr = (uint32_t)c & 16383u;
    const int32_t comp = (int32_t)VS_LAD_COMP_Q15[idx] + ((((int32_t)VS_LAD_COMP_Q15[idx + 1u] - (int32_t)VS_LAD_COMP_Q15[idx]) * (int32_t)fr) >> 14);
    *a = (int32_t)VS_LAD_A_Q31[idx] + (int32_t)(((int64_t)((int32_t)VS_LAD_A_Q31[idx + 1u] - (int32_t)VS_LAD_A_Q31[idx]) * (int32_t)fr) >> 14);
    *k = (int32_t)(((int64_t)b->lad_kr * comp) >> 15);
}

/* PORTAMENTO's step, every 16 frames: gap <- max(|gap| a - aim (1 - a), 0), a = e^(-16 / (tau fs)) */
static void glide_step(vs_voice_t *v, const blk_t *b)
{
    int32_t g = v->gr < 0 ? -v->gr : v->gr, rest;
    if (!g) return;
    rest = (int32_t)(((int64_t)g * b->porta_a - (int64_t)v->gaim * ((int64_t)0x80000000u - b->porta_a)) >> 31);
    if (rest < 0) rest = 0;
    v->gr = v->gr < 0 ? -rest : rest;
}

static void voice_block(vs_engine_t *e, vs_voice_t *v, const blk_t *b, int32_t *outc, int32_t *outl, int32_t *outr, uint32_t n)
{
    int32_t o1[VS_CTL], o2[VS_CTL], tmp[VS_CTL], *out, kstep, kstep2, fstep2, base_c, osc2_c, c, f_end, fstep, g_amp, gk;
    uint32_t ns, k, fk, off = 0;
    int32_t lv0;
    seg_t seg[2];

    /* ---- pitch: only when the note, the patch's pitch values or the bend moved */
    if (!b->porta) v->gr = 0;
    base_c = (int32_t)v->note * 100 + b->shift_c + v->usemis * 100 + (v->uc8 >> 8);   /* (UNISON's pitch and detune) */
    osc2_c = (b->track ? base_c : 5700 + b->tune_c) + b->o2_c;
    /* a glide plays in whole 1/128 octaves: the note + ceil(gap) of them */
    gk = v->gr >= 0 ? (v->gr + 65535) >> 16 : -((-v->gr + 65535) >> 16);
    if (v->pitch_key[0] != (uint32_t)base_c || v->pitch_key[1] != (uint32_t)osc2_c || v->pitch_key[2] != (uint32_t)b->bend_steps
        || v->pitch_key[3] != (uint32_t)gk) {
        int32_t gc = gk >= 0 ? (gk * 75 + 4) / 8 : -((-gk * 75 + 4) / 8);   /* the glide in cents (9.375 a step), for the tables */
        int32_t t1 = (base_c + gc + b->bend_c + 50 + 120000) / 100 - 1200;
        int32_t t2 = (osc2_c + (b->track ? gc : 0) + b->bend_c + 50 + 120000) / 100 - 1200;
        v->pitch_key[0] = (uint32_t)base_c; v->pitch_key[1] = (uint32_t)osc2_c; v->pitch_key[2] = (uint32_t)b->bend_steps;
        v->pitch_key[3] = (uint32_t)gk;
        v->inc1b = cents_frac(vs_pitch_steps(vs_pitch_inc(base_c), gk), (uint32_t)v->uc8 & 255u);
        v->inc2b = b->track ? cents_frac(vs_pitch_steps(vs_pitch_inc(osc2_c), gk), (uint32_t)v->uc8 & 255u) : vs_pitch_inc(osc2_c);
        v->inc1 = vs_pitch_steps(v->inc1b, b->bend_steps) & VS_OSC1_PHASE_MASK;
        v->inc2 = vs_pitch_steps(v->inc2b, b->bend_steps);
        v->mute1 = t1 > 127; v->mute2 = t2 > 127;                    /* no table above note 127 */
        v->tn1 = (uint8_t)vs_clamp(t1, 0, 127); v->tn2 = (uint8_t)vs_clamp(t2, 0, 127);
    }

    /* ---- silent and waiting for its end at the 256-frame boundary: only the oscillators run on */
    if (v->aenv.stage == VS_ENV_IDLE && v->ag == 0) {
        v->p1 += v->inc1 * n; v->p2 += v->inc2 * n; v->ps += (b->sub_oct ? v->inc1 >> 1 : v->inc1) * n;
        if (b->det) { copies_steps(v, b, v->inc1); for (k = 0; k < 8u; k++) v->cs[k] += v->cinc[k] * n; }
        if (((e->bpos + n) & (GRID - 1u)) == 0u) v->active = 0;
        glide_step(v, b);
        return;
    }

    /* ---- the oscillators: FM, RING, SYNC or the mod env on, a frame at a time; else each its own loop */
    if (b->cross) {
        mod_t mo = {0, 0, 0, 0};
        uint32_t i1v[VS_CTL], i2v[VS_CTL], wv[VS_CTL], dv[VS_CTL], f;
        const lfo_mod_t *lm = b->lm;
        const int menv = b->menv_on && n == VS_CTL, mp = menv && b->menv_dest == 0u, mf = menv && b->menv_dest == 1u,
                  mw = menv && b->menv_dest == 2u;
        int32_t lv[VS_CTL];
        uint8_t sv[VS_CTL];
        if (menv) menv_frames(&v->menv, &b->mcfg, lv, sv);   /* the mod env a frame at a time: an instant attack in one
                                                                frame, and OSC2's phase integrates any error in its pitch */
        if (b->menv_on) vs_env_step(&v->menv, &b->mcfg);
        if (lm && b->r_p1) {                    /* OSC1+2: OSC1's steps (whole 1/128 octaves on the bend's) */
            int32_t last = 0x7FFFFFFF;
            uint32_t inc = 0;
            for (f = 0; f < n; f++) {
                if (lm->sa[f] != last) { last = lm->sa[f]; inc = vs_pitch_steps(v->inc1b, b->bend_steps + last) & VS_OSC1_PHASE_MASK; }
                i1v[f] = inc;
            }
            mo.i1 = i1v;
        }
        if ((lm && b->r_p2) || mp) {            /* OSC2: both LFOs' steps on the bend's, then the mod env's ratio */
            int32_t last = 0x7FFFFFFF;
            uint32_t inc = v->inc2;
            for (f = 0; f < n; f++) {
                if (lm && b->r_p2 && lm->sa[f] + lm->s2[f] != last) {
                    last = lm->sa[f] + lm->s2[f];
                    inc = vs_pitch_steps(v->inc2b, b->bend_steps + last);
                }
                i2v[f] = mp ? menv_pitch(inc, lv[f], b->menv_oct) : inc;
            }
            mo.i2 = i2v;
        }
        if ((lm && b->r_pw) || mw) {
            for (f = 0; f < n; f++) wv[f] = pw_at(b, lm ? lm->pw[f] : 0, mw ? lv[f] : 0, mw);
            mo.w = wv;
            if (n) { e->pw_live = wv[n - 1]; e->pw_seq++; }   /* (the screen's; any voice will do) */
        }
        if ((lm && b->r_fm) || mf) {
            for (f = 0; f < n; f++) dv[f] = fm_at(b, lm ? lm->fm[f] : 0, mf ? lv[f] : 0, mf ? sv[f] : 0u, mf);
            mo.d = dv;
        }
        /* only pitch or P/W moving (OSC1's pitch: without its copies or the SUB): each oscillator its own loop */
        if (!b->ring && !b->sync && !b->fm_dev && !mo.d && !(mo.i1 && (b->det || b->sub_on))) osc_plain(v, b, o1, o2, n, &mo);
        else osc_cross(v, b, o1, o2, n, &mo);
    }
    else osc_plain(v, b, o1, o2, n, 0);

    /* ---- the cutoff at the block's end, in knob units; f ramps to it. When the filter env's attack lands inside the
       block (an attack of 0: the first frame), f ramps to the peak there first, then on to the end */
    lv0 = v->fenv.level;                        /* (a fresh voice ramps from the env's start, not from the block's end) */
    fk = vs_env_step(&v->fenv, &b->fcfg);
    c = cut_knob(e, v, b, v->fenv.level);
    kstep = kstep2 = 0;
#if VS_LPDL
    if (b->ftype == 4u) {                       /* LPDL: its coefficients when the cutoff moved 1/8 knob or the res moved */
        const int32_t res = vs_clamp(e->ev[VS_P_FLT_RES], 0, 127), dc = c - v->dcut;
        if (res != v->dres || dc >= 8192 || dc <= -8192) lpdl_coef(v, c, res);
        fstep = fstep2 = 0;
        fk = n;
    } else
#endif
#if VS_SH101
    if (b->ftype == 5u) {                       /* SH-101: its coefficients at the block's end (cached while unchanged);
                                                   FREQ on the SH-101's own slider scale, RES 0..127 = its slider 0..10 */
        sh101_coef(&v->shc, sh101_knob_cut(c), sh101_panel_res(vs_clamp(e->ev[VS_P_FLT_RES], 0, 127)));
        fstep = fstep2 = 0;
        fk = n;
    } else
#endif
    if (b->ftype == 1u) {                       /* LP24: a stage's a, and k = (0.0297 res - 0.076) x comp, ramped */
        int32_t a_end, k_end;
        lad_coef(b, c, &a_end, &k_end);
        if (v->la < 0) lad_coef(b, cut_knob(e, v, b, lv0), &v->la, &v->lk);
        if (fk < n) {
            int32_t a_pk, k_pk;
            lad_coef(b, cut_knob(e, v, b, 1 << 30), &a_pk, &k_pk);
            fstep = (a_pk - v->la) / (int32_t)fk; kstep = (k_pk - v->lk) / (int32_t)fk;
            fstep2 = (a_end - a_pk) / (int32_t)(n - fk); kstep2 = (k_end - k_pk) / (int32_t)(n - fk);
        } else { fstep = fstep2 = (a_end - v->la) / (int32_t)n; kstep = kstep2 = (k_end - v->lk) / (int32_t)n; }
    } else {
        f_end = svf_coef(c);
        if (v->f < 0) v->f = svf_coef(cut_knob(e, v, b, lv0));   /* a fresh voice: from the env's start, not a stale value */
        if (fk < n) {
            const int32_t f_pk = svf_coef(cut_knob(e, v, b, 1 << 30));
            fstep = (f_pk - v->f) / (int32_t)fk;
            fstep2 = (f_end - f_pk) / (int32_t)(n - fk);
        } else fstep = fstep2 = (f_end - v->f) / (int32_t)n;
    }
    if (fk > n) fk = n;

    /* ---- the velocity law, when the velocity or AMP VEL moved: 10^(-dB / 20), dB = 15 (sens / 64) (127 - vel) / 126 */
    if (v->vel_key != (uint16_t)(b->sens << 8 | v->vel)) {
        uint32_t x = b->sens * (127u - (v->vel & 127u)) * 30720u / 8064u;   /* dB x 8, Q8 */
        uint32_t i8 = x >> 8;
        v->vel_key = (uint16_t)(b->sens << 8 | v->vel);
        v->vel_g = i8 >= 240u ? (int32_t)VS_DB_GAIN_Q15[240] : (int32_t)VS_DB_GAIN_Q15[i8] + ((((int32_t)VS_DB_GAIN_Q15[i8 + 1u] - (int32_t)VS_DB_GAIN_Q15[i8]) * (int32_t)(x & 255u)) >> 8);
    }
    g_amp = (b->gain * v->vel_g) >> 15;

    /* ---- the amp gain's pieces, and the audio: to the centre bus, or (a UNISON voice with a pan) to a side's each,
       by its balance gains min(1, 1 -+ pan) */
    if (v->upan) for (k = 0; k < n; k++) tmp[k] = 0;
    out = v->upan ? tmp : outc;
    ns = amp_plan(v, &b->acfg, e->bpos & (GRID - 1u), n, seg);
    {   /* the block in pieces: the amp gain's (up to two) and the filter's (up to two), each with its own steps */
        uint32_t ai = 0, aleft = seg[0].n, fleft = fk, m;
        int32_t fs = fstep, ks = kstep;
        while (off < n) {
            const int32_t *a = o1 + off, *bb = o2 + off, *am = b->r_amp && b->lm ? b->lm->amp + off : 0;
            int32_t *o = out + off;
            while (!aleft && ai + 1u < ns) aleft = seg[++ai].n;
            if (!fleft) { fleft = n - off; fs = fstep2; ks = kstep2; }
            m = aleft < fleft ? aleft : fleft;
            if (!m) break;
            if (b->sat_num) {
                if (b->ftype == 0u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 0u, 1);
                else if (b->ftype == 2u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 2u, 1);
                else if (b->ftype == 1u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 1u, 1);
#if VS_LPDL
                else if (b->ftype == 4u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 4u, 1);
#endif
#if VS_SH101
                else if (b->ftype == 5u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 5u, 1);
#endif
                else voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 3u, 1);
            } else {
                if (b->ftype == 0u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 0u, 0);
                else if (b->ftype == 2u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 2u, 0);
                else if (b->ftype == 1u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 1u, 0);
#if VS_LPDL
                else if (b->ftype == 4u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 4u, 0);
#endif
#if VS_SH101
                else if (b->ftype == 5u) voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 5u, 0);
#endif
                else voice_out(v, b, a, bb, o, m, fs, ks, seg[ai].step, g_amp, am, 3u, 0);
            }
            off += m; aleft -= m; fleft -= m;
        }
    }
    if (v->upan) {
        const int32_t gl = v->upan > 0 ? 65536 - v->upan : 65536, gr = v->upan < 0 ? 65536 + v->upan : 65536;
        for (k = 0; k < n; k++) {
            outl[k] += (int32_t)(((int64_t)tmp[k] * gl) >> 16);
            outr[k] += (int32_t)(((int64_t)tmp[k] * gr) >> 16);
        }
    }
    if (v->a_left <= 0 && v->aenv.stage != VS_ENV_ATTACK) v->ag = v->aenv.level;   /* following: on the envelope exactly */
    if (v->ag < 0) v->ag = 0;
    if (v->aenv.stage == VS_ENV_IDLE && ((e->bpos + n) & (GRID - 1u)) == 0u) { v->active = 0; v->ag = 0; }
    glide_step(v, b);
}

/* ---- the CTRL routes (Synth::ApplyRouting): the patch's values with what the two MIDI controller routes add.
 * SRC is a status word (0xB0nn CC nn, 0xE000 the bend, 0xD000 channel pressure), ASSIGN a parameter whose range is
 * 0..127 (others are not routed), SENS d = sens - 64 above 64, sens - 63 below 63 (63, 64: off). The offset is
 * round(v d 128 / (63 x 127)) knob units, v the controller 0..127 (the bend: (bend - 8192) / 64, -128..128) */
static void routes_apply(vs_engine_t *e)
{
    static const uint8_t SRC[2] = {VS_P_CTRL_SRC1, VS_P_CTRL_SRC2}, DST[2] = {VS_P_CTRL_DST1, VS_P_CTRL_DST2},
                         SENS[2] = {VS_P_CTRL_SENS1, VS_P_CTRL_SENS2};
    uint32_t k;
    for (k = 0; k < VS_NPARAM; k++) e->ev[k] = e->patch.v[k];
    for (k = 0; k < 2u; k++) {
        const int32_t t = e->patch.v[DST[k]], w = e->patch.v[SRC[k]], sens = e->patch.v[SENS[k]];
        const int32_t d = sens >= 64 ? sens - 64 : sens <= 62 ? sens - 63 : 0;
        int32_t num, off;
        if (t < 0 || t >= (int32_t)VS_NPARAM || VS_PARAMS[t].min != 0 || VS_PARAMS[t].max != 127 || w < 0) continue;
        switch ((w >> 8) & 0xF0) {
        case 0xB0: num = (int32_t)e->cc[w & 0x7F] * d * 128; break;
        case 0xE0: num = e->bend14 * d * 2; break;          /* (bend / 64) x d x 128 */
        case 0xD0: num = (int32_t)e->pressure * d * 128; break;
        default: num = 0; break;
        }
        off = num >= 0 ? (2 * num + 8001) / 16002 : -((-2 * num + 8001) / 16002);   /* round half away from 0 of num / 8001 */
        e->ev[t] = vs_clamp(e->ev[t] + off, 0, 127);
    }
}

/* ---- the LFOs: a control block of both (they run on when off, so their phases stay free), what they move on every
 * voice (lm), and the bus's pan gains a frame (Q16, the patch's PAN in them) */
static void lfo_block(vs_engine_t *e, const blk_t *b, lfo_mod_t *lm, int32_t pan[2][VS_CTL], uint32_t n)
{
    const int32_t *p = e->ev;
    int32_t lv[2][VS_CTL], st[2][VS_CTL], q, pl, pr;
    uint32_t k, i;
    for (k = 0; k < 2u; k++) {
        const int32_t dest = p[k ? VS_P_LFO2_DEST : VS_P_LFO1_DEST], on = p[k ? VS_P_LFO2_ON : VS_P_LFO1_ON];
        const uint32_t inc = vs_lfo_inc((uint32_t)vs_clamp(p[k ? VS_P_LFO2_SPEED : VS_P_LFO1_SPEED], 0, 127),
                                        p[k ? VS_P_LFO2_TEMPO : VS_P_LFO1_TEMPO] != 0, e->bpm_q8);
        const int pitch = on && dest <= 2;
        vs_lfo_render(&e->lfo[k], n, (uint32_t)vs_clamp(p[k ? VS_P_LFO2_TYPE : VS_P_LFO1_TYPE], 0, 5), inc, lv[k],
                      pitch ? st[k] : 0, (int32_t)VS_LFO_STEPS[vs_clamp(p[k ? VS_P_LFO2_DEPTH : VS_P_LFO1_DEPTH], 0, 127)],
                      dest == 4);
    }
    for (i = 0; i < n; i++) {
        int32_t sa = 0, s2 = 0, pw = 0, fm = 0, cu = 0, am = 32768, ps = 0;
        for (k = 0; k < 2u; k++) {
            const int32_t sc = b->lfo_sc[k], v = lv[k][i];
            const int32_t sv = (int32_t)(((int64_t)sc * v) >> 18), su = (int32_t)(((int64_t)sc * ((int64_t)v + (1 << 30))) >> 19);   /* Q28 */
            switch (b->lfo_dest[k]) {
            case 1: s2 += st[k][i]; break;
            case 2: sa += st[k][i]; break;
            case 3: cu += su; break;
            case 4: am = (int32_t)(((int64_t)am * (32768 - (int32_t)(((int64_t)sc * (((int64_t)1 << 30) - v)) >> 32))) >> 15); break;
            case 5: pw += sv; break;
            case 6: fm += su; break;
            case 7: ps += sv; break;
            default: break;
            }
        }
        lm->sa[i] = sa; lm->s2[i] = s2; lm->pw[i] = pw; lm->fm[i] = fm; lm->amp[i] = am;
        lm->cut = cu > (1 << 28) ? 1 << 28 : cu;
        /* PAN: an LFO's position q = 1 - (1 - s)^3 for s >= 0 (odd; + is left; s unclamped), its gains 1 + |q| / 2
           toward that side, 1 - |q| away; times the patch's, q = (PAN - 64) / 64 below 64, / 63 above, min(1, 1 -+ q) */
        if (b->r_pan) {
            int64_t u = (1 << 28) - (ps < 0 ? -ps : ps), c = (((u * u) >> 28) * u) >> 28, qq = ps >= 0 ? (1 << 28) - c : c - (1 << 28);
            pan[0][i] = (int32_t)((qq >= 0 ? (1 << 28) + (qq >> 1) : (1 << 28) + qq) >> 12);
            pan[1][i] = (int32_t)((qq >= 0 ? (1 << 28) - qq : (1 << 28) - (qq >> 1)) >> 12);
        } else pan[0][i] = pan[1][i] = 65536;
    }
    q = vs_clamp(p[VS_P_PAN], 0, 127) - 64;
    q = q < 0 ? q * 1024 : (q * 65536 + 31) / 63;   /* Q16 */
    pl = q > 0 ? 65536 - q : 65536; pr = q < 0 ? 65536 + q : 65536;
    if (pl != 65536 || pr != 65536)
        for (i = 0; i < n; i++) {
            pan[0][i] = (int32_t)(((int64_t)pan[0][i] * pl) >> 16);
            pan[1][i] = (int32_t)(((int64_t)pan[1][i] * pr) >> 16);
        }
}

/* ---- the second core (vs_dsp.c on the chip: Melodee's audio worker). Each block, every other sounding voice goes to
 * CPU1, which sums them into the job's own lines while CPU0 renders the rest; CPU0 then adds them. The sums are
 * integers, so the output is the same as one core's, whatever the split. A voice belongs to CPU1 only between the
 * submit and the join; voice_block reads the engine and the block, writes only its voice and the lines it is given.
 * Without a second core (the host, a retired CPU1) the submit says no and CPU0 renders them all. VS_PAR_TEST (the
 * host's check) runs the job at once in the submit, to take the split path */
#ifndef VS_PAR_SUBMIT
#ifdef VS_PAR_TEST
#define VS_PAR_SUBMIT(fn, ctx) ((fn)(ctx), 1)
#else
#define VS_PAR_SUBMIT(fn, ctx) ((void)(fn), (void)(ctx), 0)
#endif
#define VS_PAR_JOIN() 1
#define VS_PAR_CHECK() ((void)0)
#endif
typedef struct {
    vs_engine_t *e;
    const blk_t *b;
    uint32_t m, n;
    uint8_t idx[VS_NV];
    int32_t bus[VS_CTL], busl[VS_CTL], busr[VS_CTL];
} par_job_t;
static par_job_t par_job __attribute__((aligned(32)));
static void par_run(void *ctx)                  /* (CPU1) */
{
    par_job_t *j = (par_job_t *)ctx;
    uint32_t i;
    for (i = 0; i < VS_CTL; i++) j->bus[i] = j->busl[i] = j->busr[i] = 0;
    for (i = 0; i < j->n; i++) voice_block(j->e, &j->e->v[j->idx[i]], j->b, j->bus, j->busl, j->busr, j->m);
}

/* the overload guard: a buffer that took longer than its budget (vs_dsp.c VS_SHED_TICKS) drops a voice, the oldest
 * released one, else the oldest held, so the next buffers fit: a click, not an audio ISR that overruns its period */
static void voice_shed(vs_engine_t *e)
{
    vs_voice_t *pick = 0;
    uint32_t i, rel = 0;
    for (i = 0; i < VS_NV; i++) {
        vs_voice_t *v = &e->v[i];
        if (!v->active) continue;
        if (!v->gate && (!rel || e->now - v->age > e->now - pick->age)) { pick = v; rel = 1; }
        else if (!rel && v->gate && (!pick || e->now - v->age > e->now - pick->age)) pick = v;
    }
    if (pick) { voice_kill(pick); e->shed++; }
}

void vs_engine_render(vs_engine_t *e, int32_t *out, uint32_t n)
{
    uint32_t pos, i, act = 0;
    const uint32_t t_all = VS_TICKS();
    int loading = e->load_req != 0u;
    VS_PAR_CHECK();                             /* a CPU1 that faulted or misread since: one core from now */
    for (pos = 0; pos < n; ) {
        uint32_t m = n - pos < VS_CTL ? n - pos : VS_CTL;
        int32_t tgt, left, bus[VS_CTL], busl[VS_CTL], busr[VS_CTL];
        uint32_t t0, t1;
        blk_t b;
        lfo_mod_t lm;
        int32_t pan[2][VS_CTL];
        routes_apply(e);
        blk_make(e, &b);
        if ((e->ev[VS_P_ARP_ON] != 0) != e->arp_on) { e->arp_on = (uint8_t)(e->ev[VS_P_ARP_ON] != 0); arp_switched(e, &b); }
        events(e, &b);
        arp_advance(e, &b, e->arp_due);         /* the frames since its last call */
        e->arp_due = (int32_t)m;
        lfo_block(e, &b, &lm, pan, m);
        b.lm = &lm;
        act = 0;
        for (i = 0; i < VS_NV; i++) act += e->v[i].active;
        /* FREQ glides to a new value over 50 ms, whatever the step; it snaps while nothing sounds */
        tgt = KNOB(vs_clamp(e->ev[VS_P_FLT_FREQ], 0, 127));
        if (tgt != e->cut_tgt) {
            int32_t d = tgt - e->cut;
            e->cut_tgt = tgt;
            e->cut_rate = (d < 0 ? -d : d) / 2205 * (int32_t)VS_CTL + 1;   /* |d| / (0.05 s x fs) a frame, x 16 */
        }
        left = e->cut_tgt - e->cut;
        if (!act || (left < 0 ? -left : left) <= e->cut_rate) e->cut = e->cut_tgt;
        else e->cut += left > 0 ? e->cut_rate : -e->cut_rate;
        for (i = 0; i < VS_CTL; i++) bus[i] = busl[i] = busr[i] = 0;
        t0 = VS_TICKS();
        {
            uint8_t own[VS_NV];
            uint32_t no = 0, k = 0;
            int par = 0;
            par_job.n = 0;
            for (i = 0; i < VS_NV; i++)
                if (e->v[i].active) { if (k++ & 1u) par_job.idx[par_job.n++] = (uint8_t)i; else own[no++] = (uint8_t)i; }
            if (par_job.n) { par_job.e = e; par_job.b = &b; par_job.m = m; par = VS_PAR_SUBMIT(par_run, &par_job); }
            for (k = 0; k < no; k++) voice_block(e, &e->v[own[k]], &b, bus, busl, busr, m);
            if (!par)
                for (k = 0; k < par_job.n; k++) voice_block(e, &e->v[par_job.idx[k]], &b, bus, busl, busr, m);
            else if (VS_PAR_JOIN())             /* (0: CPU1 retired mid-job; its voices' block is dropped) */
                for (i = 0; i < m; i++) { bus[i] += par_job.bus[i]; busl[i] += par_job.busl[i]; busr[i] += par_job.busr[i]; }
        }
        t1 = VS_TICKS();
        e->t_voice += t1 - t0;
        {   /* the bus (vs_bus.h): EFFECT, PAN, EQ, DELAY, CHORUS, in Q24; then the output limiter (vs_lim.h) */
            int32_t bl[VS_CTL], br[VS_CTL];
            for (i = 0; i < m; i++) { bl[i] = (bus[i] + busl[i]) * 32; br[i] = (bus[i] + busr[i]) * 32; }   /* Q19 -> Q24 */
            vs_bus_block(&e->bus, e->ev, bl, br, (const int32_t (*)[VS_CTL])pan, m, e->bpm_q8);
            e->t_bus += VS_TICKS() - t1;
            if (e->limit && m == VS_CTL) {
                int32_t lb[2u * VS_CTL];
                for (i = 0; i < m; i++) { lb[2u * i] = bl[i] >> 9; lb[2u * i + 1u] = br[i] >> 9; }
                vs_lim_block(&e->lim, lb);
                for (i = 0; i < 2u * m; i++) out[2u * pos + i] += lb[i];
            } else
                for (i = 0; i < m; i++) { out[2u * (pos + i)] += bl[i] >> 9; out[2u * (pos + i) + 1u] += br[i] >> 9; }
        }
        e->now++;
        e->bpos = (e->bpos + m) & (GRID - 1u);
        pos += m;
    }
    e->act = (uint8_t)act;
    if (VS_SHED_TICKS(n) && VS_TICKS() - t_all > VS_SHED_TICKS(n)) voice_shed(e);
    if (loading) {                              /* a program change: this buffer fades out, then the new patch */
        for (i = 0; i < n; i++) {
            int32_t g = (int32_t)(((n - 1u - i) << 15) / n);
            out[2u * i] = vs_mul_q15(out[2u * i], g);
            out[2u * i + 1u] = vs_mul_q15(out[2u * i + 1u], g);
        }
        for (i = 0; i < VS_NV; i++) voice_kill(&e->v[i]);
        e->patch = e->pending;
        vs_bus_clear(&e->bus);                  /* the delay's echoes fade out (Synth::ClearDelay) */
        e->cut = e->cut_tgt = KNOB(e->patch.v[VS_P_FLT_FREQ]);
        e->load_req = 0;
    }
}
