/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 programs (vs_prog.h). #included into mvsynth2.c after platform.c (slot_load / slot_save). */
#include "vs_prog.h"
#include "vs_factory.h"

/* r, home and own are read and written as one block: nothing may sit between them (all bytes: no padding anywhere) */
typedef char vs_prog_home_follows[sizeof(vs_prog_t) == 2u + VS_PROG_STORED ? 1 : -1];
typedef char vs_prog_fits_slot[VS_PROG_STORED <= 16372u ? 1 : -1];

static void home_default(vs_prog_t *g)
{
    uint32_t i, k;
    for (i = 0; i < VS_NPROG; i++)
        for (k = 0; k < VS_HOME_N; k++) g->home[i][k] = VS_HOME_DEF;
}
static void own_default(vs_prog_t *g)
{
    uint32_t i, k;
    for (i = 0; i < VS_NPROG; i++)
        for (k = 0; k < VS_NOWN; k++) g->own[i][k] = 0xFFu;
}

void vs_prog_load(vs_prog_t *g, uint32_t bank)
{
    uint32_t i, k;
    g->bank = (uint8_t)(bank > VS_NBANK ? 0u : bank);
    g->dirty = 0;
    if (g->bank == 0u) {
        for (i = 0; i < VS_NPROG; i++)
            for (k = 0; k < VS_REC; k++) g->r[i].b[k] = VS_FACTORY[i][k];
        home_default(g);
        own_default(g);
        return;
    }
    if (!slot_load(g->bank - 1u, g->r, VS_PROG_STORED)) return;   /* (the length must match: an older bank fails here) */
    own_default(g);
    if (!slot_load(g->bank - 1u, g->r, VS_PROG_STORED_V2)) return;   /* a bank from before own: own at INIT */
    home_default(g);
    if (!slot_load(g->bank - 1u, g->r, VS_PROG_STORED_V1)) return;   /* a bank from before the knobs: all default */
    for (i = 0; i < VS_NPROG; i++) {            /* never saved, or not ours: empty (the load left rubbish) */
        for (k = 0; k < VS_REC; k++) g->r[i].b[k] = 0;
        g->r[i].b[VS_NAME] = VS_REC_EMPTY;
    }
}

int vs_prog_get(const vs_prog_t *g, uint32_t slot, vs_patch_t *p)
{
    if (slot >= VS_NPROG || vs_patch_unpack(&g->r[slot], p)) return 1;
    vs_home_set(p, g->home[slot]);
    vs_own_set(p, g->own[slot]);
    return 0;
}

int vs_prog_write(vs_prog_t *g, uint32_t slot, const vs_patch_t *p)
{
    if (!g->bank || slot >= VS_NPROG) return -1;
    vs_patch_pack(p, &g->r[slot]);
    vs_home_get(p, g->home[slot]);
    vs_own_get(p, g->own[slot]);
    g->dirty = 1;
    return 0;
}

int vs_prog_write_rec(vs_prog_t *g, uint32_t slot, const vs_rec_t *r, const uint8_t *home, const uint8_t *own)
{
    vs_patch_t p;
    if (!g->bank || slot >= VS_NPROG) return -1;
    if (vs_rec_empty(r)) return vs_prog_clear(g, slot);
    if (vs_patch_unpack(r, &p)) return -2;      /* (sanitized on the way: what lands is in range) */
    vs_home_set(&p, home);
    vs_own_set(&p, own);
    return vs_prog_write(g, slot, &p);
}

int vs_prog_clear(vs_prog_t *g, uint32_t slot)
{
    uint32_t k;
    if (!g->bank || slot >= VS_NPROG) return -1;
    for (k = 0; k < VS_REC; k++) g->r[slot].b[k] = 0;
    g->r[slot].b[VS_NAME] = VS_REC_EMPTY;
    for (k = 0; k < VS_HOME_N; k++) g->home[slot][k] = VS_HOME_DEF;
    for (k = 0; k < VS_NOWN; k++) g->own[slot][k] = 0xFFu;
    g->dirty = 1;
    return 0;
}

int vs_prog_swap(vs_prog_t *g, uint32_t a, uint32_t b)
{
    vs_rec_t t;
    uint32_t k;
    if (!g->bank || a >= VS_NPROG || b >= VS_NPROG) return -1;
    t = g->r[a]; g->r[a] = g->r[b]; g->r[b] = t;
    for (k = 0; k < VS_HOME_N; k++) { const uint8_t h = g->home[a][k]; g->home[a][k] = g->home[b][k]; g->home[b][k] = h; }
    for (k = 0; k < VS_NOWN; k++) { const uint8_t o = g->own[a][k]; g->own[a][k] = g->own[b][k]; g->own[b][k] = o; }
    g->dirty = 1;
    return 0;
}

uint32_t vs_prog_next(const vs_prog_t *g, uint32_t slot, int32_t dir)
{
    uint32_t k, s = slot & (VS_NPROG - 1u);
    for (k = 0; k < VS_NPROG; k++) {
        s = (s + (dir > 0 ? 1u : VS_NPROG - 1u)) & (VS_NPROG - 1u);
        if (!vs_rec_empty(&g->r[s])) return s;
    }
    return slot;
}

int vs_prog_flush(vs_prog_t *g)
{
    if (!g->dirty || !g->bank) return 0;
    if (slot_save(g->bank - 1u, g->r, VS_PROG_STORED)) return -1;
    g->dirty = 0;
    return 0;
}

const char *vs_prog_bank_name(uint32_t bank)
{
    static const char N[9][8] = {"FACTORY", "U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8"};
    return N[bank > 8u ? 0u : bank];
}

int vs_prog_peek_name(const vs_prog_t *g, uint32_t bank, uint32_t slot, char name[VS_NAME])
{
    uint8_t b[VS_NAME + 1u];                    /* the name, then the empty mark */
    uint32_t k;
    if (slot >= VS_NPROG || bank > VS_NBANK) return 0;
    if (bank == g->bank) for (k = 0; k <= VS_NAME; k++) b[k] = g->r[slot].b[k];
    else if (bank == 0u) for (k = 0; k <= VS_NAME; k++) b[k] = VS_FACTORY[slot][k];
    else if (slot_peek(bank - 1u, slot * VS_REC, b, sizeof b)) return 0;   /* (the same place in both forms) */
    if (b[VS_NAME] == VS_REC_EMPTY) return 0;
    for (k = 0; k < VS_NAME; k++) name[k] = b[k] >= 32u && b[k] <= 126u ? (char)b[k] : ' ';
    return 1;
}
