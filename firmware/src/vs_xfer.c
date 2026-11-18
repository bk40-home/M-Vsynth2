/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 patch transfer (vs_xfer.h). #included into mvsynth2.c after vs_prog.c. */
#include "vs_xfer.h"

#ifndef APP_VER
#define APP_VER "dev"
#endif

void vs_xfer_pack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t i, o = 0;
    for (i = 0; i < n; i += 7u) {
        uint32_t k, top = 0, m = n - i < 7u ? n - i : 7u;
        for (k = 0; k < m; k++) top |= (uint32_t)(in[i + k] >> 7) << k;
        out[o++] = (uint8_t)top;
        for (k = 0; k < m; k++) out[o++] = in[i + k] & 0x7Fu;
    }
}
void vs_xfer_unpack7(const uint8_t *in, uint32_t n, uint8_t *out)
{
    uint32_t i = 0, o = 0;
    while (i < n) {
        uint32_t k, top = in[i++];
        for (k = 0; k < 7u && i < n; k++) out[o++] = (uint8_t)((in[i++] & 0x7Fu) | ((top >> k & 1u) << 7));
    }
}
static uint8_t xf_sum(const uint8_t *a, uint32_t n)
{
    uint32_t s = 0, i;
    for (i = 0; i < n; i++) s += a[i];
    return (uint8_t)((128u - (s & 0x7Fu)) & 0x7Fu);
}

void vs_xfer_init(vs_xfer_t *x)
{
    uint8_t *p = (uint8_t *)x;
    uint32_t i;
    for (i = 0; i < sizeof *x; i++) p[i] = 0;
    x->select_bank = x->select_slot = -1;
}

static void ack(vs_xfer_t *x, uint32_t cmd, uint32_t bb, uint32_t ss, uint32_t st)
{
    x->ack[0] = (uint8_t)cmd; x->ack[1] = (uint8_t)(bb & 0x7Fu); x->ack[2] = (uint8_t)(ss & 0x7Fu); x->ack[3] = (uint8_t)st;
    x->ack_due = 1;
}
/* the bank bb into RAM for a change: 0 ready, else an ACK status */
static uint32_t want_bank(vs_prog_t *g, uint32_t bb, int silent)
{
    if (bb == 0u || bb > VS_NBANK) return VS_XF_REFUSED;
    if (g->bank == bb) return VS_XF_OK;
    if (g->dirty && (!silent || vs_prog_flush(g))) return VS_XF_BUSY;
    vs_prog_load(g, bb);
    return VS_XF_OK;
}

int vs_xfer_rx(vs_xfer_t *x, vs_prog_t *g, const uint8_t *body, uint32_t n, int silent)
{
    const uint8_t *d;
    uint32_t cmd, len, bb = 0, ss = 0, st;
    vs_rec_t r;
    const uint8_t *hk;
    if (n < 5u || body[0] != 0x7Du || body[1] != 0x4Du || body[2] != 0x56u) return 0;   /* not ours */
    cmd = body[3]; d = body + 4; len = n - 5u;  /* data, then the sum */
    x->rx_n++;
    if (xf_sum(body + 3, n - 4u) != body[n - 1u]) { ack(x, cmd, 0, 0, VS_XF_BADMSG); return 1; }
    if (len >= 2u) { bb = d[0]; ss = d[1]; }
    switch (cmd) {
    case VS_XF_WRITE:                           /* (the knobs after the record, or not: the defaults) */
        if (len != 2u + VS_XF_REC7 && len != 2u + VS_XF_REC7 + VS_HOME_N) { ack(x, cmd, bb, ss, VS_XF_BADMSG); break; }
        if (ss >= VS_NPROG) { ack(x, cmd, bb, ss, VS_XF_REFUSED); break; }
        if ((st = want_bank(g, bb, silent)) != VS_XF_OK) { ack(x, cmd, bb, ss, st); break; }
        vs_xfer_unpack7(d + 2, VS_XF_REC7, r.b);
        hk = len > 2u + VS_XF_REC7 ? d + 2u + VS_XF_REC7 : 0;
        ack(x, cmd, bb, ss, vs_prog_write_rec(g, ss, &r, hk) ? VS_XF_BADMSG : VS_XF_OK);
        break;
    case VS_XF_CLEAR:
        if (len != 2u || ss >= VS_NPROG) { ack(x, cmd, bb, ss, VS_XF_BADMSG); break; }
        if ((st = want_bank(g, bb, silent)) != VS_XF_OK) { ack(x, cmd, bb, ss, st); break; }
        ack(x, cmd, bb, ss, vs_prog_clear(g, ss) ? VS_XF_REFUSED : VS_XF_OK);
        break;
    case VS_XF_PLAY:
        if (len != VS_XF_REC7 && len != VS_XF_REC7 + VS_HOME_N) { ack(x, cmd, 0, 0, VS_XF_BADMSG); break; }
        vs_xfer_unpack7(d, VS_XF_REC7, r.b);
        if (vs_patch_unpack(&r, &x->play)) { ack(x, cmd, 0, 0, VS_XF_BADMSG); break; }
        vs_home_set(&x->play, len > VS_XF_REC7 ? d + VS_XF_REC7 : 0);
        x->play_due = 1;
        if (x->link_t) { vs_patch_pack(&x->play, &x->link_has); vs_home_get(&x->play, x->link_home); }   /* linked: the page has it (no echo) */
        ack(x, cmd, 0, 0, VS_XF_OK);
        break;
    case VS_XF_READ:
        if (len != 3u || bb > VS_NBANK || ss >= VS_NPROG) { ack(x, cmd, bb, ss, VS_XF_BADMSG); break; }
        if (g->bank != bb) {
            if (g->dirty && (!silent || vs_prog_flush(g))) { ack(x, cmd, bb, ss, VS_XF_BUSY); break; }
            vs_prog_load(g, bb);
        }
        x->rd_bank = (uint8_t)bb; x->rd_slot = (uint8_t)ss;
        x->rd_n = (uint8_t)(d[2] == 0u || ss + d[2] > VS_NPROG ? VS_NPROG - ss : d[2]);
        break;
    case VS_XF_IDENT:
        x->ident_due = 1;
        break;
    case VS_XF_LINK:
        if (len != 0u) { ack(x, cmd, 0, 0, VS_XF_BADMSG); break; }
        x->link_req = 1;                        /* (vs_xfer_link: the dump, and the link opened or renewed) */
        break;
    case VS_XF_SELECT:
        if (len != 2u || bb > VS_NBANK || ss >= VS_NPROG) { ack(x, cmd, bb, ss, VS_XF_BADMSG); break; }
        x->select_bank = (int16_t)bb; x->select_slot = (int16_t)ss;
        ack(x, cmd, bb, ss, VS_XF_OK);
        break;
    default:
        ack(x, cmd, 0, 0, VS_XF_BADMSG);
        break;
    }
    return 1;
}

static uint32_t frame(uint8_t *out, uint32_t cmd, const uint8_t *d, uint32_t len)
{
    uint32_t n = 0, i;
    out[n++] = 0xF0u; out[n++] = 0x7Du; out[n++] = 0x4Du; out[n++] = 0x56u;
    out[n++] = (uint8_t)cmd;
    for (i = 0; i < len; i++) out[n++] = d[i] & 0x7Fu;
    out[n] = xf_sum(out + 4, n - 4u); n++;
    out[n++] = 0xF7u;
    return n;
}

uint32_t vs_xfer_next(vs_xfer_t *x, vs_prog_t *g, uint8_t *out, uint32_t cap)
{
    uint8_t d[2u + VS_XF_REC7 + VS_HOME_N];
    uint32_t n = 0;
    if (cap < VS_XF_MSG_MAX) return 0;
    if (x->ack_due) {
        n = frame(out, VS_XF_ACK, x->ack, 4u);
        x->ack_due = 0;
    } else if (x->ident_due) {
        static const char ID[] = "M-VSYNTH2 " APP_VER;
        uint32_t i;
        for (i = 0; ID[i] && i < 24u; i++) d[i] = (uint8_t)ID[i];
        d[i++] = 0; d[i++] = VS_NV; d[i++] = VS_NBANK;
        n = frame(out, VS_XF_IDREP, d, i);
        x->ident_due = 0;
    } else if (x->rd_n && g->bank == x->rd_bank) {
        d[0] = x->rd_bank; d[1] = x->rd_slot;
        vs_xfer_pack7(g->r[x->rd_slot].b, VS_REC, d + 2);
        {
            uint32_t k;
            for (k = 0; k < VS_HOME_N; k++) d[2u + VS_XF_REC7 + k] = g->home[x->rd_slot][k];
        }
        n = frame(out, VS_XF_DATA, d, 2u + VS_XF_REC7 + VS_HOME_N);
        x->rd_slot++; x->rd_n--;
    } else x->rd_n = 0;                         /* (the bank changed under the read: it ends) */
    if (n) x->tx_n++;
    return n;
}

uint32_t vs_xfer_link(vs_xfer_t *x, const vs_patch_t *now, int busy, uint32_t ms, uint8_t *out, uint32_t cap)
{
    vs_rec_t r;
    uint8_t hk[VS_HOME_N];
    uint32_t i;
    if (cap < VS_XF_MSG_MAX || busy) return 0;
    if (x->link_req) {                          /* LINK: the dump, whatever the page has */
        x->link_req = 0;
        x->link_t = ms | 1u;                    /* (never 0: 0 is "not linked") */
    } else {
        if (!x->link_t) return 0;
        if (ms - x->link_t >= VS_XF_LINK_HOLD) { x->link_t = 0; return 0; }   /* the page went quiet: unlinked */
        if (ms - x->link_tx < VS_XF_LINK_GAP) return 0;
        vs_patch_pack(now, &r);
        vs_home_get(now, hk);
        for (i = 0; i < VS_REC && r.b[i] == x->link_has.b[i]; i++) {}
        if (i == VS_REC) {
            for (i = 0; i < VS_HOME_N && hk[i] == x->link_home[i]; i++) {}
            if (i == VS_HOME_N) return 0;       /* the page has it */
        }
    }
    vs_patch_pack(now, &x->link_has);
    vs_home_get(now, x->link_home);
    {
        uint8_t d[VS_XF_REC7 + VS_HOME_N];
        vs_xfer_pack7(x->link_has.b, VS_REC, d);
        for (i = 0; i < VS_HOME_N; i++) d[VS_XF_REC7 + i] = x->link_home[i];
        x->link_tx = ms;
        x->tx_n++;
        return frame(out, VS_XF_EDIT, d, VS_XF_REC7 + VS_HOME_N);
    }
}
