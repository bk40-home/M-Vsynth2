/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 patch transfer: SysEx over USB, so a computer (docs/bank.html, tools/sy1_xfer.py) can send .sy1 patches
 * and whole banks into chosen slots, audition one without saving, and read every patch back as .sy1.
 * After M-vance's mv_xfer.h, but Synth1 never had a hardware SysEx to borrow, so the format is M-Vsynth2's own.
 *
 * A message: F0 7D 4D 56 cmd .. sum F7      7D: the non-commercial id; 4D 56 ("MV"): ours among its users
 *            sum: Roland's, (128 - (the bytes after 4D 56 up to the sum, added) % 128) % 128
 * Every data byte is 7-bit. A patch travels as its record (vs_patch.h, vs_rec_t) in 124 bytes: record bytes 0..110
 * and 119 as they are (already 7-bit), each routing word (16 bits) as 3 bytes, the low 7 bits first.
 *
 * To the FM-1:
 *   01 bb ss <124>   WRITE the patch into bank bb (1..VS_NBANK), slot ss (0..127). bb becomes the bank shown (loaded
 *                    first, the one before flushed); to flash when nothing sounds
 *   02 bb ss         CLEAR that slot (EMPTY)
 *   03 <124>         AUDITION: the patch plays now and is not saved (the program shows edited: SAVE keeps it)
 *   04 bb <12>       the bank's NAME (ASCII)
 *   11 bb ss nn      REQUEST nn patches (1..128) of bank bb (0 FACTORY, 1..VS_NBANK) from slot ss
 *   13               REQUEST the patch playing (edits and all)
 *   14 bb            REQUEST the bank's name
 *   1F               REQUEST the identity
 * From the FM-1:
 *   21 bb ss <124>   a patch (the answer to 11, one message a slot; an EMPTY slot: 22 bb ss)
 *   23 <124>         the patch playing
 *   24 bb <12>       a bank's name
 *   2F vv nb nv rr   the identity: firmware version (major << 4 | minor), VS_NBANK, VS_NV, the record's revision (1)
 *   7F cc ee         the reply to every message to the FM-1 that is not a request: cc the command, ee 0 taken,
 *                    1 bad sum, 2 bad frame, 3 out of range, 4 read-only, 5 busy (a flush is under way: again later)
 *
 * Flow: the sender waits for 7F (or the data it asked for) before its next message; no reply in 500 ms: it sends
 * the message again (a write is the same written twice). USB holds one received message at a time, as in M-vance.
 * "Taken" means in RAM and marked: the flash follows at the next silence (the screen says SAVING, then the bank).
 * Platform: platform.c hands the app a SysEx by its first byte; M-vance's test for Roland's 41 becomes
 * APP_SYSEX_ID (7D here). The updater's own frames always start with 00 (a 7-bit stream of 00 59 ..: ota_unpack7),
 * so the two are never confused.
 * Main loop only. */
#pragma once
#include <stdint.h>
#include "vs_prog.h"
#include "vs_voice.h"

#define VS_XF_ID 0x7Du                          /* APP_SYSEX_ID */
#define VS_XF_SIG0 0x4Du
#define VS_XF_SIG1 0x56u
#define VS_XF_PATCH 124u                        /* a record on the wire */
#define VS_XF_MSG_MAX (2u + 3u + 3u + VS_XF_PATCH + 2u)   /* the longest message, F0 .. F7 */

enum { VS_XF_NONE = 0, VS_XF_SLOT = 1, VS_XF_TEMP = 2, VS_XF_REQ = 4, VS_XF_BAD = 8, VS_XF_BANK = 16 };

typedef struct {
    uint16_t rx_n, tx_n;                        /* messages taken / sent in this transfer (the screen counts them) */
    uint8_t last_bank, last_slot;               /* where the last WRITE went (0xFF: none yet) */
    uint8_t dump_bank, dump_slot, dump_n;       /* a dump to send: from this slot, dump_n left */
    uint8_t reply[3], reply_due;                /* a 7F to send */
} vs_xfer_t;

/* one received SysEx (body: the bytes between F0 and F7). Returns VS_XF_* bits (VS_XF_BANK: the bank shown changed) */
uint32_t vs_xfer_rx(vs_xfer_t *x, vs_prog_t *g, vs_engine_t *e, const uint8_t *body, uint32_t n);
/* the next message to send (a reply, or the next of a requested dump) into out, F0 .. F7: its length, 0 none due */
uint32_t vs_xfer_next(vs_xfer_t *x, const vs_prog_t *g, const vs_engine_t *e, uint8_t *out, uint32_t cap);
