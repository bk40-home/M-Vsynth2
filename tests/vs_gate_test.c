/* SPDX-License-Identifier: GPL-3.0-only */
/* The gate (firmware/src/vs_gate.h) on this computer: every LOAD x FILTER at 1, 8 and 16 voices sounds, stays
 * inside full scale, carries no DC to speak of and is the same twice; 0 voices is silence. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/src/vs_gate.c"

static vs_gate_t g;
static int32_t buf[512], first[512];

int main(void)
{
    uint32_t nv, l, f, i, b, fails = 0;
    for (nv = 0; nv <= 16u; nv = nv ? nv * 2u : 1u) {
        if (nv == 2u || nv == 4u) continue;
        for (l = 0; l < VS_GL_N; l++)
            for (f = 0; f < VS_GF_N; f++) {
                int32_t peak = 0;
                int64_t sum = 0, n = 0;
                int pass;
                for (pass = 0; pass < 2; pass++) {
                    vs_gate_init(&g);
                    g.nv = nv; g.load = l; g.filt = f;
                    for (b = 0; b < 2000u; b++) {       /* 11.6 s: the sweep's whole cycle, three times */
                        memset(buf, 0, sizeof buf);
                        vs_gate_render(&g, buf, 256);
                        if (b == 1999u) { if (!pass) memcpy(first, buf, sizeof buf); else if (memcmp(first, buf, sizeof buf)) { printf("FAIL not repeatable nv %u\n", nv); fails++; } }
                        if (pass) continue;
                        for (i = 0; i < 512u; i++) {
                            int32_t a = buf[i] < 0 ? -buf[i] : buf[i];
                            if (a > peak) peak = a;
                            sum += buf[i]; n++;
                        }
                    }
                }
                if (nv == 0u ? peak != 0 : (peak < 1000 || peak > 32767 || llabs(sum / n) > 64)) {
                    printf("FAIL nv %u load %u filt %u: peak %d dc %lld\n", nv, l, f, peak, (long long)(sum / n));
                    fails++;
                }
            }
    }
    printf(fails ? "vs_gate_test: %u FAILED\n" : "vs_gate_test: ok\n", fails);
    return fails != 0;
}
