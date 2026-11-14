/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 programs (proposal 5.3, decision 8): Synth1's banks of 128 numbered slots. Main loop only: never the ISR.
 *
 *   FACTORY  bank 0: firmware/gen/vs_factory.h (tools/make_bank.py from the .sy1 files of tools/bank/factory), read-only
 *   U1..U8   banks 1..VS_NBANK: 128 slots each, one platform flash slot each (128 x 120 B = 15,360 of 16,372 B);
 *            a bank never saved (or unreadable: another firmware's, a size change) reads as 128 empty slots
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
} vs_prog_t;

void vs_prog_load(vs_prog_t *g, uint32_t bank);                 /* a bank into RAM (a fresh user bank: all empty) */
static inline int vs_prog_empty(const vs_prog_t *g, uint32_t slot) { return slot >= VS_NPROG || vs_rec_empty(&g->r[slot]); }
int vs_prog_get(const vs_prog_t *g, uint32_t slot, vs_patch_t *p);   /* 0 ok, 1 empty */
int vs_prog_write(vs_prog_t *g, uint32_t slot, const vs_patch_t *p);  /* 0 ok, -1 read-only */
int vs_prog_write_rec(vs_prog_t *g, uint32_t slot, const vs_rec_t *r);   /* the same from a record (checked: -2 bad) */
int vs_prog_clear(vs_prog_t *g, uint32_t slot);
int vs_prog_swap(vs_prog_t *g, uint32_t a, uint32_t b);
/* the next slot holding a patch from slot, dir +1 / -1, wrapping; slot itself when no other has one */
uint32_t vs_prog_next(const vs_prog_t *g, uint32_t slot, int32_t dir);
int vs_prog_flush(vs_prog_t *g);                /* the bank to flash if marked: 0 done (or nothing to do), -1 failed */
/* a bank's label: "FACTORY", "U1".."U8" */
const char *vs_prog_bank_name(uint32_t bank);
