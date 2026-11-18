/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 patch (vs_patch.h): the 99 values, the stored record, the panel's CCs, what a value reads as.
 * Ported from Synth2's EngineCore/Params.hpp, MidiLearn.hpp and ParamFormat.hpp (MIT, Copyright (c) 2026 Francois
 * Chabloz). Main loop only. Integers only, no 64-bit division. */
#include "vs_patch.h"

#define REC_VAL VS_NAME                         /* the record: the value bytes start here .. */
#define REC_ROUTE (VS_NAME + VS_NREC)           /* .. the four routing words here .. */
#define REC_COLOR (REC_ROUTE + 8u)              /* .. and the colour is last */
typedef char vs_rec_size_ok[REC_COLOR + 1u == VS_REC ? 1 : -1];
static const uint8_t ROUTE_ID[4] = {VS_P_CTRL_SRC1, VS_P_CTRL_DST1, VS_P_CTRL_SRC2, VS_P_CTRL_DST2};

static int32_t clamp_id(uint32_t id, int32_t v)
{
    const vs_param_info_t *p = &VS_PARAMS[id];
    return v < p->min ? p->min : v > p->max ? p->max : v;
}

void vs_patch_init(vs_patch_t *p)
{
    static const char NAME[VS_NAME + 1u] = "INIT            ";
    uint32_t i;
    for (i = 0; i < VS_NPARAM; i++) p->v[i] = VS_PARAMS[i].init;
    for (i = 0; i < VS_NAME; i++) p->name[i] = NAME[i];
    p->color = 2u;                              /* blue, as Synth2's INIT */
    vs_home_set(p, 0);
}

/* ---- HOME's knobs (vs_patch.h) */
uint8_t vs_home_default(uint32_t k)
{
    static const uint8_t DEF[VS_HOME_N] = {VS_P_FLT_FREQ, VS_P_FLT_RES, VS_P_FLT_AMOUNT, VS_P_AMP_GAIN};
    return DEF[k < VS_HOME_N ? k : 0u];
}
int vs_home_ok(uint32_t id)
{
    return id < VS_NPARAM && VS_PARAMS[id].kind != VS_K_SRC && VS_PARAMS[id].kind != VS_K_DST;
}
void vs_home_set(vs_patch_t *p, const uint8_t *b)
{
    uint32_t k;
    for (k = 0; k < VS_HOME_N; k++) p->home[k] = b && vs_home_ok(b[k]) ? b[k] : vs_home_default(k);
}
void vs_home_get(const vs_patch_t *p, uint8_t *b)
{
    uint32_t k;
    for (k = 0; k < VS_HOME_N; k++)
        b[k] = !vs_home_ok(p->home[k]) || p->home[k] == vs_home_default(k) ? (uint8_t)VS_HOME_DEF : p->home[k];
}

void vs_param_set(vs_patch_t *p, uint32_t id, int32_t v)
{
    if (id < VS_NPARAM) p->v[id] = clamp_id(id, v);
}

int32_t vs_param_centre(uint32_t id)
{
    return id == VS_P_UNI_PITCH ? 24 : id == VS_P_KEY_SHIFT ? 0 : 64;
}

uint32_t vs_patch_sanitize(vs_patch_t *p)
{
    uint32_t i, n = 0;
    for (i = 0; i < VS_NPARAM; i++) {
        int32_t c = clamp_id(i, p->v[i]);
        if (c != p->v[i]) { p->v[i] = c; n++; }
    }
    for (i = 0; i < VS_NAME; i++)
        if (p->name[i] < 32 || p->name[i] > 126) p->name[i] = ' ';
    if (p->color >= VS_NCOLOR) p->color = 0;
    for (i = 0; i < VS_HOME_N; i++)
        if (!vs_home_ok(p->home[i])) p->home[i] = vs_home_default(i);
    return n;
}

void vs_patch_pack(const vs_patch_t *p, vs_rec_t *r)
{
    uint32_t i;
    for (i = 0; i < VS_NAME; i++) r->b[i] = (uint8_t)p->name[i] & 0x7Fu;
    for (i = 0; i < VS_NREC; i++) {
        uint32_t id = VS_REC_ID[i];
        r->b[REC_VAL + i] = (uint8_t)(clamp_id(id, p->v[id]) - VS_PARAMS[id].min);
    }
    for (i = 0; i < 4u; i++) {
        int32_t v = clamp_id(ROUTE_ID[i], p->v[ROUTE_ID[i]]);
        uint32_t w = v < 0 ? 0xFFFFu : (uint32_t)v;      /* (65535 itself is no source and no parameter: it reads back as none) */
        r->b[REC_ROUTE + 2u * i] = (uint8_t)w;
        r->b[REC_ROUTE + 2u * i + 1u] = (uint8_t)(w >> 8);
    }
    r->b[REC_COLOR] = p->color < VS_NCOLOR ? p->color : 0u;
}

int vs_patch_unpack(const vs_rec_t *r, vs_patch_t *p)
{
    uint32_t i;
    if (vs_rec_empty(r)) return 1;
    for (i = 0; i < VS_NAME; i++) p->name[i] = (char)r->b[i];
    for (i = 0; i < VS_NREC; i++) {
        uint32_t id = VS_REC_ID[i];
        p->v[id] = (int32_t)r->b[REC_VAL + i] + VS_PARAMS[id].min;
    }
    for (i = 0; i < 4u; i++) {
        uint32_t w = (uint32_t)r->b[REC_ROUTE + 2u * i] | (uint32_t)r->b[REC_ROUTE + 2u * i + 1u] << 8;
        p->v[ROUTE_ID[i]] = w == 0xFFFFu ? -1 : (int32_t)w;
    }
    p->color = r->b[REC_COLOR];
    vs_home_set(p, 0);                          /* (not in the record: the caller sets them when it has them) */
    vs_patch_sanitize(p);
    return 0;
}

/* ---- the panel's CCs: 0..127 onto min..max, linear, rounded (Synth2 ScaledValueForCC) */
static int32_t cc_scaled(uint32_t id, uint32_t val)
{
    const vs_param_info_t *p = &VS_PARAMS[id];
    return p->min + (int32_t)((val * (uint32_t)(p->max - p->min) + 63u) / 127u);
}

int32_t vs_cc_apply(vs_patch_t *p, uint32_t cc, uint32_t val)
{
    uint32_t id = cc < 128u ? VS_CC_PARAM[cc] : 0xFFu;
    if (id >= VS_NPARAM) return -1;
    p->v[id] = cc_scaled(id, val > 127u ? 127u : val);
    return (int32_t)id;
}

uint32_t vs_cc_value(uint32_t id, int32_t v)
{
    const vs_param_info_t *p;
    uint32_t range, c;
    if (id >= VS_NPARAM) return 0;
    p = &VS_PARAMS[id];
    v = clamp_id(id, v);
    range = (uint32_t)(p->max - p->min);
    if (!range) return 0;
    c = ((uint32_t)(v - p->min) * 127u + range / 2u) / range;   /* near it: then the first that scales to v */
    if (c > 127u) c = 127u;
    while (c > 0u && cc_scaled(id, c - 1u) >= v) c--;
    while (c < 127u && cc_scaled(id, c) < v) c++;
    return c;
}

/* ---- what a value reads as (Synth2 FormatParam) */
typedef struct { char *p, *end; } out_t;
static void put_c(out_t *o, char c) { if (o->p < o->end) *o->p++ = c; }
static void put_s(out_t *o, const char *s) { while (*s) put_c(o, *s++); }
static void put_u(out_t *o, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) put_c(o, t[--n]);
}
static void put_signed(out_t *o, int32_t v)     /* +3, 0, -3 */
{
    if (v > 0) put_c(o, '+');
    if (v < 0) put_c(o, '-');
    put_u(o, (uint32_t)(v < 0 ? -v : v));
}
static void put_frac(out_t *o, uint32_t v, uint32_t div)   /* v / div with the digits div has: 231, 100 -> 2.31 */
{
    put_u(o, v / div);
    put_c(o, '.');
    if (div == 100u) put_c(o, (char)('0' + v / 10u % 10u));
    put_c(o, (char)('0' + v % 10u));
}

uint32_t vs_param_text(uint32_t id, int32_t v, int tempo_sync, int32_t fx_type, char *out, uint32_t cap)
{
    out_t o;
    const vs_param_info_t *p;
    uint32_t u;
    if (!cap) return 0;
    o.p = out; o.end = out + cap - 1u;
    if (id >= VS_NPARAM) { *o.p = 0; return 0; }
    p = &VS_PARAMS[id];
    v = clamp_id(id, v);
    u = (uint32_t)(v - p->min);
    switch (id) {
    case VS_P_OSC2_PITCH: put_signed(&o, VS_OSC2_SEMIS[v]); put_s(&o, " st"); break;
    case VS_P_OSC2_FINE:
    case VS_P_OSC_TUNE: put_signed(&o, VS_FINE_CENTS[v]); put_s(&o, " ct"); break;
    case VS_P_KEY_SHIFT: put_signed(&o, v); put_s(&o, " st"); break;
    case VS_P_UNI_PITCH: put_signed(&o, v - 24); put_s(&o, " st"); break;
    case VS_P_BEND_RANGE: put_u(&o, u); put_s(&o, " st"); break;
    case VS_P_ARP_BEAT: put_s(&o, VS_ARP_BEAT[u]); break;
    case VS_P_DLY_TIME: put_s(&o, VS_DLY_TIME[u]); break;
    case VS_P_LFO1_SPEED:
    case VS_P_LFO2_SPEED:
        if (tempo_sync) put_s(&o, VS_LFO_SYNC[u * 30u / 128u]);
        else { put_frac(&o, VS_LFO_HZ[u], u < VS_LFO_TENTHS ? 100u : 10u); put_s(&o, " Hz"); }
        break;
    case VS_P_FX_LEVEL:
        if (fx_type < 6) put_u(&o, u);
        else {                                  /* the phasers: feedback in %, 64 = 0 (rounded half away from 0) */
            int32_t n = (v - 64) * 100, d = v >= 64 ? 63 : 64, a = n < 0 ? -n : n, q = (2 * a + d) / (2 * d);
            put_signed(&o, n < 0 ? -q : q);
            put_c(&o, '%');
        }
        break;
    default:
        switch (p->kind) {
        case VS_K_ENUM: put_s(&o, VS_CHOICE[p->choice + u]); break;
        case VS_K_SWITCH: put_s(&o, v ? "ON" : "OFF"); break;
        case VS_K_SIGNED: put_signed(&o, v - vs_param_centre(id)); break;
        case VS_K_SRC:
            if (v < 0) put_s(&o, "NONE");
            else if ((v & 0xF000) == 0xB000) { put_s(&o, "CC "); put_u(&o, (uint32_t)v & 127u); }
            else if ((v & 0xF000) == 0xE000) put_s(&o, "BEND");
            else if ((v & 0xF000) == 0xD000) put_s(&o, "PRESS");
            else put_u(&o, (uint32_t)v);
            break;
        case VS_K_DST:
            if (v < 0) put_s(&o, "NONE");
            else if (v < (int32_t)VS_NPARAM) put_s(&o, VS_PARAMS[v].label);
            else put_u(&o, (uint32_t)v);
            break;
        default: put_u(&o, (uint32_t)v); break;
        }
        break;
    }
    *o.p = 0;
    return (uint32_t)(o.p - out);
}

const char *vs_param_label(uint32_t id, int32_t fx_type)
{
    if (id >= VS_NPARAM) return "";
    if (id >= VS_P_FX_CTRL1 && id <= VS_P_FX_LEVEL && fx_type >= 0 && fx_type < 10)
        return VS_FX_KNOB[(uint32_t)fx_type * 3u + (id - VS_P_FX_CTRL1)];
    return VS_PARAMS[id].label;
}
