/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 bus (vs_bus.h). Built in vs_dsp.c at -O2.
 * Ported from Synth2's EngineCore/Effects.hpp and Synth.hpp (MIT, Copyright (c) 2026 Francois Chabloz). */
#include "vs_bus.h"
#include "vs_fixed.h"
#include "vs_tables.h"
#include "vs_bus_tables.h"
#include "vs_params.h"

#define ONE24 (1 << 24)
#define ONE30 (1 << 30)

/* the lines (the platform's pool: zeroed at boot). 16 bits at half scale: Q24 >> 10, +-2 full scale */
static int16_t dline[2][VS_DLY_LEN] __attribute__((section(".pool")));
static int16_t cline[4][VS_CHO_LEN] __attribute__((section(".pool")));

static inline int16_t to_line(int32_t x) { x >>= 10; return (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); }
static inline int32_t from_line(int16_t s) { return (int32_t)s * 1024; }
static inline int32_t bmul30(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 30); }
static inline int32_t bmul30r(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b + (1 << 29)) >> 30); }   /* rounded: recursions */
static inline int32_t bmul24(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 24); }
static inline int32_t bsat32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFF ? -0x7FFFFFFF : (int32_t)v; }

/* ---- fixed-point helpers (control rate and the comp.'s detector) */
static int32_t bus_sin(uint32_t ph)            /* sin(2 pi ph / 2^32), Q30 */
{
    uint32_t j = ph >> 22, fr = (ph >> 6) & 65535u;
    return VS_SIN_Q30[j] + (int32_t)(((int64_t)(VS_SIN_Q30[j + 1u] - VS_SIN_Q30[j]) * fr) >> 16);
}
static int32_t bus_log2(uint64_t x, int q)     /* log2(x / 2^q), Q24; x > 0 */
{
    int32_t msb = 63, lg;
    uint32_t mant, j, fr;
    while (!((x >> msb) & 1u)) msb--;
    mant = (uint32_t)(msb >= 30 ? x >> (msb - 30) : x << (30 - msb));   /* 2^30 .. 2^31 */
    j = (mant >> 22) & 255u; fr = (mant >> 6) & 65535u;
    lg = (int32_t)((VS_LOG2_Q16[j] << 8) + (int32_t)((((int64_t)VS_LOG2_Q16[j + 1u] - VS_LOG2_Q16[j]) * fr) >> 8));
    return (msb - q) * ONE24 + lg;
}
static int64_t bus_exp2(int32_t l, int q)      /* 2^(l / 2^24) in Q q (0 when it falls below) */
{
    int32_t ip = l >> 24, sh;
    uint32_t x = (uint32_t)l & 0xFFFFFFu, j = x >> 16;
    uint64_t r = VS_EXP2_Q30[j] + (uint32_t)(((uint64_t)(VS_EXP2_Q30[j + 1u] - VS_EXP2_Q30[j]) * (x & 65535u)) >> 16);
    sh = 30 - q - ip;
    if (sh >= 63) return 0;
    return sh >= 0 ? (int64_t)(r >> sh) : (int64_t)(r << (sh < -30 ? 30 : -sh));
}
static uint64_t bus_isqrt(uint64_t x)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}

/* ---- a biquad, direct form 1: Q24 signal, coefficients b0 b1 b2 a1 a2 at Q sh */
static inline int32_t bq(vs_bq_t *s, const int32_t *c, int32_t x, int sh)
{
    int64_t acc = (int64_t)c[0] * x + (int64_t)c[1] * s->x1 + (int64_t)c[2] * s->x2 - (int64_t)c[3] * s->y1 - (int64_t)c[4] * s->y2;
    int32_t y = bsat32(acc >> sh);
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}

void vs_bus_init(vs_bus_t *b)
{
    uint8_t *p = (uint8_t *)b;
    uint32_t i;
    for (i = 0; i < sizeof *b; i++) p[i] = 0;
    b->ph_dir = 1;
    b->eq_key = -1;
    b->dsmooth[0] = b->dsmooth[1] = -1;
    b->din = ONE30;
}
void vs_bus_clear(vs_bus_t *b)
{
    if (!b->dfade) b->dfade = VS_DLY_FADE;
}

/* ================================================================== EFFECT */
static int32_t ad1_curve(const int32_t *t, int32_t w)   /* w Q12 -> f(w) Q24, on u = w / (|w| + 1) */
{
    uint32_t aw = (uint32_t)(w < 0 ? -w : w), x, i;
    int32_t u = (int32_t)vs_udiv64_32((uint64_t)aw << 20, aw + 4096u);   /* |u|, Q20 */
    if (w < 0) u = -u;
    x = (uint32_t)(u + (1 << 20));              /* 0 .. 2^21: 1024 steps of 2^11 */
    i = x >> 11;
    if (i >= 1024u) return t[1024];
    return t[i] + (int32_t)(((int64_t)(t[i + 1u] - t[i]) * (int32_t)(x & 2047u)) >> 11);
}
static int32_t ad2_curve(int64_t u)             /* u Q24 */
{
    int64_t a = (u < 0 ? -u : u) * 20;          /* |u| / 0.05 */
    int32_t y;
    if (a >= ((int64_t)200 << 24)) y = VS_AD2_CURVE_Q24[200];
    else {
        uint32_t i = (uint32_t)(a >> 24), fr = (uint32_t)(a >> 8) & 65535u;
        y = VS_AD2_CURVE_Q24[i] + (int32_t)(((int64_t)(VS_AD2_CURVE_Q24[i + 1u] - VS_AD2_CURVE_Q24[i]) * fr) >> 16);
    }
    return u < 0 ? -y : y;
}
static int32_t fold(int64_t v)                  /* triangle: slope 1 through 0, +-1 at +-1, period 4; v Q24 */
{
    int64_t a = (v < 0 ? -v : v) + ONE24;
    int32_t ph = (int32_t)(a & ((4 << 24) - 1)), y = ph < (2 << 24) ? ph - ONE24 : (3 << 24) - ph;
    return v < 0 ? -y : y;
}

static void effect(vs_bus_t *b, const int32_t *p, int32_t *s0, int32_t *s1, uint32_t n)
{
    const int32_t type = vs_clamp(p[VS_P_FX_TYPE], 0, 9), c1 = vs_clamp(p[VS_P_FX_CTRL1], 0, 127);
    const int32_t c2 = vs_clamp(p[VS_P_FX_CTRL2], 0, 127), lev = vs_clamp(p[VS_P_FX_LEVEL], 0, 127);
    int32_t *s[2];
    uint32_t i, ch;
    s[0] = s0; s[1] = s1;
    if (type <= 2) {                            /* the drives: curve -> RBJ high-pass -> RBJ low-pass -> gain */
        const int32_t *lp = VS_FX_LP_Q29 + c2 * 5, *hp = type == 0 ? VS_FX_HP_AD1_Q29 : VS_FX_HP_DIST_Q29;
        const int32_t outg = type == 0 ? (int32_t)(((int64_t)VS_AD1_OUT_Q24[c1] * (lev + 2) * 130056) >> 24)   /* x (LEVEL + 2) / 129 */
                                       : (int32_t)(((uint32_t)(lev + 2) << 24) / 129u);
        for (i = 0; i < n; i++)
            for (ch = 0; ch < 2u; ch++) {
                const int32_t x = s[ch][i];
                int32_t z;
                if (type == 0) {                /* a.d.1 */
                    int32_t *h = b->ad1_h[ch], w, h1, v, h2;
                    w = (int32_t)(((int64_t)x * VS_AD1_DRIVE_Q12[c1]) >> 24)
                      - (int32_t)(((int64_t)b->ad1_fb[ch] * VS_AD1_LOOP_Q24) >> 36) + (VS_AD1_BIAS1_Q24 >> 12);   /* Q12 */
                    z = ad1_curve(VS_AD1_CURVE1_Q24, w);   /* (the tables hold f - f(bias): no constant 0xFFC.. for the ROM check) */
                    b->ad1_fb[ch] += (int32_t)(((int64_t)(z - b->ad1_fb[ch]) * VS_AD1_LPA_Q31) >> 31);
                    h1 = bmul30(z - h[0], VS_AD1_H1C_Q30) + bmul30(h[1], VS_AD1_H1D_Q30);
                    v = ad1_curve(VS_AD1_CURVE2_Q24, (int32_t)((((int64_t)h1 * VS_AD1_GAIN2_Q24) >> 24) + VS_AD1_BIAS2_Q24) >> 12);
                    h2 = bmul30(v - h[2], VS_AD1_H2C_Q30) + bmul30(h[3], VS_AD1_H2D_Q30);
                    h[0] = z; h[1] = h1; h[2] = v; h[3] = h2;
                    z = h2;
                } else if (type == 1) {
                    z = ad2_curve(((int64_t)x * VS_AD2_DRIVE_Q16[c1]) >> 16);
                } else {
                    z = bmul24(VS_DD_PEAK_Q24[c1], fold(((int64_t)x * VS_DD_RATE_Q16[c1]) >> 16));
                }
                s[ch][i] = bmul24(outg, bq(&b->lp[ch], lp, bq(&b->hp[ch], hp, z, 29), 29));
            }
        return;
    }
    if (type == 3) {                            /* deci. */
        const uint32_t period = (uint32_t)c1 + 1u;
        const int32_t q = VS_DECI_Q_Q24[c2 >> 3], m = lev >= 127 ? ONE24 : (lev << 24) / 128;
        for (i = 0; i < n; i++) {
            if (b->hold_n == 0u)
                for (ch = 0; ch < 2u; ch++) {   /* floor(x / q) q */
                    int32_t x = s[ch][i], k = x >= 0 ? x / q : -((-x + q - 1) / q);
                    b->hold[ch] = k * q;
                }
            b->hold_n = (b->hold_n + 1u) % period;
            for (ch = 0; ch < 2u; ch++) {
                int32_t w = b->hold[ch];
                if (period > 1u) {              /* the one-pole low-pass: b (x + x1) + a y1 */
                    int32_t y = bmul30(VS_DECI_B_Q30, w + b->held[ch]) + bmul30(VS_DECI_A_Q30, b->dlp[ch]);
                    b->held[ch] = w; b->dlp[ch] = y; w = y;
                }
                s[ch][i] = bmul24(ONE24 - m, s[ch][i]) + bmul24(m, w);
            }
        }
        return;
    }
    if (type == 4) {                            /* r.m.: x (1 - m + m sin) */
        const int32_t m = (lev << 24) / 127;
        const uint32_t inc = VS_RM_INC_Q32[c1];
        for (i = 0; i < n; i++) {
            const int32_t g = ONE24 - m + (int32_t)(((int64_t)m * bus_sin(b->rm_ph)) >> 30);
            b->rm_ph += inc;
            s[0][i] = bmul24(s[0][i], g);
            s[1][i] = bmul24(s[1][i], g);
        }
        return;
    }
    if (type == 5) {                            /* comp.: x out (Gmax^-4 + (env / 0.2604)^4)^(-1/4) */
        const uint32_t a = VS_COMP_A_Q31[c2];
        const int32_t lg = VS_COMP_LGMAX_Q24[c1], outg = VS_COMP_OUT_Q24[lev];
        for (i = 0; i < n; i++)
            for (ch = 0; ch < 2u; ch++) {
                const int32_t x = s[ch][i], ax = x < 0 ? -x : x;
                int32_t l, hi, d;
                uint32_t di;
                b->env[ch] += (int32_t)(((int64_t)(ax - b->env[ch]) * a + (1 << 30)) >> 31);
                if (b->env[ch] > 0) {
                    const int32_t le = 4 * (bus_log2((uint64_t)b->env[ch], 24) + VS_COMP_LKNEE_Q24);
                    hi = le > lg ? le : lg; d = le > lg ? le - lg : lg - le;
                    di = (uint32_t)d >> 18;     /* steps of 1/64 */
                    l = hi + (di >= 2048u ? 0 : VS_SOFTPLUS_Q24[di] + (int32_t)(((int64_t)(VS_SOFTPLUS_Q24[di + 1u] - VS_SOFTPLUS_Q24[di]) * (int32_t)((uint32_t)d & 262143u)) >> 18));
                } else l = lg;
                s[ch][i] = (int32_t)(((int64_t)bmul24(x, outg) * bus_exp2(-(l >> 2), 24)) >> 24);
            }
        return;
    }
    {   /* phasers */
        static const uint8_t STAGES[4] = {2, 4, 8, 12};
        const uint32_t stages = STAGES[type - 6];
        const int32_t h = c1 * 440402;           /* 0.02625 x CTRL1 octaves, Q24 */
        const int32_t step = (int32_t)(((int64_t)h * VS_PH_RATE_Q32[c2]) >> 32);
        const int32_t k = (int32_t)(((int64_t)(lev - 64) * 1063004405) >> 6);   /* 0.99 (LEVEL - 64) / 64, Q30 */
        const int32_t top = 38923141 + h, bot = 38923141 - h;   /* 2.32 +- H octaves, Q24 */
        uint32_t live = 0, st;
        for (i = 0; i < n; i++) live |= (uint32_t)(s[0][i] | s[1][i]);
        for (i = 0; i < n; i++) {
            int32_t a;
            uint32_t pi, pf;
            if (live && h > 0) {
                b->ph_pos += b->ph_dir * step;
                if (b->ph_pos > top) { b->ph_dir = -1; b->ph_pos = 2 * top - b->ph_pos; }
                else if (b->ph_pos < bot && b->ph_dir < 0) { b->ph_dir = 1; b->ph_pos = 2 * bot - b->ph_pos; }
            }
            pi = (uint32_t)vs_clamp(b->ph_pos + (2 << 24), 0, (8 << 24) - 1);   /* (the table starts 2 octaves down) */
            pf = pi & 65535u; pi >>= 16;
            a = VS_PH_A_Q30[pi] + (int32_t)(((int64_t)(VS_PH_A_Q30[pi + 1u] - VS_PH_A_Q30[pi]) * (int32_t)pf) >> 16);
            for (ch = 0; ch < 2u; ch++) {
                const int32_t x = s[ch][i];
                int32_t y = x + bmul30(k, b->ap_fb[ch]);
                for (st = 0; st < stages; st++) {
                    const int32_t o = bmul30(a, y) + b->ap[ch][st];
                    b->ap[ch][st] = y - bmul30(a, o);
                    y = o;
                }
                b->ap_fb[ch] = y;
                s[ch][i] = (int32_t)(((int64_t)x + y) >> 1);
            }
        }
    }
}

/* ================================================================== EQ */
/* a root pair at the analog Q 2^lq (Q24) and w0 (Q29), mapped by impulse invariance: z^2 + c1 z + c2, Q28 */
static void eq_pair(int32_t lq, int32_t w0, int64_t *c1, int64_t *c2)
{
    /* zeta = 1 / (2 q); e = exp(-zeta w0) */
    const int64_t zeta = bus_exp2(-lq - ONE24, 28);                     /* Q28 */
    const int32_t zw = (int32_t)((zeta * w0) >> 29);                   /* zeta w0, Q28 */
    const int32_t L2E = 24204406;                                       /* log2(e), Q24 */
    const int64_t e = bus_exp2(-(int32_t)(((int64_t)zw * L2E) >> 28), 28);
    if (zeta <= ((int64_t)1 << 28)) {           /* -2 e cos(sqrt(1 - zeta^2) w0) */
        const uint64_t r = bus_isqrt((uint64_t)((((int64_t)1 << 56) - zeta * zeta)));   /* Q28 */
        const int64_t th = ((int64_t)r * w0) >> 28;                    /* radians Q29 */
        const uint32_t ph = (uint32_t)((th * 1367130551) >> 30) + 0x40000000u;   /* cycles Q32, + a quarter: cos */
        *c1 = -(2 * e * bus_sin(ph)) >> 30;
    } else {                                    /* -2 e cosh(s w0) = -(exp(-(zeta - s) w0) + exp(-(zeta + s) w0)) */
        const int64_t s = (int64_t)bus_isqrt((uint64_t)(zeta * zeta - ((int64_t)1 << 56)));
        const int32_t lo = (int32_t)((((zeta - s) * w0) >> 29) * L2E >> 28), hi = (int32_t)((((zeta + s) * w0) >> 29) * L2E >> 28);
        *c1 = -(bus_exp2(-lo, 28) + bus_exp2(-hi, 28));
    }
    *c2 = (e * e) >> 28;
}
static void eq_coef(vs_bus_t *b, int32_t f, int32_t lv, int32_t qk)
{
    const int32_t w0 = VS_EQ_W0_Q29[f], lg = VS_EQ_LG_Q24[lv];
    const int32_t lpq = VS_EQ_LQ_Q24[qk] + (lg < 0 ? lg : 0), lzq = lpq - lg;   /* the poles' Q (x G for a cut); the zeros' / G */
    int64_t a1, a2, z1, z2, cw, c2w, sw, s2w, pr, pi, zr, zi, num, den, k;
    uint32_t ph = (uint32_t)(((int64_t)w0 * 1367130551) >> 30);         /* w0 in cycles, Q32 */
    eq_pair(lpq, w0, &a1, &a2);
    eq_pair(lzq, w0, &z1, &z2);
    /* k = G |1 + a1 u + a2 u^2| / |1 + z1 u + z2 u^2|, u = e^(-i w0): the gain at f0 is G */
    cw = bus_sin(ph + 0x40000000u) >> 2; c2w = bus_sin(2u * ph + 0x40000000u) >> 2;   /* Q28 */
    sw = bus_sin(ph) >> 2; s2w = bus_sin(2u * ph) >> 2;
    pr = ((int64_t)1 << 28) + ((a1 * cw) >> 28) + ((a2 * c2w) >> 28); pi = ((a1 * sw) >> 28) + ((a2 * s2w) >> 28);
    zr = ((int64_t)1 << 28) + ((z1 * cw) >> 28) + ((z2 * c2w) >> 28); zi = ((z1 * sw) >> 28) + ((z2 * s2w) >> 28);
    num = pr * pr + pi * pi; den = zr * zr + zi * zi;                 /* Q56 */
    while (den >= ((int64_t)1 << 31) || num >= ((int64_t)1 << 43)) { num >>= 1; den >>= 1; }
    if (den <= 0) den = 1;
    {
        const uint64_t q = (uint64_t)num << 20;
        const uint32_t ratio = (q >> 32) >= (uint64_t)den ? 0xFFFFFFFFu : vs_udiv64_32(q, (uint32_t)den);   /* Q20 */
        k = (int64_t)bus_isqrt((uint64_t)ratio << 20);   /* Q20 */
    }
    k = (k * bus_exp2(lg, 24)) >> 18;           /* x G: Q26 */
    b->eq_b[0] = (int32_t)vs_clamp64(k, -0x7FFFFFFF, 0x7FFFFFFF);
    b->eq_b[1] = (int32_t)vs_clamp64((k * z1) >> 28, -0x7FFFFFFF, 0x7FFFFFFF);
    b->eq_b[2] = (int32_t)vs_clamp64((k * z2) >> 28, -0x7FFFFFFF, 0x7FFFFFFF);
    b->eq_a[0] = (int32_t)(a1 >> 2); b->eq_a[1] = (int32_t)(a2 >> 2);  /* Q26 */
}

/* ================================================================== DELAY */
static inline int32_t dly_at(const vs_bus_t *b, uint32_t ch, uint32_t i)
{
    if (b->dwritten >= VS_DLY_LEN) return from_line(dline[ch][i]);   /* (the usual case: the line full) */
    uint32_t back = b->dpos + VS_DLY_LEN - 1u - i;   /* frames since it was written, - 1 (no %: a divide a read) */
    if (back >= VS_DLY_LEN) back -= VS_DLY_LEN;
    return b->dwritten >= VS_DLY_LEN || back < b->dwritten ? from_line(dline[ch][i]) : 0;
}
static int32_t dly_read(const vs_bus_t *b, uint32_t ch, uint32_t d16)   /* d16: the delay, frames Q16, rounded up */
{
    /* 32-bit: frames Q16 reach 2^31.4 (a 1 s line), unsigned. ((pos << 24) - delay) >> 8 is (pos << 16) - ceil(delay / 256):
       the same index and fraction as the Q24 sum it replaces */
    const uint32_t p16 = b->dpos << 16;
    uint32_t rp = p16 - d16, i0, i1, fr;
    int32_t a, c;
    if (p16 < d16) rp += (uint32_t)VS_DLY_LEN << 16;   /* (once: a delay is under a line) */
    i0 = rp >> 16; fr = rp & 65535u;
    if (i0 >= VS_DLY_LEN) i0 -= VS_DLY_LEN;
    i1 = i0 + 1u == VS_DLY_LEN ? 0u : i0 + 1u;
    a = dly_at(b, ch, i0); c = dly_at(b, ch, i1);
    return a + (int32_t)(((int64_t)(c - a) * fr) >> 16);
}
static void dly_empty(vs_bus_t *b)
{
    b->dwritten = 0;
    b->din = ONE30;
    b->dtone_x[0] = b->dtone_x[1] = b->dtone_y[0] = b->dtone_y[1] = 0;
}
static int32_t dtone(vs_bus_t *b, uint32_t ch, int32_t t, int32_t x)
{
    int32_t y;
    if (t == 64) return x;
    y = bmul30r((int32_t)VS_DTONE_G_Q30[t], t > 64 ? x - b->dtone_x[ch] : x + b->dtone_x[ch]) + bmul30r(VS_DTONE_B_Q30[t], b->dtone_y[ch]);
    b->dtone_x[ch] = x; b->dtone_y[ch] = y;
    return y;
}
/* echo: the wet echoes go there (the chorus takes them) instead of into l / r */
static void delay(vs_bus_t *b, const int32_t *p, int32_t *l, int32_t *r, uint32_t n, uint32_t bpm_q8, int32_t *el, int32_t *er)
{
    const int32_t t = vs_clamp(p[VS_P_DLY_TIME], 0, 19), sp = vs_clamp(p[VS_P_DLY_SPREAD], 0, 127) - 64;
    const int32_t sd = sp < 0 ? (-sp > 63 ? 63 : -sp) : (sp > 63 ? 63 : sp);
    const uint32_t bpm = bpm_q8 < (20u << 8) ? 20u << 8 : bpm_q8;
    const int32_t fb = (int32_t)vs_udiv64_32((uint64_t)vs_clamp(p[VS_P_DLY_FBK], 0, 127) << 30, 127u);
    const int32_t dw = vs_clamp(p[VS_P_DLY_DRYWET], 0, 127);
    const int32_t wet = dw >= 65 ? ONE30 : (int32_t)vs_udiv64_32((uint64_t)dw << 31, 129u);    /* min(1, dw / 64.5) */
    const int32_t dry = dw <= 64 ? ONE30 : (int32_t)vs_udiv64_32((uint64_t)(127 - dw) << 30, 63u);
    const int32_t type = vs_clamp(p[VS_P_DLY_TYPE], 0, 2), tn = vs_clamp(p[VS_P_DLY_TONE], 0, 127);
    uint32_t base = (VS_DLY_BEAT[t] / bpm) & ~3u, off = VS_DLY_SPREAD[sd], tl, tr, i;
    const uint32_t maxt = VS_DLY_LEN - 4u;
    tl = base + (sp > 0 ? off : 0u); tr = base + (sp < 0 ? off : 0u);
    if (tl < VS_DLY_MIN) tl = VS_DLY_MIN;
    if (tr < VS_DLY_MIN) tr = VS_DLY_MIN;
    if (tl > maxt) tl = maxt;
    if (tr > maxt) tr = maxt;
    if (b->dsmooth[0] < 0) { b->dsmooth[0] = (int64_t)tl << 24; b->dsmooth[1] = (int64_t)tr << 24; b->dglide = 1; }
    if (tl != b->dtl || tr != b->dtr) { b->dtl = tl; b->dtr = tr; b->dglide = 1; }
    for (i = 0; i < n; i++) {
        int32_t out = ONE30, e0, e1, inl, inr, wl, wr, wo;
        if (b->dfade) {
            if (--b->dfade == 0) { dly_empty(b); b->din = 0; b->dsmooth[0] = (int64_t)tl << 24; b->dsmooth[1] = (int64_t)tr << 24; b->dglide = 1; }
            else out = (int32_t)vs_udiv64_32((uint64_t)b->dfade << 30, VS_DLY_FADE);
        }
        if (b->dglide) {                        /* the times glide; once a frame's step is 0 on both, they stay (the
                                                   targets are the block's): no 64-bit glide until a target moves */
            const int64_t s0 = ((((int64_t)tl << 24) - b->dsmooth[0]) * VS_DLY_GLIDE_Q31 + ((int64_t)1 << 30)) >> 31;
            const int64_t s1 = ((((int64_t)tr << 24) - b->dsmooth[1]) * VS_DLY_GLIDE_Q31 + ((int64_t)1 << 30)) >> 31;
            b->dsmooth[0] += s0; b->dsmooth[1] += s1;
            b->d16[0] = (uint32_t)((b->dsmooth[0] + 255) >> 8); b->d16[1] = (uint32_t)((b->dsmooth[1] + 255) >> 8);
            if (!s0 && !s1) b->dglide = 0;
        }
        e0 = dtone(b, 0, tn, dly_read(b, 0, b->d16[0]));
        e1 = dtone(b, 1, tn, dly_read(b, 1, b->d16[1]));
        if (b->din == ONE30) { inl = l[i]; inr = r[i]; }   /* (x 1.0, exactly) */
        else { inl = bmul30(l[i], b->din); inr = bmul30(r[i], b->din); }
        if (b->din < ONE30) { b->din += ONE30 / (int32_t)VS_DLY_FADE; if (b->din > ONE30) b->din = ONE30; }
        switch (type) {
        case 1: wl = inr + bmul30(fb, e1); wr = inl + bmul30(fb, e0); break;   /* X: a left input echoes right first */
        case 2: wl = (int32_t)(((int64_t)inl + inr) >> 1) + bmul30(fb, e1); wr = e0; break;   /* PP */
        default: wl = inl + bmul30(fb, e0); wr = inr + bmul30(fb, e1); break;
        }
        dline[0][b->dpos] = to_line(wl);
        dline[1][b->dpos] = to_line(wr);
        if (++b->dpos == VS_DLY_LEN) b->dpos = 0;
        if (b->dwritten < VS_DLY_LEN) b->dwritten++;
        wo = out == ONE30 ? wet : bmul30(wet, out);
        e0 = wo == ONE30 ? e0 : bmul30(wo, e0); e1 = wo == ONE30 ? e1 : bmul30(wo, e1);
        if (dry != ONE30) { l[i] = bmul30(dry, l[i]); r[i] = bmul30(dry, r[i]); }
        if (el) { el[i] = e0; er[i] = e1; }
        else { l[i] += e0; r[i] += e1; }
    }
}

/* ================================================================== CHORUS */
static int32_t cho_read(const vs_bus_t *b, uint32_t line, int32_t delay)   /* delay: frames Q16 (0: the frame just written) */
{
    int32_t rp = (int32_t)(b->cpos << 16) - delay, a, c;
    uint32_t i0, i1, fr;
    if (rp < 0) rp += (int32_t)(VS_CHO_LEN << 16);
    i0 = (uint32_t)rp >> 16; fr = (uint32_t)rp & 65535u;
    if (i0 >= VS_CHO_LEN) i0 -= VS_CHO_LEN;
    i1 = i0 + 1u == VS_CHO_LEN ? 0u : i0 + 1u;
    a = cline[line][i0]; c = cline[line][i1];
    return a * 1024 + (((c - a) * (int32_t)(fr >> 1)) >> 5);   /* (from_line: 32-bit, the fraction to 15 bits) */
}
static void chorus(vs_bus_t *b, const int32_t *p, int32_t *l, int32_t *r, uint32_t n, const int32_t *el, const int32_t *er)
{
    const int32_t type = vs_clamp(p[VS_P_CHO_TYPE], 1, 4), quad = type >= 4;
    const uint32_t thr = type == 1 ? 0u : 0x40000000u;
    const int32_t base = (int32_t)VS_CHO_TIME[vs_clamp(p[VS_P_CHO_TIME], 0, 127)] << 16;   /* Q16 */
    const int32_t rate = vs_clamp(p[VS_P_CHO_RATE], 0, 127);
    const int32_t depth = (int32_t)(((((int64_t)base * VS_CHO_DEPTH_Q16[vs_clamp(p[VS_P_CHO_DEPTH], 0, 127)]) >> 16) * VS_CHO_SWING_Q16[rate]) >> 16);
    const int32_t fb = (int32_t)(((int64_t)(vs_clamp(p[VS_P_CHO_FBK], 0, 127) - 64) * 1063004405) >> 6);   /* 0.99 (fb - 64) / 64 */
    const int32_t level = (vs_clamp(p[VS_P_CHO_LEVEL], 0, 127) << 24) / 127;
    const int32_t maxd = (int32_t)((VS_CHO_LEN - 3u) << 16);
    const uint32_t inc = VS_CHO_INC_Q32[rate];
    uint32_t i, k;
    if (!el && b->cechoes) {                    /* the delay went off: its chorus lines empty */
        for (k = 0; k < VS_CHO_LEN; k++) cline[2][k] = cline[3][k] = 0;
    }
    b->cechoes = el != 0;
    const uint32_t th[4] = {0u, quad ? 0x40000000u : thr, 0x80000000u, 0xC0000000u}, taps = quad ? 4u : 2u;
    for (i = 0; i < n; i++) {
        int32_t d[4], wl, wr, ol, orr;
        uint32_t line;
        int near = 0;
        for (k = 0; k < taps; k++) {
            d[k] = vs_clamp(base + (int32_t)(((int64_t)depth * bus_sin(b->cphase - th[k])) >> 30), 0, maxd);
            near |= d[k] < 65536;               /* (at 1 frame or more the read stops short of this frame, or weighs it 0) */
        }
        ol = l[i]; orr = r[i];
        for (line = 0; line < (el ? 4u : 2u); line += 2u) {
            const int32_t inl = line ? el[i] : l[i], inr = line ? er[i] : r[i];
            int32_t fl, fr2;
            if (near) {                         /* a tap under a frame reads this frame: the input goes in first */
                cline[line][b->cpos] = to_line(inl);
                cline[line + 1u][b->cpos] = to_line(inr);
            }
            if (quad) {
                const int32_t ll = cho_read(b, line, d[0]), rl = cho_read(b, line + 1u, d[1]);
                const int32_t lr = cho_read(b, line, d[2]), rr = cho_read(b, line + 1u, d[3]);
                wl = ll + rl; wr = lr + rr; fl = ll; fr2 = rr;
            } else {
                wl = fl = cho_read(b, line, d[0]);
                wr = fr2 = cho_read(b, line + 1u, d[1]);
            }
            cline[line][b->cpos] = to_line(inl + bmul30(fb, fl));
            cline[line + 1u][b->cpos] = to_line(inr + bmul30(fb, fr2));
            if (!line) { ol += bmul24(level, wl); orr += bmul24(level, wr); }
            else { ol += wl; orr += wr; }
        }
        if (++b->cpos == VS_CHO_LEN) b->cpos = 0;
        b->cphase += inc;
        l[i] = ol; r[i] = orr;
    }
}

/* ================================================================== the block */
void vs_bus_block(vs_bus_t *b, const int32_t *p, int32_t *l, int32_t *r, const int32_t pan[2][VS_BUS_N], uint32_t n,
                  uint32_t bpm_q8)
{
    uint32_t i, ch;
    if (p[VS_P_FX_ON]) effect(b, p, l, r, n);
    for (i = 0; i < n; i++) {                   /* PAN */
        l[i] = (int32_t)(((int64_t)l[i] * pan[0][i]) >> 16);
        r[i] = (int32_t)(((int64_t)r[i] * pan[1][i]) >> 16);
    }
    {   /* EQ: the peak (LEVEL not 64), then TONE */
        const int32_t f = vs_clamp(p[VS_P_EQ_FREQ], 0, 127), lv = vs_clamp(p[VS_P_EQ_LEVEL], 0, 127), qk = vs_clamp(p[VS_P_EQ_Q], 0, 127);
        const uint32_t t = (uint32_t)vs_clamp(p[VS_P_EQ_TONE], 0, 127);
        const int32_t g = (int32_t)VS_TONE_G_Q30[t], bb = VS_TONE_B_Q30[t];
        if (lv != 64) {
            const int32_t key = f << 14 | lv << 7 | qk;
            int32_t c[5];
            if (key != b->eq_key) { b->eq_key = key; eq_coef(b, f, lv, qk); }
            c[0] = b->eq_b[0]; c[1] = b->eq_b[1]; c[2] = b->eq_b[2]; c[3] = b->eq_a[0]; c[4] = b->eq_a[1];
            for (i = 0; i < n; i++) { l[i] = bq(&b->eq[0], c, l[i], 26); r[i] = bq(&b->eq[1], c, r[i], 26); }
        }
        for (ch = 0; ch < 2u; ch++) {
            int32_t *s = ch ? r : l, x1 = b->tone_x[ch], y1 = b->tone_y[ch];
            for (i = 0; i < n; i++) {
                const int32_t x = s[i], y = bmul30r(g, t > 64u ? x - x1 : x + x1) + bmul30r(bb, y1);
                x1 = x; y1 = y; s[i] = y;
            }
            b->tone_x[ch] = x1; b->tone_y[ch] = y1;
        }
    }
    {   /* DELAY, CHORUS (with both on, the echoes reach the output through the chorus only) */
        const int don = p[VS_P_DLY_ON] != 0, con = p[VS_P_CHO_ON] != 0;
        int32_t el[VS_BUS_N], er[VS_BUS_N];
        if (don && con) {
            delay(b, p, l, r, n, bpm_q8, el, er);
            chorus(b, p, l, r, n, el, er);
        } else {
            if (don) delay(b, p, l, r, n, bpm_q8, 0, 0);
            else if (b->dfade) { b->dfade = 0; dly_empty(b); b->din = 0; }   /* off: a pending clear empties at once */
            if (con) chorus(b, p, l, r, n, 0, 0);
            else b->cphase += VS_CHO_INC_Q32[vs_clamp(p[VS_P_CHO_RATE], 0, 127)] * n;   /* its LFO runs on */
        }
    }
}
