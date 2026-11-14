/* SPDX-License-Identifier: GPL-3.0-only */
/* The voice engine (firmware/src/vs_voice.h, Stage 4a) on this computer: what it does as a whole. How it SOUNDS
 * is tests/run_compare.sh's (against Synth2's own engine); this is the plumbing: notes in, voices, pitch, the
 * pedal, a program change, and that silence is silence. */
#include <math.h>
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

static unsigned fails;
#define EXPECT(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static vs_engine_t e, e2;
static int32_t buf[512];

static int32_t run(vs_engine_t *g, uint32_t buffers)   /* render; the peak */
{
    int32_t peak = 0;
    uint32_t b, i;
    for (b = 0; b < buffers; b++) {
        memset(buf, 0, sizeof buf);
        vs_engine_render(g, buf, 256);
        for (i = 0; i < 512u; i++) { int32_t a = buf[i] < 0 ? -buf[i] : buf[i]; if (a > peak) peak = a; }
    }
    return peak;
}
static uint32_t active(const vs_engine_t *g) { uint32_t i, n = 0; for (i = 0; i < VS_NV; i++) n += g->v[i].active; return n; }
static uint32_t gated(const vs_engine_t *g) { uint32_t i, n = 0; for (i = 0; i < VS_NV; i++) n += g->v[i].active && g->v[i].gate; return n; }
static void fresh(vs_engine_t *g) { vs_engine_init(g); vs_param_set(&g->patch, VS_P_AENV_R, 20); }

int main(void)
{
    uint32_t n, i;
    int32_t peak;
    vs_patch_t p;

    fresh(&e);
    EXPECT(run(&e, 4) == 0 && active(&e) == 0, "silence before a note");
    vs_engine_note_on(&e, 69, 127);
    peak = run(&e, 40);
    EXPECT(peak > 6000 && peak < 12000 && active(&e) == 1 && e.act == 1, "INIT on A4: one voice, a peak near 0.2 x 1.18 of full scale");
    EXPECT(buf[0] == buf[1] && buf[100] == buf[101], "mono to both sides");
    vs_engine_note_off(&e, 69);
    run(&e, 200);
    EXPECT(active(&e) == 0 && run(&e, 4) == 0, "a released voice ends, and then nothing sounds at all");

    for (n = 0; n < 128u; n++) {                /* pitch: every note, to OSC1's own 2^-27-cycle grid */
        double want = 440.0 * pow(2.0, ((double)n - 69.0) / 12.0) / 44100.0 * 4294967296.0, got;
        fresh(&e);
        vs_engine_note_on(&e, n, 100);
        run(&e, 1);
        got = (double)e.v[0].inc1;
        EXPECT(got <= want + 1.0 && got > want - 33.0 - want * 1e-6, "OSC1's step: the note's, truncated to 2^-27 cycle");
        EXPECT(fabs((double)e.v[0].inc2 - want) < 2.0 + want * 1e-6, "OSC2's step: the note's");
        if (fails) break;
    }
    fresh(&e);                                  /* the bend: full up at BEND 12 is an octave, exactly 128 steps */
    vs_param_set(&e.patch, VS_P_BEND_RANGE, 12);
    vs_engine_note_on(&e, 60, 100);
    run(&e, 1);
    i = e.v[0].inc2;
    e.bend14 = 8191; run(&e, 1);
    EXPECT(e.v[0].inc2 == 2u * i || e.v[0].inc2 + 1u == 2u * i, "bend full up at BEND 12: an octave");
    e.bend14 = -8192; run(&e, 1);
    EXPECT(e.v[0].inc2 == i / 2u || e.v[0].inc2 + 1u == i / 2u, "bend full down: an octave down");
    e.bend14 = 127; run(&e, 1);
    EXPECT(e.v[0].inc2 == i, "only the bend's top 7 bits count");
    fresh(&e);                                  /* above note 127 an oscillator is silent */
    vs_param_set(&e.patch, VS_P_OSC2_PITCH, 127);
    vs_engine_note_on(&e, 100, 100);
    run(&e, 1);
    EXPECT(e.v[0].mute2 && !e.v[0].mute1, "OSC2 above note 127: silent");

    fresh(&e);                                  /* voices: the round robin, the limit, stealing */
    for (n = 0; n < VS_NV; n++) { vs_engine_note_on(&e, 40 + n, 100); run(&e, 1); EXPECT(e.v[n].active && e.v[n].note == 40 + n, "notes take the voices in turn"); }
    vs_engine_note_on(&e, 80, 100); run(&e, 1);
    EXPECT(active(&e) == VS_NV && e.v[0].note == 80, "at the limit the oldest held voice goes");
    vs_engine_note_off(&e, 44); run(&e, 1);
    vs_engine_note_on(&e, 81, 100); run(&e, 1);
    EXPECT(e.v[4].note == 81, "a released voice goes before a held one");
    fresh(&e);
    vs_param_set(&e.patch, VS_P_POLY, 2);
    for (n = 0; n < 4u; n++) { vs_engine_note_on(&e, 50 + n, 100); run(&e, 1); }
    EXPECT(active(&e) == 2, "POLY 2: two voices");
    fresh(&e);
    e.nv = 3;
    for (n = 0; n < 6u; n++) { vs_engine_note_on(&e, 50 + n, 100); run(&e, 1); }
    EXPECT(active(&e) == 3, "GLOBAL's VOICES 3: three voices");
    fresh(&e);
    for (n = 0; n < VS_NV + 2u; n++) vs_engine_note_on(&e, 50 + n, 100);   /* a chord over the limit, in one block */
    run(&e, 1);
    EXPECT(active(&e) == VS_NV && e.v[0].note == 50, "a chord over the limit: its last notes are dropped, none is stolen");
    fresh(&e);
    vs_engine_note_on(&e, 60, 100); run(&e, 1);
    vs_engine_note_on(&e, 60, 100); run(&e, 1);
    EXPECT(active(&e) == 2 && gated(&e) == 1 && e.v[1].gate, "a key pressed again: its voice is released, a fresh one starts");

    fresh(&e);                                  /* the pedal */
    vs_engine_sustain(&e, 1);
    vs_engine_note_on(&e, 60, 100); run(&e, 2);
    vs_engine_note_off(&e, 60); run(&e, 20);
    EXPECT(gated(&e) == 1, "the pedal holds a released key");
    vs_engine_sustain(&e, 0); run(&e, 1);
    EXPECT(gated(&e) == 0, "the pedal up lets it go");
    vs_engine_note_on(&e, 61, 100); run(&e, 2);
    vs_engine_all_off(&e); run(&e, 1);
    EXPECT(active(&e) == 0 && run(&e, 2) == 0, "all off: at once");

    fresh(&e);                                  /* a program change */
    vs_engine_note_on(&e, 60, 100); run(&e, 4);
    vs_patch_init(&p);
    vs_param_set(&p, VS_P_OSC1_SHAPE, 3);
    memcpy(p.name, "TEST            ", 16);
    EXPECT(vs_engine_load(&e, &p) == 0 && vs_engine_load(&e, &p) == -1, "a load is taken; a second while it lands is not");
    memset(buf, 0, sizeof buf);
    vs_engine_render(&e, buf, 256);
    EXPECT(buf[2 * 255] == 0 && buf[0] != 0, "the buffer a load lands in fades to silence");
    EXPECT(active(&e) == 0 && e.patch.v[VS_P_OSC1_SHAPE] == 3 && !memcmp(e.patch.name, "TEST", 4) && e.load_req == 0, "then the new patch is in and no voice sounds");
    EXPECT(vs_engine_load(&e, &p) == 0, "and the next load is taken");

    fresh(&e);                                  /* a full chord stays inside full scale; and the same twice */
    fresh(&e2);
    for (n = 0; n < VS_NV; n++) { vs_engine_note_on(&e, 36 + 5 * n, 127); vs_engine_note_on(&e2, 36 + 5 * n, 127); }
    peak = run(&e, 100);
    EXPECT(peak <= 31784, "8 voices of the INIT patch: the output limiter holds them at 0.97 of full scale");
    run(&e2, 100);
    EXPECT(!memcmp(e.v, e2.v, sizeof e.v), "the same notes twice: the same voices, bit for bit");
    printf(fails ? "vs_voice_test: %u FAILED\n" : "vs_voice_test: ok (8-voice peak %d of 32767)\n", fails ? fails : (unsigned)peak);
    return fails != 0;
}
