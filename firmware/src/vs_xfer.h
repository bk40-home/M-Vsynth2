/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 patch transfer (proposal 6.2, decision 11): our own SysEx, under the non-commercial id 7D with an "MV"
 * signature (Synth1 never had hardware, so there is nothing to borrow). Main loop only.
 *
 * A message: F0 7D 4D 56 cmd data.. sum F7. sum = (128 - (cmd + data bytes) mod 128) mod 128. A record (vs_rec_t, 120
 * bytes, 8 of them not 7-bit: the routing words) travels 7-bit packed, 7 bytes in 8 (the first holds their top bits,
 * byte k's in bit k): 138 bytes. bb: a bank (0 FACTORY, 1..8 U1..U8), ss: a slot 0..127.
 * hk: HOME's four knobs (vs_patch.h), four 7-bit bytes after a record: a parameter id, or 7F (that knob's default).
 * The FM-1 sends them after every record; it takes a record with them or without (without: the defaults), so a
 * sender from before them still works, and a reader from before them sees one 7-byte group too many at the end of
 * the packed record (its unpack gives 3 bytes past the 120, which it does not read).
 *
 * To the FM-1:
 *   01 bb ss rec [hk]  WRITE  the patch into the slot (bank 1..8). The bank comes into RAM if it was not; flash at
 *                          the next silence (vs_prog.h). Acknowledged
 *   02 bb ss       CLEAR   the slot emptied. Acknowledged
 *   03 rec [hk]    PLAY    an audition: the patch plays now, nothing is saved. Acknowledged
 *   04 bb ss nn    READ    nn slots from ss (nn 0: to the bank's end): one DATA each, one a main-loop pass
 *   05             IDENT   answered with IDENT
 *   06 bb ss       SELECT  the bank and the slot, as the panel would (an empty slot: the bank only). Acknowledged
 *   07             LINK    answered with EDIT (what plays now), and from then on every change to what plays is
 *                          reported as an EDIT (at most one each VS_XF_LINK_GAP ms) until VS_XF_LINK_HOLD ms after
 *                          the last LINK: a page renews it. A PLAY while linked is taken as what the page has, so
 *                          it is not reported back (the simulator's Link FM-1, sim/link.js)
 * From the FM-1:
 *   11 bb ss rec hk  DATA  a slot as stored (empty: the record's empty mark, rec[16] = 7F; hk all 7F)
 *   15 text..      IDENT   "M-VSYNTH2 " the version, then 00, VS_NV, VS_NBANK
 *   17 rec hk      EDIT    what plays now (the edit buffer), linked only (07)
 *   7F cmd bb ss st  ACK   st: 0 done, 1 a bad message (sum, length), 2 refused (FACTORY, a bank or slot out of
 *                          range), 3 busy (the bank in RAM has unsaved changes and something sounds: send it again)
 * A sender waits for each ACK (docs/bank.html, tools/sy1_xfer.py): nothing is lost, nothing waits longer than it
 * must. */
#pragma once
#include <stdint.h>
#include "vs_prog.h"
#include "vs_voice.h"

#define VS_XF_REC7 138u                         /* a record, 7-bit packed */
#define VS_XF_MSG_MAX (9u + VS_XF_REC7 + VS_HOME_N)   /* the longest, F0 .. F7: a DATA (F0 7D 4D 56 11 bb ss rec hk sum F7) */

enum { VS_XF_WRITE = 1, VS_XF_CLEAR, VS_XF_PLAY, VS_XF_READ, VS_XF_IDENT, VS_XF_SELECT, VS_XF_LINK,
       VS_XF_DATA = 0x11, VS_XF_IDREP = 0x15, VS_XF_EDIT = 0x17, VS_XF_ACK = 0x7F };
#define VS_XF_LINK_HOLD 30000u                 /* ms: a link lapses this long after the last LINK (a page renews at 20 s) */
#define VS_XF_LINK_GAP 50u                     /* ms: at most one EDIT report this often (the newest sound) */
enum { VS_XF_OK, VS_XF_BADMSG, VS_XF_REFUSED, VS_XF_BUSY };

typedef struct {
    uint16_t rx_n, tx_n;                        /* messages taken / sent (the screen counts them) */
    uint8_t ack[5], ack_due;                    /* the ACK to send (cmd bb ss st), and whether one is due */
    uint8_t ident_due;
    uint8_t rd_bank, rd_slot, rd_n;             /* a READ in progress: from where, how many left */
    int16_t select_bank, select_slot;           /* a SELECT for the app to carry out (-1: none) */
    uint8_t play_due;                           /* an audition waiting in play */
    vs_patch_t play;
    uint8_t link_req;                           /* a LINK came: the dump is due */
    uint32_t link_t, link_tx;                   /* the last LINK's ms (0: not linked), the last report's */
    vs_rec_t link_has;                          /* what the page has (the last EDIT sent, or the PLAY it sent) */
    uint8_t link_home[VS_HOME_N];               /* .. and its HOME knobs (vs_home_get's bytes) */
} vs_xfer_t;

void vs_xfer_init(vs_xfer_t *x);
/* one received SysEx (body: the bytes between F0 and F7, the 7D first). silent: nothing sounds (a bank change may
 * write the old one to flash). Returns 1 if it was ours */
int vs_xfer_rx(vs_xfer_t *x, vs_prog_t *g, const uint8_t *body, uint32_t n, int silent);
/* the next message to send into out (F0 .. F7): its length, 0 when none is due */
uint32_t vs_xfer_next(vs_xfer_t *x, vs_prog_t *g, uint8_t *out, uint32_t cap);
/* the link (07): an EDIT into out when one is due, its length, else 0. now: what plays; busy: a new sound is still
 * landing (nothing is reported then, so an old sound never goes back to the page); ms: the clock. Call it when
 * vs_xfer_next had nothing to send */
uint32_t vs_xfer_link(vs_xfer_t *x, const vs_patch_t *now, int busy, uint32_t ms, uint8_t *out, uint32_t cap);
/* the record packing, for the tests and the tools' mirror */
void vs_xfer_pack7(const uint8_t *in, uint32_t n, uint8_t *out);     /* n bytes -> n + ceil(n / 7) */
void vs_xfer_unpack7(const uint8_t *in, uint32_t n, uint8_t *out);   /* the inverse (n: the packed length) */
