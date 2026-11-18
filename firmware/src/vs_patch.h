/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 patch: the 99 Synth1 parameters as native integers, exactly what a .sy1 file stores (the same index, the
 * same value), its name, and the ways in and out: get / set, the stored record, the panel's CC map.
 * Ported from Synth2's EngineCore/Params.hpp and Sy1Codec.hpp (MIT, Copyright (c) 2026 Francois Chabloz).
 *
 * Why native values: a .sy1 loads value for value and goes back value for value (docs/bank.html writes ver=112, as
 * Synth2 does, so a patch saved on the FM-1 opens in Synth2 and in Synth1), and the engine reads each value through
 * a law table built on the PC (tools/gen_tables.py from Synth2's MeasuredTables.hpp).
 *
 * What the firmware does NOT do (the PC side does, tools/sy1.py and docs/sim/bank/sy1.js, before a patch is sent):
 *   .sy1 text       parsing: name line, color=, ver=, "index,value" lines, missing lines = the original's defaults
 *   legacy versions the original's conversion of ver 100 / 105..108 patches (Synth2's ConvertLegacy and
 *                   LegacyTables.hpp); checked against Synth2's EngineCoreTests/legacy_fixture.txt
 *   names           Shift-JIS / Windows-1252 to ASCII 32..126, 16 characters (FONT_S has no others)
 * So the firmware only ever sees today's encoding. It still clamps: every value from outside goes through
 * vs_patch_sanitize (Synth2's ClampValue: the original plays an out-of-range value as the nearest in range).
 *
 * The record (vs_rec_t, 120 bytes: what a bank stores and the transfer carries, every byte 7-bit except ctl):
 *   0..15    name, ASCII, space padded
 *   16..110  the 95 parameters that are not routing words, in index order: value - min (KEY SHIFT -24..24 is 0..48,
 *            a 1-based choice is 0-based); 0x7F in byte 16 marks an EMPTY slot
 *   111..118 the 4 routing words (86..89: ctrl src1, assign1, src2, assign2) as uint16, little endian; 0xFFFF = -1
 *            (none). A source is status << 8 | cc (0xB001 the mod wheel, 0xE000 the bend, 0xD000 pressure)
 *   119      the patch's colour (a .sy1's "color=": 0 default, 1 red, 2 blue, 3 green, 4 yellow: the five Synth2's
 *            banks use; another name is kept as default)
 * 128 of them are 15360 B: one flash slot (16372 B) holds a bank.
 *
 * HOME's four knobs (Kris 18 Nov): which parameters KNOB 1..4 turn on HOME, chosen on GLO's HOME KNOBS page and kept
 * with the patch. Not in the record (it is full): a bank keeps them in a table after its records (vs_prog.h), the
 * transfer carries them as four bytes after a record (vs_xfer.h). A byte is a parameter id, or VS_HOME_DEF (0x7F):
 * that knob's default, FREQ, RES, AMOUNT, GAIN. A .sy1 has no place for them: a patch from one, from an older bank or
 * from a sender that leaves them off plays with the defaults. Any parameter but the four routing words (86..89).
 * Nothing here runs in the audio ISR. Integers only (the chip has no FPU), no 64-bit division. */
#pragma once
#include <stdint.h>
#include "vs_params.h"

#define VS_NAME 16u                             /* a name's length */
#define VS_REC 120u                             /* a stored patch */
#define VS_REC_EMPTY 0x7Fu                      /* rec.b[16]: nothing in this slot */
#define VS_NCOLOR 5u                            /* colours: default, red, blue, green, yellow */
#define VS_HOME_N 4u                            /* HOME's knobs */
#define VS_HOME_DEF 0x7Fu                       /* a stored knob byte: the default */

typedef struct {
    int32_t v[VS_NPARAM];                       /* native values (the routing words: -1 none, else 0..65535) */
    char name[VS_NAME];
    uint8_t color;
    uint8_t home[VS_HOME_N];                    /* the parameter each HOME knob turns (always a valid one: vs_home_ok) */
} vs_patch_t;

typedef struct {
    uint8_t b[VS_REC];
} vs_rec_t;

void vs_patch_init(vs_patch_t *p);                                  /* the INIT patch (VS_PARAMS[].init, "INIT") */
static inline int32_t vs_param_get(const vs_patch_t *p, uint32_t id) { return p->v[id]; }
void vs_param_set(vs_patch_t *p, uint32_t id, int32_t v);          /* clamped to min..max */
int32_t vs_param_centre(uint32_t id);          /* a VS_K_SIGNED parameter's zero: 64, UNISON PITCH 24, KEY SHIFT 0 */
uint32_t vs_patch_sanitize(vs_patch_t *p);     /* clamp every value, clean the name; returns how many were out */
void vs_patch_pack(const vs_patch_t *p, vs_rec_t *r);
int vs_patch_unpack(const vs_rec_t *r, vs_patch_t *p);              /* 0 ok (sanitized), 1 the slot is empty */
static inline int vs_rec_empty(const vs_rec_t *r) { return r->b[VS_NAME] == VS_REC_EMPTY; }

/* HOME's knobs: the default of knob k; whether id may be on one; the stored bytes into p->home (VS_HOME_DEF, NULL or
 * a bad id: the default) and back (a default knob stored as VS_HOME_DEF) */
uint8_t vs_home_default(uint32_t k);
int vs_home_ok(uint32_t id);
void vs_home_set(vs_patch_t *p, const uint8_t *b);
void vs_home_get(const vs_patch_t *p, uint8_t *b);

/* The panel's own CC map (VS_CC_PARAM, decision 9: Synth1 has none of its own): a CC value 0..127 scaled onto the
 * parameter's range, as Synth2's MIDI learn scales it (ScaledValueForCC). Returns the parameter id, or -1. */
int32_t vs_cc_apply(vs_patch_t *p, uint32_t cc, uint32_t val);
uint32_t vs_cc_value(uint32_t id, int32_t v);  /* the inverse: the CC value that vs_cc_apply turns back into v */

/* A value as the panel shows it (Synth2's ParamFormat.hpp): "LP24", "+12 st", "1/8T", "2.31 Hz", "-3", "on".
 * tempo_sync: the LFO's TEMPO switch (SPEED reads as a note value); fx_type: EFFECT LEVEL reads as feedback % on
 * the phasers. A routing source reads "CC 1", "BEND", "PRESS" or "NONE"; an assign, its parameter's label.
 * Writes at most cap bytes with the NUL (cap >= 12 holds every text); returns the length. Integers only. */
uint32_t vs_param_text(uint32_t id, int32_t v, int tempo_sync, int32_t fx_type, char *out, uint32_t cap);
/* The label of an EFFECT knob for the type playing (CTRL 1 on DECI reads "RATE"): Synth2's DisplayName. */
const char *vs_param_label(uint32_t id, int32_t fx_type);
