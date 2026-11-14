/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2, Stage 3: the gate (docs/design/mvsynth2-port-proposal-2026-11-13.md, 3.4). Not the engine: one voice
 * loop at its dearest, as the engine will run it, so the FM-1's DEBUG figure can decide VS_NV before the engine is
 * written. It goes when Stage 4a's vs_voice.c arrives.
 *
 * A voice, a frame (the operations of Synth2's voice, EngineCore/Voice.hpp: MIT, Copyright (c) 2026 Francois
 * Chabloz; the levels and settings here are fixed and only representative):
 *   LOAD OSC      OSC1 pulse (two interpolated saw reads) + OSC2 pulse a fifth up (two more) -> mix
 *   LOAD DETUNE   + OSC1's 8 DETUNE copies as pulses: 16 truncated reads, no interpolation (Synth2's CopiesSum)
 *   LOAD SUB      + the SUB as a square an octave down (two interpolated reads) and its 8 copies (16 truncated reads)
 *   FILTER SVF    the Chamberlin SVF run twice with the input held (LP12 / HP12 / BP12), its cutoff swept by a
 *                 triangle LFO, f from the table once a control block and ramped
 *   FILTER LP24   four one-poles with the fed-back output and the AGC's follower
 *   both          the output limiter (a follower, and its gain table above the floor), the amp gain, the pan
 * Every voice sounds all the time (a measurement must not depend on what is played): voice v is 5 v semitones above
 * BASE - 12 (SPREAD 5), so 8 voices read 8 different tables, as a chord would. The keys move BASE.
 *   TABLES FLASH  the oscillator tables read where they are (firmware/gen/vs_wave.h: const, in flash)
 *   TABLES RAM    read from a copy in the pool (the saw tables: 78 KB), made at start-up. The FM-1's first figures
 *                 (Kris, 13 Nov 2026: 8 voices OSC 18 %, +DETUNE 42 %, +SUB 104 %; +SUB with 6 voices 46 %, with 7
 *                 78 %) have a knee where the tables in use pass about 32 KB: this switch shows whether that is the
 *                 flash's cache, and what a voice costs without it
 *   SPREAD        semitones between the voices, 0..7. 0: every voice on one note (three tables in all)
 *
 * Formats: phases Q32 (OSC1's step with its low 5 bits clear: Synth2's PhaseStep), audio Q14 (the tables'), f Q14,
 * gains Q15. Integers only, no 64-bit division. render runs in the audio ISR; the main loop writes nv / load /
 * filt / base / ram / spread (one 32-bit store each). */
#pragma once
#include <stdint.h>

#define VS_GATE_NV 16u                          /* voices built into the gate (the engine: VS_NV) */
#define VS_CTL 16u                              /* the control block (frames) */
enum { VS_GL_OSC, VS_GL_DETUNE, VS_GL_SUB, VS_GL_N };
enum { VS_GF_SVF, VS_GF_LP24, VS_GF_N };

typedef struct {
    uint32_t p1, ps, p2;                        /* OSC1, SUB (half OSC1's rate), OSC2: Q32 */
    uint32_t inc1, inc2;
    uint32_t cs[8], cinc[8];                    /* the copies at the SUB's rate: OSC1's copy k is cs[k] << 1 */
    const int16_t *t1, *ts, *t2;                /* the saw tables of OSC1's, the SUB's and OSC2's notes */
    int32_t low, band;                          /* SVF states, Q14 */
    int32_t st[4], fb, agc;                     /* LP24: stages, the fed-back output, the AGC's follower */
    int32_t lenv;                               /* the limiter's follower, Q14 */
} vs_gate_voice_t;

typedef struct {
    vs_gate_voice_t v[VS_GATE_NV];
    volatile uint32_t nv;                       /* voices sounding, 0..VS_GATE_NV */
    volatile uint32_t load, filt;               /* VS_GL_*, VS_GF_* */
    volatile uint32_t base;                     /* BASE, a MIDI note */
    volatile uint32_t ram;                      /* TABLES: 0 flash, 1 the pool's copy */
    volatile uint32_t spread;                   /* SPREAD, semitones between the voices (0..7) */
    uint32_t tune_live;                         /* base | spread << 8 | ram << 16 the voices are tuned to (the ISR's) */
    uint32_t lfo;                               /* the cutoff sweep's phase, Q32 */
    int32_t f;                                  /* the SVF f reached at the end of the last block, Q22 */
} vs_gate_t;

void vs_gate_init(vs_gate_t *g);
void vs_gate_render(vs_gate_t *g, int32_t *out, uint32_t n);   /* ISR: n stereo frames, adds Q15 */
