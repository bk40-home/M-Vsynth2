/* SPDX-License-Identifier: GPL-3.0-only */
/* Stage 3's desk check: the gate's voice loop beside M-vance's whole engine, on this computer, both at -O2.
 * M-vance's figure on the FM-1 is known (Kris, 13 Nov 2026: under 60 % of the buffer with everything on), so the
 * ratio gives a first guess of the gate's. A guess only: a computer's cache is not the chip's flash.
 *   cc -O2 -I firmware/src -I firmware/gen -I firmware/hal -I MVANCE/firmware/src -I MVANCE/firmware/gen \
 *      -o build/gate_bench tools/bench/gate_vs_mvance.c MVANCE/firmware/src/mv_dsp.c firmware/src/vs_dsp.c -lm */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mv_voice.h"
#include "vs_gate.h"

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
#define BUFS 4000                                /* 23 s of sound */
static int32_t buf[512];
static mv_engine_t e;
static vs_gate_t g;

static double mvance(int ssaw, int fx)
{
    double t0;
    int i, n, best_i;
    double best = 1e9;
    for (best_i = 0; best_i < 3; best_i++) {
        mv_engine_init(&e);
        if (ssaw) { mv_param_set(&e.patch, MV_P_OSC1_WAVE, 0); mv_param_set(&e.patch, MV_P_OSC1_C1, 80); mv_param_set(&e.patch, MV_P_OSC1_C2, 127); }
        mv_param_set(&e.patch, MV_P_FLT_SLOPE, 1);
        mv_param_set(&e.patch, MV_P_AENV_R, 127);
        if (fx) {
            mv_param_set(&e.patch, MV_P_MFX_LEVEL, 100); mv_param_set(&e.patch, MV_P_DLY_LEVEL, 100);
            e.rvb[MV_RV_LEVEL] = 100;
        } else {
            mv_param_set(&e.patch, MV_P_MFX_LEVEL, 0); mv_param_set(&e.patch, MV_P_DLY_LEVEL, 0);
            e.rvb[MV_RV_LEVEL] = 0;
        }
        for (n = 0; n < 8; n++) mv_engine_note_on(&e, 36 + 5 * n, 100);
        for (i = 0; i < 50; i++) { memset(buf, 0, sizeof buf); mv_engine_render(&e, buf, 256); }
        t0 = now();
        for (i = 0; i < BUFS; i++) { memset(buf, 0, sizeof buf); mv_engine_render(&e, buf, 256); }
        t0 = now() - t0;
        if (t0 < best) best = t0;
    }
    return best / BUFS * 1e6;
}
static double gate(unsigned nv, unsigned load, unsigned filt)
{
    double t0, best = 1e9;
    int i, r;
    for (r = 0; r < 3; r++) {
        vs_gate_init(&g);
        g.nv = nv; g.load = load; g.filt = filt;
        t0 = now();
        for (i = 0; i < BUFS; i++) { memset(buf, 0, sizeof buf); vs_gate_render(&g, buf, 256); }
        t0 = now() - t0;
        if (t0 < best) best = t0;
    }
    return best / BUFS * 1e6;
}
int main(void)
{
    static const char *const L[3] = {"OSC", "+DETUNE", "+SUB"}, *const F[2] = {"SVF", "LP24"};
    double ref = mvance(1, 1), a;
    unsigned l, f;
    printf("M-vance, 8 voices, us a buffer on this computer:\n");
    printf("  INIT patch (-24 dB), no FX        %7.1f\n", mvance(0, 0));
    printf("  SUPER SAW, no FX                  %7.1f\n", mvance(1, 0));
    printf("  SUPER SAW, MULTI-FX, DELAY, REVERB %6.1f   = 1.00 (the FM-1: under 60 %%)\n", ref);
    printf("The gate, 8 voices:                    us    x M-vance's full stack\n");
    for (l = 0; l < 3; l++)
        for (f = 0; f < 2; f++) {
            a = gate(8, l, f);
            printf("  %-8s %-5s                  %7.1f   %.2f\n", L[l], F[f], a, a / ref);
        }
    return 0;
}
