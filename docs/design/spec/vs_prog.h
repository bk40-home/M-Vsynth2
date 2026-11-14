/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 programs: banks of 128 patches, as Synth1 has them (a bank folder of 001.sy1 .. 128.sy1). PRESETS
 * selects a slot of the bank shown, BANK (the SEQ button) the bank, SAVE writes. The main loop's: never the ISR.
 *
 *   FACTORY  bank 0: firmware/gen/vs_factory.h, built by tools/make_bank.py from the .sy1 files of tools/bank/factory/ (Synth2's
 *            "Synth2 Factory" bank, MIT: LICENSING.md). In flash, read-only; its 001 is the INIT patch. Slots a
 *            bank folder leaves out are EMPTY, as in Synth2
 *   U1..U8   banks 1..VS_NBANK: flash slot b - 1 each, 128 x 120 B = 15360 of its 16372 B. A slot not saved yet (or
 *            another firmware's, or unreadable) is an empty bank
 *   RAM      ONE bank, the one shown (cur: 15.4 KB): a bank change loads it (a flash read, no erase: at once).
 *            The factory bank is copied in too, so every bank reads the same way
 *   changes  write (the edited patch into a slot; also the copy to another slot), swap (two slots: the reorder),
 *            clear (a slot EMPTY): in RAM at once, the bank marked; vs_prog_flush writes it, and the caller calls
 *            it only when nothing sounds (a slot is 4 sectors: the erases hold the interrupts off ~200 ms)
 *   a bank change with unsaved changes: flushed first (the caller waits for silence; the change shows at once on
 *            the screen, the new bank plays from the moment it is loaded)
 *   empty    PRESETS and program change skip EMPTY slots (Synth2: the next non-empty slot); SAVE lists them
 * Bank names: FACTORY, and 12 characters each for U1..U8 in the settings record (the transfer sets them; default
 * "USER 1" ..). The program playing is (bank, slot), kept in the settings.
 * It is #included into mvsynth2.c (the unity build): it saves with platform.c's slot_load / slot_save. */
#pragma once
#include <stdint.h>
#include "vs_build.h"
#include "vs_patch.h"

#define VS_NSLOT 128u                           /* patches a bank */
#define VS_BANK_FACTORY 0u                      /* bank 0: read-only; 1..VS_NBANK the user banks */
#define VS_BANK_NAME 12u

typedef struct {
    vs_rec_t rec[VS_NSLOT];                     /* the bank shown (RAM copy) */
    uint8_t bank;                               /* which: 0 FACTORY, 1..VS_NBANK */
    uint8_t dirty;                              /* it changed since it was read or written */
    uint8_t from_flash;                         /* it was read from flash (else: empty, or the factory's) */
} vs_prog_t;

void vs_prog_load(vs_prog_t *g, uint32_t bank);                    /* boot, and a bank change (after a flush) */
int vs_prog_get(const vs_prog_t *g, uint32_t n, vs_patch_t *p);   /* slot n (0..127): 0 ok, 1 empty (*p untouched) */
const char *vs_prog_name(const vs_prog_t *g, uint32_t n);         /* its 16 characters (not NUL ended), 0 if empty */
int32_t vs_prog_next(const vs_prog_t *g, uint32_t n, int32_t dir); /* the next non-empty slot from n (wraps), -1 none */
int vs_prog_write(vs_prog_t *g, uint32_t n, const vs_patch_t *p); /* 0 ok, -1 the bank is read-only */
int vs_prog_swap(vs_prog_t *g, uint32_t a, uint32_t b);
int vs_prog_clear(vs_prog_t *g, uint32_t n);
int vs_prog_edited(const vs_prog_t *g, uint32_t n, const vs_patch_t *p);   /* the patch playing differs from slot n */
static inline int vs_prog_dirty(const vs_prog_t *g) { return g->dirty != 0u; }
static inline int vs_prog_readonly(const vs_prog_t *g) { return g->bank == VS_BANK_FACTORY; }
/* write the bank if marked (the caller: when nothing sounds). 0 written, 1 it was not marked, -1 the flash refused
 * (it stays marked) */
int vs_prog_flush(vs_prog_t *g);
