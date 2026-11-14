/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware's engine playing a compare script on this computer (tests/run_compare.sh): the same file
 * tools/compare/ref_render.cpp plays through Synth2's own engine (the script's format is described there).
 *   vs_render SCRIPT OUT.f32        (stereo float32, 44100 Hz, 1.0 = full scale; the output limiter off)
 * It plays as on the FM-1: buffers of 256 frames; an event is queued before the control block of its frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/src/vs_patch.c"
#include "../firmware/src/vs_osc.c"
#include "../firmware/src/vs_env.c"
#include "../firmware/src/vs_lfo.c"
#include "../firmware/src/vs_bus.c"
#include "../firmware/src/vs_lim.c"
#include "../firmware/src/vs_voice.c"

typedef struct { long frame; int kind, a, b; } ev_t;
static vs_engine_t eng;
static ev_t evs[4096];

int main(int argc, char **argv)
{
    char line[256], w[16];
    long len = 0, pos, f;
    int a, b, nev = 0, next = 0;
    FILE *in, *out;
    if (argc != 3) { fprintf(stderr, "usage: vs_render SCRIPT OUT.f32\n"); return 2; }
    in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    vs_engine_init(&eng);
    eng.limit = 0;                              /* (beside Synth2: its output is not limited) */
    while (fgets(line, sizeof line, in)) {
        a = b = 0; f = 0;
        if (sscanf(line, "%15s", w) != 1 || w[0] == '#') continue;
        if (!strcmp(w, "p")) { sscanf(line, "%*s %d %d", &a, &b); vs_param_set(&eng.patch, (uint32_t)a, b); }
        else if (!strcmp(w, "len")) sscanf(line, "%*s %ld", &len);
        else if (nev < 4096 && !strcmp(w, "on")) { sscanf(line, "%*s %ld %d %d", &f, &a, &b); evs[nev].frame = f; evs[nev].kind = 0; evs[nev].a = a; evs[nev++].b = b; }
        else if (nev < 4096 && !strcmp(w, "off")) { sscanf(line, "%*s %ld %d", &f, &a); evs[nev].frame = f; evs[nev].kind = 1; evs[nev++].a = a; }
        else if (nev < 4096 && !strcmp(w, "bend")) { sscanf(line, "%*s %ld %d", &f, &a); evs[nev].frame = f; evs[nev].kind = 2; evs[nev++].a = a; }
        else if (nev < 4096 && !strcmp(w, "pedal")) { sscanf(line, "%*s %ld %d", &f, &a); evs[nev].frame = f; evs[nev].kind = 3; evs[nev++].a = a; }
        else { fprintf(stderr, "vs_render: %s?\n", w); return 2; }
    }
    fclose(in);
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    for (pos = 0; pos < len; pos += 16) {
        int32_t buf[32];
        float fl[32];
        int i;
        for (; next < nev && evs[next].frame <= pos; next++) {
            if (evs[next].kind == 0) vs_engine_note_on(&eng, (uint32_t)evs[next].a, (uint32_t)evs[next].b);
            else if (evs[next].kind == 1) vs_engine_note_off(&eng, (uint32_t)evs[next].a);
            else if (evs[next].kind == 2) eng.bend14 = evs[next].a - 8192;
            else vs_engine_sustain(&eng, evs[next].a);
        }
        memset(buf, 0, sizeof buf);
        vs_engine_render(&eng, buf, 16);
        for (i = 0; i < 32; i++) fl[i] = (float)buf[i] * (float)(1u << VS_OUT_SHIFT) / 32768.0f;
        fwrite(fl, sizeof(float), 32, out);
    }
    fclose(out);
    return 0;
}
