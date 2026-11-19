/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 programs (proposal 5.3, decision 8): Synth1's banks of 128 numbered slots. Main loop only: never the ISR.
 *
 *   FACTORY  bank 0: firmware/gen/vs_factory.h (tools/make_bank.py from the .sy1 files of tools/bank/factory), read-only
 *   U1..U8   banks 1..VS_NBANK: 128 slots each, one platform flash slot each: the records (128 x 120 B), then HOME's
 *            knobs (128 x 4 B, vs_patch.h), then the own parameters (128 x VS_NOWN B: LFO FADE, vs_patch.h
 *            vs_own_set): 16,128 of 16,372 B. A bank saved in an older form (records and knobs, 15,872 B; records
 *            only, 15,360 B) loads with what it lacks at the defaults, and is saved in the new form the next time; a
 *            bank never saved (or unreadable: another firmware's) reads as 128 empty slots. (An older firmware reads a
 *            newer-form bank as empty)
 *   in RAM   one bank at a time (15 KB); changing bank is a flash read, no erase
 *   changes  write (a patch into a slot), clear (a slot emptied), swap (two slots): in RAM at once, the bank marked;
 *            vs_prog_flush writes it, and the caller calls it only when nothing sounds (a 4-sector erase holds the
 *            interrupts off ~200 ms: docs/API.md). Bank 0 takes no change (-1)
 *   empty    slots are real: PRESETS and program change pass them, as Synth2's arrows do
 * It is #included into mvsynth2.c (the unity build): it saves with platform.c's slot_load / slot_save. */
#pragma once
#include <stdint.h>
#include "vs_build.h"
#include "vs_patch.h"

#define VS_NPROG 128u                           /* slots a bank */

typedef struct {
    uint8_t bank;                               /* the bank in RAM: 0 FACTORY, 1..VS_NBANK */
    uint8_t dirty;                              /* 1: it has changes flash has not */
    vs_rec_t r[VS_NPROG];
    uint8_t home[VS_NPROG][VS_HOME_N];          /* HOME's knobs, a slot's four (VS_HOME_DEF: default); stored right after r */
    uint8_t own[VS_NPROG][VS_NOWN];             /* the own parameters, a slot's (vs_own_get's bytes; 0xFF: INIT); after home */
} vs_prog_t;
#define VS_PROG_STORED (sizeof(vs_rec_t) * VS_NPROG + (VS_HOME_N + VS_NOWN) * VS_NPROG)   /* r, home and own, as one */
#define VS_PROG_STORED_V2 (sizeof(vs_rec_t) * VS_NPROG + VS_HOME_N * VS_NPROG)            /* a bank from before own */
#define VS_PROG_STORED_V1 (sizeof(vs_rec_t) * VS_NPROG)                                  /* a bank from before the knobs */

void vs_prog_load(vs_prog_t *g, uint32_t bank);                 /* a bank into RAM (a fresh user bank: all empty) */
static inline int vs_prog_empty(const vs_prog_t *g, uint32_t slot) { return slot >= VS_NPROG || vs_rec_empty(&g->r[slot]); }
int vs_prog_get(const vs_prog_t *g, uint32_t slot, vs_patch_t *p);   /* 0 ok, 1 empty */
int vs_prog_write(vs_prog_t *g, uint32_t slot, const vs_patch_t *p);  /* 0 ok, -1 read-only */
/* the same from a record (checked: -2 bad); home: its four knob bytes, own: its VS_NOWN bytes, or NULL (the defaults) */
int vs_prog_write_rec(vs_prog_t *g, uint32_t slot, const vs_rec_t *r, const uint8_t *home, const uint8_t *own);
int vs_prog_clear(vs_prog_t *g, uint32_t slot);
int vs_prog_swap(vs_prog_t *g, uint32_t a, uint32_t b);
/* the next slot holding a patch from slot, dir +1 / -1, wrapping; slot itself when no other has one */
uint32_t vs_prog_next(const vs_prog_t *g, uint32_t slot, int32_t dir);
int vs_prog_flush(vs_prog_t *g);                /* the bank to flash if marked: 0 done (or nothing to do), -1 failed */
/* a bank's label: "FACTORY", "U1".."U8" */
const char *vs_prog_bank_name(uint32_t bank);
/* the name in a slot of any bank, without loading it (SAVE shows what it replaces): 1 a patch (name: 16 characters,
 * space padded), 0 empty or unreadable. The bank in RAM is read there; another, a glance at flash (slot_peek) */
int vs_prog_peek_name(const vs_prog_t *g, uint32_t bank, uint32_t slot, char name[VS_NAME]);
