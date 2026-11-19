/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 voice engine: Synth2's engine (EngineCore/Synth.hpp, Voice.hpp, Filter.hpp, Envelope.hpp: MIT,
 * Copyright (c) 2026 Francois Chabloz) in C99 fixed point, played from the 99 native values of a patch.
 * Synth2 is a recreation of Synth1 measured from its sound; every law here is Synth2's (its AGENTS.md, "Engine
 * facts"), through the tables of tools/gen_tables.py. Nothing is re-measured or invented: what the chip forces to
 * differ is under "Not as Synth2", what is not ported yet under "Not yet".
 *
 * STAGE 4b (docs/design/mvsynth2-port-proposal-2026-11-13.md, section 10): the first voice, and its modulation as
 * it is ported (4b, in this order: the sub; detune; FM, ring, sync; the mod env; the LFOs; mono, legato; portamento;
 * unison; the CTRL routes).
 *
 * Signal flow, a voice:
 *   OSC1 (SINE / SAW / PULSE / TRIANGLE) + SUB (SINE / TRIANGLE / SAW / SQUARE, at OSC1's note or an octave down)
 *   and OSC2 (SAW / PULSE / TRIANGLE / NOISE) -> MIX (a linear crossfade) -> FILTER
 *   (LP12 / HP12 / BP12: the Chamberlin SVF run twice a frame with the input held; LP24: four bilinear one-poles
 *   with the feedback a frame late through an AGC; LPDL: the fitted diode ladder; SH-101 (type 5, M-Vsynth2's own:
 *   sh101_vcf.h, float, the SH-101's IR3109 VCF and its resonance circuit, its own FREQ and RES scales) -> the output
 *   limiter (not LPDL or SH-101: their feedback limits itself)
 *   -> SATURATION -> AMP (0.2 x GAIN x the velocity law x the amp env) -> the bus
 * The bus (vs_bus.h): EFFECT -> PAN (the patch's, and the LFOs': a gain a side) -> EQ (peak, TONE) -> DELAY ->
 *   CHORUS -> the output limiter (vs_lim.h: the one thing added to Synth2) -> out
 *   pitch    note + KEY SHIFT + TUNE; OSC2: + PITCH + FINE, or from note 57 with TRACK off; the bend in whole
 *            1/128-octave steps, truncated toward 0, from the wheel's top 7 bits x BEND; OSC1's step in whole
 *            2^-27 cycles. A table is chosen by the bent note, rounded; above note 127 an oscillator is silent
 *   pulse    the low part's duty P/W / 254, held in [1.25, 252.625] / 254
 *   sub      its own weight w = GAIN / (GAIN + 32) against OSC1's 1 - w; with it on, OSC2 takes OSC1's weight too
 *            and the three are scaled back to unity: OSC1 (1 - w)(1 - m), SUB w (1 - m), OSC2 (1 - w) m, over
 *            1 - w m (m = MIX). Its table: OSC1's note, or 12 below. Its square is a pulse of width 1/2. Its phase
 *            runs whether it is heard or not, and a fresh voice starts it at OSC1's (half of it an octave down)
 *   detune   8 copies of OSC1 at +-1, 3, 5, 7 x 20 DETUNE / 127 cents, read from the same tables with the phase
 *            TRUNCATED to 11 bits, no interpolation (the original's own read: its lines are part of the sound);
 *            OSC1 and its copies x 0.297 (0.288 on a sine). With the SUB on it gets 8 copies too, on the same
 *            phases at its rate. The copies' phases stand still while DETUNE is 0. At a note: OSC PHASE 0, they run
 *            on from the slot's last note; above 0, copy k starts deg_k x (v - 1) / 126 after OSC1
 *   cross    FM, RING or SYNC on: the oscillators are worked out a frame at a time, OSC2 first:
 *            FM       OSC1's step moves by a FIXED deviation a frame, beta(FM) x 261.6 Hz x OSC2 (its noise: x 0.65),
 *                     through zero; the step in whole 2^-27 cycles (truncated toward 0). The SUB and the copies
 *                     take the same deviation, not scaled by their ratios. FM 0 is no FM at all
 *            RING     OSC2 x OSC1 in OSC2's place (OSC1 after its copies, before the SUB), and no FM. With an FM
 *                     source in the patch (FM above 0) OSC2's pulse counts double and its noise at full scale;
 *                     the SUB's copies skip the stack's gain (~+10 dB, bounded by the filter's limiter)
 *            SYNC     OSC2 restarts (naive, not band-limited) whenever OSC1's phase drops: at a forward wrap at
 *                     7e-5 cycle, and at 0 on every frame FM drives OSC1 backward without wrapping. Synced noise
 *                     restarts from its fixed seed, so it repeats every OSC1 cycle
 *   mod env  attack / decay to 0 (the amp env's laws), on a note, never released; on (M.ENV) it runs and moves one
 *            destination by AMOUNT, a = (amount - 64) / 63 above 64, / 64 below, at env e:
 *            OSC2     OSC2's pitch x 2^(5 e a): +-60 semitones at e = 1, not quantized (its table stays the note's)
 *            FM       the FM knob moves toward 127 (a > 0) or 0 by the room left, m = a g: g = e in the attack, then
 *                     1 + ln e / ln 1e4 (the decay reaches FM in dB), 0 below 1e-4; beta between knobs, linear.
 *                     (So it is FM's source: with RING on and AMOUNT 67 or more, the ring takes the full scale)
 *            P/W      the low part's duty moves toward 1 (m > 0) or 0 by m of the room left on that side
 *            Its level is found a frame at a time (an instant attack lands in one frame, and OSC2's phase
 *            integrates any error in its pitch), its destination's value with it; with it on, the oscillators
 *            take the cross path. (A part block, under 16 frames, holds its destination)
 *   LFOs     two, global (vs_lfo.h); on with DEPTH above 0, each moves one destination, d = DEPTH / 127:
 *            OSC2, OSC1+2  pitch: the LFO's own whole 1/128-octave steps added to the bend's (OSC1+2: both
 *                     oscillators, OSC1's copies and the SUB with them; the tables stay the note's)
 *            FILTER   the cutoff toward knob 125 by u = d (v + 1) / 2 (held to 1) of the room left, after the env;
 *                     taken at the block's last frame (f ramps to it, as the env's)
 *            AMP      a gain 1 - d (1 - v) / 2 (smooth random takes its own form here: vs_lfo.h)
 *            P/W      the duty toward 1 (s > 0) or 0 by |s| of the room left, s = d v; then the mod env's
 *            FM       the FM knob toward 127 by u; then the mod env's (and with RING on, the ring's full scale)
 *            PAN      position q = 1 - (1 - s)^3 (odd, + is left), gains 1 + |q| / 2 toward that side, 1 - |q|
 *                     away; times the patch's PAN, q = (PAN - 64) / 64 below 64, / 63 above, min(1, 1 -+ q)
 *            Both on one destination: their steps add (pitch); s and u are the sums (FILTER, P/W, FM, PAN); the
 *            gains multiply (AMP). Pitch, P/W and FM move a frame at a time, in the cross path
 *   above note 127 an oscillator is silent (but NOISE: it has no pitch to lose)
 *   phases   OSC PHASE 0: a voice slot's oscillators run on from its last note (from the measured per-slot values
 *            at power-on). Above 0: OSC1 starts at 0, OSC2 at (v - 1) / 126 of half a cycle
 *   filter   the cutoff in knob units: FREQ (its 50 ms glide) + KBD TRACK about key 48; the env sweeps it from
 *            base - 2.1 to the target base + (AMOUNT - 64) x 1.984 (x the velocity scale with VEL SW), the target
 *            held in [-2, 125]; f = 2 sin(pi fc / (2 fs)); damping 1 - res / 127, input gain 1 - 0.7 res / 127
 *   limiter  a gain on a follower of the filter's output (3.7 ms up, 6.9 ms down), nothing below 0.39
 *   LP24     a = g / (1 + g), g = tan(pi fc / fs) from the measured cutoffs; k = max(0, 0.0297 res - 0.076) x comp;
 *            no input gain (the passband is 1 / (1 + k)); the AGC (1.2 ms up, 202 ms down) takes the feedback by
 *            (5.5 / e)^0.883 above 5.5
 *   LPDL     a linear 303-style diode ladder, 2 x oversampled trapezoid, its feedback high-passed at 100 Hz, w / k / g
 *            from the fitted tables; the input through tanh (x 1.3 / g, undone after), the feedback through
 *            tanh(2 k f) / 2. Its step x' = P x + q (u0 + u1) is solved for by elimination in Q30 (Synth2: a 5 x 5
 *            inverse in double), when the cutoff moved 1/8 knob or the resonance moved (VS_LPDL 0: it plays as LP12)
 *   amp env  outside the attack an 11-bit gain taken at every 256th frame and ramped between (as measured:
 *            Voice::AmpGain); per frame in the attack and for 256 frames after a note on. A voice ends at the
 *            first 256-frame boundary after its envelope fell silent
 * Voices: POLY. Round robin from the last voice started; at the limit (POLY, held to VS_NV and GLOBAL's VOICES) the
 *   oldest released voice goes first, then the oldest held; voices started in the same control block are not
 *   stolen (a chord over the limit drops notes). A key pressed again while held releases its voice and takes a
 *   fresh one. A stolen voice keeps its filter and envelope levels (no click). The sustain pedal holds releases.
 *   MONO / LEGATO: voice 0 plays the held-key stack (every key on goes on top; a key off, after the pedal, comes
 *   off). A key on while a voice sounds moves every sounding voice to it: MONO retriggers the envelopes (from where
 *   they are, at the new velocity) and restarts the oscillators' phases; LEGATO moves the pitch only. A key off
 *   falls back to the top held key (MONO retriggers, the velocity kept), or releases when none is held. With no
 *   voice held, a key on starts voice 0 afresh
 * PORTAMENTO: a voice glides from its own last pitch (a never-used one from note 0): every 16 frames the gap
 *   g <- max(|g| a - E (1 - a), 0), a = e^(-16 / (tau fs)), aimed E = 1.35 % of the starting gap past the note so it
 *   lands; it plays in whole 1/128 octaves (the note + ceil(g)), the tables follow it. A fresh note glides unless
 *   AUTO is on; a MONO / LEGATO note change always glides
 * UNISON: a note takes min(VOICES, the voice limit) voices (POLY: the round robin, a stack may steal from a note of
 *   the same block; MONO: voices 0..N-1). A voice's place is its slot's, i = slot % N, t = 2 i / (N - 1) - 1: it is
 *   detuned t x DETUNE's cents, panned t x (SPREAD - 64) / 64 (balance gains min(1, 1 -+ pan)), and odd places play
 *   at PITCH - 24 semitones. PHASE (with OSC PHASE above 0) moves every oscillator of the k-th voice started by its
 *   slot k's start phases x PHASE / 127
 * CTRL routes: two, each adding round(v d 128 / (63 x 127)) knob units to ASSIGN (only a 0..127 parameter): v the
 *   SRC controller 0..127 (a CC, channel pressure) or the bend / 64 (-128..128), d = SENS - 64 above 64, SENS - 63
 *   below 63. The engine plays ev[] (the patch with the routes' offsets), worked out every control block
 * A program change fades the buffer it lands in to silence, then every voice stops and the new patch plays.
 *
 * ARP (ARP ON): the held keys sorted, a step a note value of the tempo (BEAT; the tempo: bpm_q8, 120 until set),
 *   GATE / 127 of it sounding, types up & down, up, down, random over RANGE + 1 octaves; its notes play as keys do.
 *   Turned on with keys down (a program change), the held keys become its; off, its keys play voices again
 * Not yet: poly notes inside one control block take their voices in arrival order (Synth2: reversed).
 * Not as Synth2 (the chip's limits; proposal 4.9):
 *   voices    VS_NV (8), not 32
 *   arp       its steps land on the 16-frame grid (Synth2: to the frame); no drift, up to 0.36 ms late
 *   tables    a saw table every 6 semitones and a triangle every 24, read linearly; not one a note, cubic
 *   control   the filter's cutoff (glide, key track, envelope) moves every VS_CTL (16) frames, f ramped between
 *             (LPDL's coefficients: taken at those points, not ramped);
 *             Synth2 moves it every frame. Note events land on the 16-frame grid (the original's own event grid)
 *   level     an output limiter (vs_lim.h, M-vance's) is the last stage: nothing passes 0.97 of full scale. A
 *             plugin's host has headroom above it; the FM-1 has none (proposal 4.8: the one thing added)
 *
 * Formats: oscillators Q14, the filter's states and output Q20, envelopes Q30, cutoff Q16 knob units, f Q30, phases
 * Q32, the bus Q15 in int32 (as the platform takes it).
 * Threads: the main loop calls note_on / note_off / sustain / all_off / load and writes patch values (one 32-bit
 * store each: vs_param_set on eng.patch) and bend14; the audio ISR calls render. Events cross in a single-producer
 * ring, so the ISR alone owns the voices. */
#pragma once
#include <stdint.h>
#include "vs_build.h"
#include "vs_patch.h"
#include "vs_env.h"
#include "vs_lfo.h"
#include "vs_bus.h"
#include "vs_lim.h"
#if VS_SH101
#ifndef SH101_RES_TAPER
#define SH101_RES_TAPER 2.0f        /* type 5's RES knob: it sings from knob ~108 (the 101's linear slider: from 92), nearer
                                       the top as Synth1's LP24 does; the full 101 range still at 127 (sh101_vcf.h) */
#endif
#ifndef SH101_SING_TRIM
#define SH101_SING_TRIM 0.7f        /* type 5 sings -3 dB at full RES (eased in from its onset; 0.4 was -8 dB: too tame), so
                                       a 3-4 note chord stays off the output limiter (the circuit's own level, 1.0, is for a
                                       mono synth) */
#endif
#include "sh101_vcf.h"
#endif

#define VS_FS 44100u
#define VS_CTL 16u                              /* the control block (frames): Synth1's own event grid */
#define VS_EVQ 64u                              /* events in flight (a power of two) */
#define VS_OUT_SHIFT 0u                         /* (the bus was halved until the output limiter of 4d: no longer) */

typedef struct {
    uint32_t p1, p2;                            /* OSC1 / OSC2 phase, Q32 */
    uint32_t ps;                                /* the SUB's phase, Q32: locked to OSC1's at a note (its step: OSC1's, or half) */
    uint32_t cs[8], cinc[8];                    /* DETUNE: the 8 copies' phases and steps, Q32. Kept at the SUB's rate when
                                                   chalf (OSC1's copy: cs << 1), so OSC1's and the SUB's copies share them */
    uint32_t cinc_for;                          /* the OSC1 step cinc was worked out for */
    uint8_t cdet, chalf;                        /* .. and the DETUNE knob; the copies run at half rate (SUB OCTAVE) */
    uint32_t inc1, inc2;                        /* their steps (bend included) */
    uint32_t inc1b, inc2b;                      /* .. and before the bend (an LFO on pitch adds its steps to the bend's) */
    uint32_t pitch_key[4];                      /* what inc1 / inc2 were worked out for: recomputed when it moves */
    int32_t uc8, upan;                          /* UNISON: its detune (cents Q8) and pan (-1..1, Q16) in the stack */
    int8_t usemis;                              /* .. its pitch (semitones: PITCH - 24 on odd places) */
    uint8_t uk;                                 /* .. the k-th voice of the stack started (its phase shift's slot) */
    uint32_t ush;                               /* .. UNISON PHASE / 127, Q16 */
    int32_t gr, gaim;                           /* PORTAMENTO: the gap to the note, and where it aims past it, 1/128 octaves Q16 */
    uint8_t tn1, tn2;                           /* the notes their tables are chosen by (the bent pitch, rounded) */
    uint8_t mute1, mute2;                       /* above note 127: silent */
    uint32_t noise;                             /* OSC2 NOISE: the 20-bit state */
    vs_env_t aenv, fenv, menv;                  /* amp, filter, mod env (the mod env is never released: it decays to 0) */
    int32_t ag, ag_step;                        /* the amp gain now and its step a frame, Q30 */
    int32_t a_left;                             /* frames left of a 256-grid ramp (0: the gain follows the envelope) */
    uint16_t a_since;                           /* frames since the note on, held at 256 */
    uint8_t a_stage;                            /* the stage the ramp was planned in */
    int32_t low, band;                          /* SVF states, Q20 */
    int32_t lenv;                               /* the limiter's follower, Q20 */
    int32_t la, lk;                             /* LP24: a stage's a (Q31; -1: none yet) and k (Q16) reached at the block's end */
    int32_t lad[4], lfb;                        /* .. its four stages and the feedback, Q20 */
    int32_t agc, agg;                           /* .. its AGC's follower (Q20) and gain (Q16) */
#if VS_LPDL
    int32_t dd[5], dx;                          /* LPDL: y1..y4, z (Q24); the last saturated input */
    int32_t dp[5][5], dq[5], dq1[5];            /* .. a frame's P^2, a step's q, P q (lpdl_coef), Q27 */
    int32_t dk, dsc, dgo;                       /* .. k (Q16), the input's drive 1.3 / g and the output's g / drive (Q16) */
    int32_t dcut;                               /* .. the cutoff they are for (Q16 knob units) */
    int16_t dres;                               /* .. and the resonance (-1: none yet) */
#endif
#if VS_SH101
    sh101_coef_t shc;                           /* SH-101 (type 5): coefficients at the block's end (cached), and state */
    sh101_state_t shs;
#endif
    int32_t f;                                  /* SVF f reached at the end of the last block, Q30 */
    int32_t vel_g;                              /* the velocity law's gain, Q15, and what it was worked out for */
    uint16_t vel_key;
    uint8_t note, vel, gate, active;
    uint32_t age;                               /* the control block it started in */
} vs_voice_t;

typedef struct {
    vs_patch_t patch;                           /* the sound: the main loop writes values, the ISR reads */
    int32_t ev[VS_NPARAM];                      /* .. as played: with the CTRL routes' offsets (the ISR's, every block) */
    volatile uint8_t cc[128], pressure;         /* MIDI controllers, channel pressure (main loop): the CTRL routes' sources */
    volatile int32_t bend14;                    /* pitch bend, -8192..+8191 (main loop) */
    volatile uint32_t nv;                       /* voices that take new notes, 1..VS_NV (GLOBAL's VOICES; main loop) */
    volatile uint32_t evq[VS_EVQ];              /* events: type << 16 | note << 8 | velocity */
    volatile uint32_t ev_w, ev_r;
    vs_patch_t pending;                         /* a patch to load (the main loop fills it, then sets load_req) */
    volatile uint32_t load_req;                 /* 1: load pending at the next buffer (the ISR clears it) */
    vs_voice_t v[VS_NV];
    int32_t last;                               /* the voice started last (the round robin), -1 none yet */
    uint32_t now;                               /* control blocks rendered */
    uint32_t bpos;                              /* frames into the 256-frame grid */
    int32_t cut, cut_tgt, cut_rate;             /* FREQ's glide: where it is, its target, its step a block; Q16 */
    uint32_t down[4], held[4];                  /* keys down; keys the pedal holds (bits by note) */
    uint8_t pedal;
    uint8_t keys[32], nkeys;                    /* the held-key stack, the last pressed on top (MONO / LEGATO play from it) */
    uint8_t kvel[128];                          /* each key's last velocity */
    uint8_t arp_keys[32], arp_n, arp_vel, arp_on;   /* the arp: its keys (sorted), their velocity; ARP ON as last seen */
    int32_t arp_step, arp_dir, arp_count, arp_gate, arp_acc, arp_play, arp_due;   /* step, direction, frames to the next
                                                   step and gate end, the step's fraction (Q8), the note playing (-1), frames due */
    uint32_t arp_rng;                           /* RANDOM's xorshift */
    vs_bus_t bus;                               /* EFFECT, PAN, EQ, DELAY, CHORUS (vs_bus.h) */
    vs_lim_t lim;                               /* the output limiter (vs_lim.h) */
    uint8_t limit;                              /* 1: the limiter on (the firmware); 0: off (tests/vs_render.c, beside Synth2) */
    volatile uint32_t t_voice, t_bus;           /* the meter: timer ticks spent on the voices and on the bus (wrapping sums;
                                                   the main loop takes differences) */
    volatile uint32_t shed;                     /* voices the overload guard dropped (vs_voice.c voice_shed) */
    vs_lfo_t lfo[2];                            /* LFO 1, LFO 2 (global, free running) */
    volatile uint32_t bpm_q8;                   /* the tempo, beats a minute Q8 (main loop; 120 until a MIDI clock sets it) */
    volatile uint8_t act;                       /* voices sounding (the ISR writes; the screen reads) */
    volatile uint32_t pw_live, pw_seq;          /* P/W as a voice last played it while an LFO or the mod env moved it (Q32),
                                                   and a count of the blocks that did: the OSC pages draw it (issue #3) */
} vs_engine_t;

void vs_engine_init(vs_engine_t *e);                                     /* before the first render */
void vs_engine_note_on(vs_engine_t *e, uint32_t note, uint32_t vel);    /* main loop; vel 0: off */
void vs_engine_note_off(vs_engine_t *e, uint32_t note);                  /* main loop */
void vs_engine_sustain(vs_engine_t *e, int down);                        /* main loop: CC 64 */
void vs_engine_all_off(vs_engine_t *e);                                  /* main loop: CC 120 / 123, a panic */
void vs_engine_lit(const vs_engine_t *e, uint32_t on[4], uint32_t rel[4]);   /* the notes sounding: gate open / releasing */
/* main loop: a program change. 0 taken (it lands at the next buffer), -1 one is still landing: again later */
int vs_engine_load(vs_engine_t *e, const vs_patch_t *p);
void vs_engine_render(vs_engine_t *e, int32_t *out, uint32_t n);         /* ISR: n stereo frames, adds Q15 */
int vs_core1_online(void);                     /* 1: the second core renders half the voices (vs_dsp.c) */
uint32_t vs_core1_job_ticks(void);             /* CPU1 render time (wrapping tick sum); 0 without the second core */
uint32_t vs_core1_wait_ticks(void);            /* CPU0 time spent waiting at the join (wrapping tick sum) */
