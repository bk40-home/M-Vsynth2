/* SPDX-License-Identifier: GPL-3.0-only */
/* The transfer (firmware/src/vs_xfer.h) and the banks (vs_prog.h) on this computer: a write acknowledged and read
 * back, a bad sum refused, FACTORY refused, a whole bank round trip, the 7-bit packing, an audition, a busy bank, the
 * link (LINK, EDIT reports, no echo of a PLAY, the lapse). */
#include <stdio.h>
#include <string.h>
#include "../firmware/src/vs_patch.c"
#define VS_NV 8u
static uint8_t flash[8][16384];
static int flash_ok[8];
static int slot_load(uint32_t i, void *p, uint32_t n) { if (i >= 8u || !flash_ok[i]) return -1; memcpy(p, flash[i], n); return 0; }
static int slot_save(uint32_t i, const void *p, uint32_t n) { if (i >= 8u) return -1; memcpy(flash[i], p, n); flash_ok[i] = 1; return 0; }
#include "../firmware/src/vs_prog.c"
#include "../firmware/src/vs_xfer.c"

static unsigned fails;
#define EXPECT(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static uint32_t msg(uint8_t *body, uint32_t cmd, const uint8_t *d, uint32_t len)   /* a body (between F0 and F7) */
{
    uint32_t n = 0, i, s = cmd;
    body[n++] = 0x7D; body[n++] = 0x4D; body[n++] = 0x56; body[n++] = (uint8_t)cmd;
    for (i = 0; i < len; i++) { body[n++] = d[i]; s += d[i]; }
    body[n++] = (uint8_t)((128u - (s & 127u)) & 127u);
    return n;
}
static uint32_t next(vs_xfer_t *x, vs_prog_t *g, uint8_t *out) { return vs_xfer_next(x, g, out, VS_XF_MSG_MAX); }

int main(void)
{
    static vs_prog_t g;
    static vs_xfer_t x;
    static uint8_t body[200], out[VS_XF_MSG_MAX], d[200];
    vs_patch_t p, q;
    vs_rec_t r, back;
    uint32_t n, i, k;

    {   /* the packing: every byte value, odd lengths */
        uint8_t in[120], pk[140], un[120];
        for (i = 0; i < 120u; i++) in[i] = (uint8_t)(i * 37u + 11u);
        vs_xfer_pack7(in, 120, pk);
        for (i = 0; i < VS_XF_REC7; i++) EXPECT(pk[i] < 128u, "packed bytes are 7-bit");
        vs_xfer_unpack7(pk, VS_XF_REC7, un);
        EXPECT(!memcmp(in, un, 120), "7-bit packing round trips");
    }
    vs_prog_load(&g, 0);
    vs_xfer_init(&x);
    EXPECT(!vs_prog_empty(&g, 0) && g.bank == 0, "FACTORY loads, slot 1 holds INIT");

    vs_patch_init(&p);
    memcpy(p.name, "XFER TEST       ", 16);
    p.v[VS_P_FLT_FREQ] = 33; p.v[VS_P_CTRL_SRC1] = 0xE000; p.v[VS_P_CTRL_DST1] = 19;
    vs_patch_pack(&p, &r);
    d[0] = 2; d[1] = 5; vs_xfer_pack7(r.b, VS_REC, d + 2);
    n = msg(body, VS_XF_WRITE, d, 2 + VS_XF_REC7);
    EXPECT(vs_xfer_rx(&x, &g, body, n, 1) == 1, "a WRITE is ours");
    n = next(&x, &g, out);
    EXPECT(n == 11 && out[4] == VS_XF_ACK && out[5] == VS_XF_WRITE && out[6] == 2 && out[7] == 5 && out[8] == VS_XF_OK, "WRITE acknowledged OK");
    EXPECT(g.bank == 2 && g.dirty && !vs_prog_empty(&g, 5), "the bank came into RAM, the slot holds it, marked");
    EXPECT(vs_prog_get(&g, 5, &q) == 0 && q.v[VS_P_FLT_FREQ] == 33 && q.v[VS_P_CTRL_SRC1] == 0xE000 && !memcmp(q.name, "XFER TEST", 9),
           "the patch reads back whole (the routing words too)");

    body[n - 1] ^= 1;                           /* (out of order: a fresh message, its sum broken) */
    n = msg(body, VS_XF_WRITE, d, 2 + VS_XF_REC7); body[n - 1] = (uint8_t)((body[n - 1] + 1u) & 127u);
    vs_xfer_rx(&x, &g, body, n, 1); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_BADMSG, "a bad sum is refused");

    d[0] = 0; n = msg(body, VS_XF_WRITE, d, 2 + VS_XF_REC7);
    vs_xfer_rx(&x, &g, body, n, 1); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_REFUSED && g.bank == 2, "FACTORY is read-only");

    d[0] = 3; n = msg(body, VS_XF_WRITE, d, 2 + VS_XF_REC7);
    vs_xfer_rx(&x, &g, body, n, 0); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_BUSY && g.bank == 2, "another bank while this one is unsaved and sounding: busy");
    vs_xfer_rx(&x, &g, body, msg(body, VS_XF_WRITE, d, 2 + VS_XF_REC7), 1); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_OK && g.bank == 3 && flash_ok[1], "silent: the old bank goes to flash, the new one comes in");

    d[0] = 2; d[1] = 0; d[2] = 0;               /* READ U2, all of it */
    vs_xfer_rx(&x, &g, body, msg(body, VS_XF_READ, d, 3), 1);
    for (k = 0; k < 128u; k++) {
        n = next(&x, &g, out);
        if (n != 9u + VS_XF_REC7 || out[4] != VS_XF_DATA || out[6] != k) { printf("k %u n %u cmd %x\n", k, n, out[4]); EXPECT(0, "a DATA for every slot, in order"); break; }
        vs_xfer_unpack7(out + 7, VS_XF_REC7, back.b);
        if (k == 5u) EXPECT(!memcmp(back.b, r.b, VS_REC), "slot 6 of U2 reads back as written");
        else if (!vs_rec_empty(&back)) { EXPECT(0, "the other slots of a fresh bank are empty"); break; }
    }
    EXPECT(next(&x, &g, out) == 0, "then nothing");

    vs_xfer_pack7(r.b, VS_REC, d);
    vs_xfer_rx(&x, &g, body, msg(body, VS_XF_PLAY, d, VS_XF_REC7), 1); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_OK && x.play_due && x.play.v[VS_P_FLT_FREQ] == 33, "PLAY: an audition waits for the app");

    vs_xfer_rx(&x, &g, body, msg(body, VS_XF_IDENT, d, 0), 1); n = next(&x, &g, out);
    EXPECT(out[4] == VS_XF_IDREP && !memcmp(out + 5, "M-VSYNTH2", 9), "IDENT answers with the name");

    d[0] = 2; d[1] = 5; vs_xfer_rx(&x, &g, body, msg(body, VS_XF_CLEAR, d, 2), 1); n = next(&x, &g, out);
    EXPECT(out[8] == VS_XF_OK && vs_prog_empty(&g, 5), "CLEAR empties the slot");
    EXPECT(vs_prog_next(&g, 5, 1) == 5 && vs_prog_flush(&g) == 0, "an empty bank: PRESETS stays; flush writes");

    {   /* the link (07, EDIT 17): the simulator's Link FM-1 */
        vs_patch_t now, pagep;
        vs_rec_t got;
        uint32_t ms = 1000;
        vs_patch_init(&now); memcpy(now.name, "LINKED          ", 16);
        vs_xfer_init(&x);
        EXPECT(vs_xfer_link(&x, &now, 0, ms, out, sizeof out) == 0, "not linked: no report");
        vs_xfer_rx(&x, &g, body, msg(body, VS_XF_LINK, d, 0), 1);
        EXPECT(next(&x, &g, out) == 0, "LINK: no ACK (the EDIT is the answer)");
        EXPECT(vs_xfer_link(&x, &now, 1, ms, out, sizeof out) == 0, "a sound still landing: the dump waits");
        n = vs_xfer_link(&x, &now, 0, ms, out, sizeof out);
        EXPECT(n == 7u + VS_XF_REC7 && out[4] == VS_XF_EDIT, "LINK answered with EDIT (what plays)");
        vs_xfer_unpack7(out + 5, VS_XF_REC7, got.b);
        EXPECT(vs_patch_unpack(&got, &q) == 0 && !memcmp(q.name, "LINKED", 6), "the EDIT holds the sound playing");
        EXPECT(vs_xfer_link(&x, &now, 0, ms += 100, out, sizeof out) == 0, "unchanged: no report");
        now.v[VS_P_FLT_FREQ] = 99;
        n = vs_xfer_link(&x, &now, 0, ms, out, sizeof out);
        vs_xfer_unpack7(out + 5, VS_XF_REC7, got.b); vs_patch_unpack(&got, &q);
        EXPECT(n && out[4] == VS_XF_EDIT && q.v[VS_P_FLT_FREQ] == 99, "a knob turned: reported once");
        EXPECT(vs_xfer_link(&x, &now, 0, ms += 100, out, sizeof out) == 0, "and only once");
        now.v[VS_P_FLT_FREQ] = 98;                            /* two turns close together: the gap */
        EXPECT(vs_xfer_link(&x, &now, 0, ms, out, sizeof out) != 0, "the next turn reported");
        now.v[VS_P_FLT_FREQ] = 97;
        EXPECT(vs_xfer_link(&x, &now, 0, ms + 10, out, sizeof out) == 0, "a turn within the gap waits");
        n = vs_xfer_link(&x, &now, 0, ms += 60, out, sizeof out);
        vs_xfer_unpack7(out + 5, VS_XF_REC7, got.b); vs_patch_unpack(&got, &q);
        EXPECT(n && q.v[VS_P_FLT_FREQ] == 97, "then the newest value goes");
        pagep = now; pagep.v[VS_P_FLT_FREQ] = 12;            /* the page sends a sound (PLAY) */
        vs_patch_pack(&pagep, &r); vs_xfer_pack7(r.b, VS_REC, d);
        vs_xfer_rx(&x, &g, body, msg(body, VS_XF_PLAY, d, VS_XF_REC7), 1);
        EXPECT(next(&x, &g, out) && out[8] == VS_XF_OK, "PLAY while linked: acknowledged");
        EXPECT(vs_xfer_link(&x, &now, 1, ms += 100, out, sizeof out) == 0, "the old sound is not reported while the new one lands");
        now = x.play; x.play_due = 0;                         /* (the app loads it) */
        EXPECT(vs_xfer_link(&x, &now, 0, ms += 100, out, sizeof out) == 0, "the page's own sound is not echoed back");
        vs_xfer_rx(&x, &g, body, msg(body, VS_XF_LINK, d, 0), 1);
        EXPECT(vs_xfer_link(&x, &now, 0, ms += 100, out, sizeof out) && out[4] == VS_XF_EDIT, "a renewal dumps again");
        EXPECT(vs_xfer_link(&x, &now, 0, ms += VS_XF_LINK_HOLD - 50u, out, sizeof out) == 0 && x.link_t, "linked until the hold");
        now.v[VS_P_FLT_FREQ] = 50;
        EXPECT(vs_xfer_link(&x, &now, 0, ms += 100, out, sizeof out) == 0 && !x.link_t, "no renewal: the link lapses, nothing sent");
        d[0] = 1; vs_xfer_rx(&x, &g, body, msg(body, VS_XF_LINK, d, 1), 1);
        EXPECT(next(&x, &g, out) && out[8] == VS_XF_BADMSG, "a LINK with data is refused");
    }

    printf(fails ? "vs_xfer_test: %u FAILED\n" : "vs_xfer_test: ok\n", fails);
    return fails != 0;
}
