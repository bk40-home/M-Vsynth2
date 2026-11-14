/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2, Stage 3: the gate's voice loop (vs_gate.h). Built in vs_dsp.c at -O2, as the engine will be. */
#include "vs_gate.h"
#include "vs_wave.h"
#include "vs_gate_tab.h"

#define GATE_PW 1288490188u                     /* P/W: 0.3 cycle */
#define GATE_HALF (1u << 31)                    /* half a cycle: the SUB's square */
#define GATE_COPY_Q15 4866                      /* a copy: x 0.297, and the pulse's / 2 */
#define GATE_LIM_FLOOR 6390                     /* the limiter's floor, 0.39 */

static const uint32_t INC_C9[12] = {            /* MIDI 120..131, f x 2^32 / 44100 (Dinghy's table) */
    815363807u, 863847862u, 915214929u, 969636441u, 1027294024u, 1088380105u,
    1153098554u, 1221665363u, 1294309365u, 1371273005u, 1452813141u, 1539201906u};
/* the copies' steps over OSC1's, Q16: +-1, 3, 5, 7 x 6.3 cents (DETUNE 40) */
static const uint32_t COPY_Q16[8] = {63869u, 64344u, 64821u, 65298u, 65775u, 66257u, 66741u, 67229u};

/* TABLES RAM: the saw tables again, in the platform's pool (zeroed at boot, filled by vs_gate_init) */
static int16_t gate_ram[VS_WAVE_NT][VS_WAVE_N + 1u] __attribute__((section(".pool")));

static uint32_t note_inc(uint32_t note)
{
    if (note > 131u) note = 131u;
    return INC_C9[note % 12u] >> (10u - note / 12u);
}

static void gate_tune(vs_gate_t *g, uint32_t base, uint32_t spread, uint32_t ram)
{
    const int16_t (*saw)[VS_WAVE_N + 1u] = ram ? (const int16_t (*)[VS_WAVE_N + 1u])gate_ram : VS_SAW;
    uint32_t v, k;
    for (v = 0; v < VS_GATE_NV; v++) {
        vs_gate_voice_t *p = &g->v[v];
        uint32_t note = base + spread * v, sub;
        note = note < 12u ? 0u : note - 12u;
        if (note > 119u) note = 119u;
        sub = note < 12u ? 0u : note - 12u;
        p->inc1 = note_inc(note) & ~31u;        /* whole 2^-27 cycles */
        p->inc2 = note_inc(note + 7u);
        for (k = 0; k < 8u; k++)
            p->cinc[k] = (uint32_t)(((uint64_t)(p->inc1 >> 1) * COPY_Q16[k]) >> 16);
        p->t1 = saw[vs_wave_of(note)];
        p->ts = saw[vs_wave_of(sub)];
        p->t2 = saw[vs_wave_of(note + 7u)];
    }
    g->tune_live = base | spread << 8 | ram << 16;
}

void vs_gate_init(vs_gate_t *g)
{
    uint32_t v, k, r = 0x2545F491u;
    for (v = 0; v < VS_WAVE_NT; v++)
        for (k = 0; k <= VS_WAVE_N; k++) gate_ram[v][k] = VS_SAW[v][k];
    for (v = 0; v < VS_GATE_NV; v++) {
        vs_gate_voice_t *p = &g->v[v];
        p->low = p->band = p->fb = p->agc = p->lenv = 0;
        p->st[0] = p->st[1] = p->st[2] = p->st[3] = 0;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5; p->p1 = r; p->ps = r >> 1;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5; p->p2 = r;
        for (k = 0; k < 8u; k++) { r ^= r << 13; r ^= r >> 17; r ^= r << 5; p->cs[k] = r; }
    }
    g->nv = 8u; g->load = VS_GL_SUB; g->filt = VS_GF_SVF; g->base = 48u; g->ram = 0; g->spread = 5u;
    g->lfo = 0; g->f = (int32_t)VS_GATE_SVF_F[64] << 8;
    gate_tune(g, 48u, 5u, 0u);
}

/* an interpolated read: 11 bits of index, 15 of fraction */
static inline int32_t rd(const int16_t *t, uint32_t p)
{
    uint32_t i = p >> 21;
    int32_t a = t[i], b = t[i + 1u];
    return a + (((b - a) * (int32_t)((p >> 6) & 32767u)) >> 15);
}

/* one voice, n frames (n <= VS_CTL), added to out. load and filt are constants at each call site, so each case is
 * compiled as its own loop with nothing tested a frame (Synth2 does the same: RenderChunkAs) */
static inline __attribute__((always_inline)) void gate_voice(vs_gate_voice_t *p, int32_t *out, uint32_t n,
                                                             const uint32_t load, const uint32_t filt, int32_t fq,
                                                             int32_t fstep, int32_t amp, int32_t pan_l, int32_t pan_r)
{
    uint32_t p1 = p->p1, ps = p->ps, p2 = p->p2, i, k;
    const uint32_t inc1 = p->inc1, inc2 = p->inc2;
    const int16_t *t1 = p->t1, *ts = p->ts, *t2 = p->t2;
    int32_t low = p->low, band = p->band, fb = p->fb, agc = p->agc, lenv = p->lenv;
    int32_t s0 = p->st[0], s1 = p->st[1], s2 = p->st[2], s3 = p->st[3];
    for (i = 0; i < n; i++) {
        int32_t o1, o2, x, y, a;
        p1 += inc1; p2 += inc2;
        o1 = (rd(t1, p1) - rd(t1, p1 + GATE_PW)) >> 1;
        o2 = (rd(t2, p2) - rd(t2, p2 + GATE_PW)) >> 1;
        if (load >= VS_GL_DETUNE) {
            int32_t c1 = 0;
            for (k = 0; k < 8u; k++) {
                uint32_t c = (p->cs[k] += p->cinc[k]) << 1;
                c1 += t1[c >> 21] - t1[(c + GATE_PW) >> 21];
            }
            o1 += (c1 * GATE_COPY_Q15) >> 15;
        }
        if (load >= VS_GL_SUB) {
            int32_t sub, cs = 0;
            ps += inc1 >> 1;
            sub = (rd(ts, ps) - rd(ts, ps + GATE_HALF)) >> 1;
            for (k = 0; k < 8u; k++) {
                uint32_t c = p->cs[k];
                cs += ts[c >> 21] - ts[(c + GATE_HALF) >> 21];
            }
            sub += (cs * GATE_COPY_Q15) >> 15;
            x = (o1 + o2 + 2 * sub) >> 2;       /* mix 64, sub gain 64: 1/4, 1/2, 1/4 */
        } else {
            x = (o1 + o2) >> 1;
        }
        fq += fstep;
        if (filt == VS_GF_SVF) {
            int32_t f = fq >> 8, in = (x * 21299) >> 15, high;   /* in gain 1 - 0.7 r, r = 0.5 */
            low += (int32_t)(((int64_t)f * band) >> 14);
            high = in - low - (band >> 1);
            band += (int32_t)(((int64_t)f * high) >> 14);
            low += (int32_t)(((int64_t)f * band) >> 14);
            high = in - low - (band >> 1);
            band += (int32_t)(((int64_t)f * high) >> 14);
            y = low;
        } else {
            int32_t g1 = fq >> 8, u = x - fb - (fb >> 1), v;      /* k = 1.5 */
            if (g1 > 16384) g1 = 16384;                           /* the one-pole's a, Q15 (the gate: ~ f) */
            /* (rounded: a floor in each of the four stages adds up to a DC offset round the loop) */
            v = (int32_t)(((int64_t)(u - s0) * g1 + 16384) >> 15); u = v + s0; s0 = u + v;
            v = (int32_t)(((int64_t)(u - s1) * g1 + 16384) >> 15); u = v + s1; s1 = u + v;
            v = (int32_t)(((int64_t)(u - s2) * g1 + 16384) >> 15); u = v + s2; s2 = u + v;
            v = (int32_t)(((int64_t)(u - s3) * g1 + 16384) >> 15); u = v + s3; s3 = u + v;
            fb = u;
            a = u < 0 ? -u : u;
            agc += ((a - agc) * (a > agc ? 608 : 4)) >> 15;       /* the AGC's follower: 1.2 ms up, 0.2 s down */
            y = u;
        }
        a = y < 0 ? -y : y;
        lenv += ((a - lenv) * (a > lenv ? 199 : 108)) >> 15;      /* 3.7 ms up, 6.9 ms down */
        if (lenv > GATE_LIM_FLOOR) {
            uint32_t e = (uint32_t)lenv >> 9, fr = (uint32_t)lenv & 511u;
            int32_t g0, gn;
            if (e > VS_GATE_LIM_N - 2u) { e = VS_GATE_LIM_N - 2u; fr = 511u; }
            g0 = VS_GATE_LIM[e]; gn = VS_GATE_LIM[e + 1u];
            y = (int32_t)(((int64_t)y * (g0 + (((gn - g0) * (int32_t)fr) >> 9))) >> 15);
        }
        y = (y * amp) >> 15;
        out[2u * i] += (y * pan_l) >> 15;
        out[2u * i + 1u] += (y * pan_r) >> 15;
    }
    p->p1 = p1; p->ps = ps; p->p2 = p2;
    p->low = low; p->band = band; p->fb = fb; p->agc = agc; p->lenv = lenv;
    p->st[0] = s0; p->st[1] = s1; p->st[2] = s2; p->st[3] = s3;
}

void vs_gate_render(vs_gate_t *g, int32_t *out, uint32_t n)
{
    uint32_t nv = g->nv, load = g->load, filt = g->filt, base = g->base & 255u, pos, v;
    uint32_t spread = g->spread & 7u, ram = g->ram & 1u;
    int32_t amp;
    if (nv > VS_GATE_NV) nv = VS_GATE_NV;
    if ((base | spread << 8 | ram << 16) != g->tune_live) gate_tune(g, base, spread, ram);
    if (!nv) return;
    amp = 9830 * 8 / (int32_t)nv;               /* 8 voices: 0.3 each; more: the same sum */
    if (amp > 19660) amp = 19660;
    for (pos = 0; pos < n; pos += VS_CTL) {
        uint32_t m = n - pos < VS_CTL ? n - pos : VS_CTL, tri, k;
        int32_t knob, ft, fstep;
        g->lfo += 311000u * m;                  /* the sweep: 0.3 Hz */
        tri = g->lfo >> 15;                     /* 0..131071 */
        if (tri > 65535u) tri = 131071u - tri;  /* a triangle, 0..65535 */
        knob = (40 << 8) + (int32_t)((tri * (70u << 8)) >> 16);   /* FREQ 40..110, Q8 */
        k = (uint32_t)knob >> 8;
        ft = VS_GATE_SVF_F[k] + (((VS_GATE_SVF_F[k + 1u] - VS_GATE_SVF_F[k]) * (knob & 255)) >> 8);
        fstep = ((ft << 8) - g->f) / (int32_t)VS_CTL;
        for (v = 0; v < nv; v++) {
            vs_gate_voice_t *p = &g->v[v];
            int32_t *o = out + 2u * pos, pl = v & 1u ? 32767 : 22938, pr = v & 1u ? 22938 : 32767;
            switch (load * 2u + filt) {
            case 0: gate_voice(p, o, m, VS_GL_OSC, VS_GF_SVF, g->f, fstep, amp, pl, pr); break;
            case 1: gate_voice(p, o, m, VS_GL_OSC, VS_GF_LP24, g->f, fstep, amp, pl, pr); break;
            case 2: gate_voice(p, o, m, VS_GL_DETUNE, VS_GF_SVF, g->f, fstep, amp, pl, pr); break;
            case 3: gate_voice(p, o, m, VS_GL_DETUNE, VS_GF_LP24, g->f, fstep, amp, pl, pr); break;
            case 4: gate_voice(p, o, m, VS_GL_SUB, VS_GF_SVF, g->f, fstep, amp, pl, pr); break;
            default: gate_voice(p, o, m, VS_GL_SUB, VS_GF_LP24, g->f, fstep, amp, pl, pr); break;
            }
        }
        g->f += fstep * (int32_t)m;
    }
}
