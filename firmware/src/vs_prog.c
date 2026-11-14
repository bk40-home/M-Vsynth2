/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 programs (vs_prog.h). #included into mvsynth2.c after platform.c (slot_load / slot_save). */
#include "vs_prog.h"
#include "vs_factory.h"

void vs_prog_load(vs_prog_t *g, uint32_t bank)
{
    uint32_t i, k;
    g->bank = (uint8_t)(bank > VS_NBANK ? 0u : bank);
    g->dirty = 0;
    if (g->bank == 0u) {
        for (i = 0; i < VS_NPROG; i++)
            for (k = 0; k < VS_REC; k++) g->r[i].b[k] = VS_FACTORY[i][k];
        return;
    }
    if (slot_load(g->bank - 1u, g->r, sizeof g->r))   /* never saved, or not ours: empty (the load left rubbish) */
        for (i = 0; i < VS_NPROG; i++) {
            for (k = 0; k < VS_REC; k++) g->r[i].b[k] = 0;
            g->r[i].b[VS_NAME] = VS_REC_EMPTY;
        }
}

int vs_prog_get(const vs_prog_t *g, uint32_t slot, vs_patch_t *p)
{
    if (slot >= VS_NPROG) return 1;
    return vs_patch_unpack(&g->r[slot], p);
}

int vs_prog_write(vs_prog_t *g, uint32_t slot, const vs_patch_t *p)
{
    if (!g->bank || slot >= VS_NPROG) return -1;
    vs_patch_pack(p, &g->r[slot]);
    g->dirty = 1;
    return 0;
}

int vs_prog_write_rec(vs_prog_t *g, uint32_t slot, const vs_rec_t *r)
{
    vs_patch_t p;
    if (!g->bank || slot >= VS_NPROG) return -1;
    if (vs_rec_empty(r)) return vs_prog_clear(g, slot);
    if (vs_patch_unpack(r, &p)) return -2;      /* (sanitized on the way: what lands is in range) */
    return vs_prog_write(g, slot, &p);
}

int vs_prog_clear(vs_prog_t *g, uint32_t slot)
{
    uint32_t k;
    if (!g->bank || slot >= VS_NPROG) return -1;
    for (k = 0; k < VS_REC; k++) g->r[slot].b[k] = 0;
    g->r[slot].b[VS_NAME] = VS_REC_EMPTY;
    g->dirty = 1;
    return 0;
}

int vs_prog_swap(vs_prog_t *g, uint32_t a, uint32_t b)
{
    vs_rec_t t;
    if (!g->bank || a >= VS_NPROG || b >= VS_NPROG) return -1;
    t = g->r[a]; g->r[a] = g->r[b]; g->r[b] = t;
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
    if (slot_save(g->bank - 1u, g->r, sizeof g->r)) return -1;
    g->dirty = 0;
    return 0;
}

const char *vs_prog_bank_name(uint32_t bank)
{
    static const char N[9][8] = {"FACTORY", "U1", "U2", "U3", "U4", "U5", "U6", "U7", "U8"};
    return N[bank > 8u ? 0u : bank];
}
