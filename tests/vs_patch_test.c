/* SPDX-License-Identifier: GPL-3.0-only */
/* The patch (firmware/src/vs_patch.h) on this computer: INIT, clamping, the record there and back, the panel's
 * CCs both ways, what values read as, and the factory bank (firmware/gen/vs_factory.h): every record unpacks, 001
 * is INIT. With an argument it prints the factory bank as "slot v0 .. v98" lines instead: tests/sy1_test.py
 * compares them with what tools/sy1.py reads from the .sy1 files, so the C and the Python agree on the record. */
#include <stdio.h>
#include <string.h>
#include "../firmware/src/vs_patch.c"
#include "vs_factory.h"

static unsigned fails;
#define EXPECT(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static int text_is(uint32_t id, int32_t v, int sync, int32_t fx, const char *want)
{
    char s[16];
    uint32_t n = vs_param_text(id, v, sync, fx, s, sizeof s);
    if (strcmp(s, want) || n != strlen(want)) { printf("  %u = %d reads \"%s\", not \"%s\"\n", (unsigned)id, (int)v, s, want); return 0; }
    return 1;
}

int main(int argc, char **argv)
{
    vs_patch_t p, q;
    vs_rec_t r;
    uint32_t i, k, n, rng = 12345u;
    (void)argv;
    if (argc > 1) {                             /* the dump for tests/sy1_test.py */
        for (i = 0; i < 128u; i++) {
            memcpy(r.b, VS_FACTORY[i], VS_REC);
            if (vs_patch_unpack(&r, &p)) continue;
            printf("%u %u %.16s|", (unsigned)i + 1u, (unsigned)p.color, p.name);
            for (k = 0; k < VS_NPARAM; k++) printf(" %d", (int)p.v[k]);
            printf("\n");
        }
        return 0;
    }
    vs_patch_init(&p);
    for (i = 0; i < VS_NPARAM; i++) EXPECT(p.v[i] == VS_PARAMS[i].init, "INIT values");
    EXPECT(!memcmp(p.name, "INIT            ", 16) && vs_patch_sanitize(&p) == 0, "INIT is clean");
    vs_param_set(&p, VS_P_FLT_AMOUNT, 128); EXPECT(p.v[VS_P_FLT_AMOUNT] == 127, "set clamps high");
    vs_param_set(&p, VS_P_KEY_SHIFT, -99); EXPECT(p.v[VS_P_KEY_SHIFT] == -24, "set clamps low");
    vs_param_set(&p, VS_P_LFO1_DEST, 0); EXPECT(p.v[VS_P_LFO1_DEST] == 1, "a 1-based choice clamps to 1");

    for (n = 0; n < 2000u; n++) {               /* random patches: the record there and back */
        for (i = 0; i < VS_NPARAM; i++) {
            const vs_param_info_t *pi = &VS_PARAMS[i];
            rng = rng * 1664525u + 1013904223u;
            p.v[i] = pi->min + (int32_t)((rng >> 8) % (uint32_t)(pi->max - pi->min + 1));
            if (pi->kind >= VS_K_SRC && p.v[i] == 65535) p.v[i] = -1;   /* (the one word a record can not hold) */
        }
        for (i = 0; i < VS_NAME; i++) { rng = rng * 1664525u + 1013904223u; p.name[i] = (char)(32u + (rng >> 8) % 95u); }
        p.color = (uint8_t)(n % VS_NCOLOR);
        vs_patch_pack(&p, &r);
        for (i = 0; i < VS_REC; i++)
            if (r.b[i] > 127u && (i < VS_NAME + VS_NREC || i == VS_REC - 1u)) { EXPECT(0, "a record byte outside the routing words is 7-bit"); break; }
        EXPECT(!vs_rec_empty(&r), "a packed patch is not EMPTY");
        EXPECT(vs_patch_unpack(&r, &q) == 0 && !memcmp(p.v, q.v, sizeof p.v) && !memcmp(p.name, q.name, VS_NAME) && p.color == q.color, "record round trip");
        if (fails) break;
    }
    memset(&r, 0, sizeof r); r.b[VS_NAME] = VS_REC_EMPTY;
    EXPECT(vs_rec_empty(&r) && vs_patch_unpack(&r, &q) == 1, "an EMPTY record");
    memset(&r, 0xFF, sizeof r); r.b[VS_NAME] = 0;   /* rubbish: unpacks to something in range */
    EXPECT(vs_patch_unpack(&r, &q) == 0 && vs_patch_sanitize(&q) == 0, "a rubbish record comes out clean");

    for (k = 0; k < 128u; k++) {                /* the CCs: every value there and back */
        uint32_t id = VS_CC_PARAM[k];
        if (id == 0xFFu) continue;
        if (id >= VS_NPARAM) { EXPECT(0, "CC table: a parameter that is not one"); continue; }
        EXPECT(VS_PARAMS[id].cc == k, "CC table and VS_PARAMS agree");
        for (i = 0; i < 128u; i++) {
            int32_t got = vs_cc_apply(&p, k, i);
            EXPECT(got == (int32_t)id && p.v[id] >= VS_PARAMS[id].min && p.v[id] <= VS_PARAMS[id].max, "a CC lands in range");
            vs_cc_apply(&q, k, vs_cc_value(id, p.v[id]));
            EXPECT(q.v[id] == p.v[id], "vs_cc_value is vs_cc_apply's inverse");
        }
        vs_cc_apply(&p, k, 0); EXPECT(p.v[id] == VS_PARAMS[id].min, "CC 0 is the minimum");
        vs_cc_apply(&p, k, 127); EXPECT(p.v[id] == VS_PARAMS[id].max, "CC 127 is the maximum");
    }
    EXPECT(vs_cc_apply(&p, 1, 64) == -1 && vs_cc_apply(&p, 64, 127) == -1, "the mod wheel and the pedal are not the panel's");

    EXPECT(text_is(VS_P_FLT_TYPE, 1, 0, -1, "LP24") && text_is(VS_P_FLT_TYPE, 4, 0, -1, "LPDL"), "filter types");
    EXPECT(text_is(VS_P_OSC2_SHAPE, 1, 0, -1, "SAW") && text_is(VS_P_OSC2_SHAPE, 4, 0, -1, "NOISE"), "a 1-based choice");
    EXPECT(text_is(VS_P_OSC2_PITCH, 64, 0, -1, "0 st") && text_is(VS_P_OSC2_PITCH, 51, 0, -1, "-12 st"), "OSC2 PITCH");
    EXPECT(text_is(VS_P_OSC2_FINE, 64, 0, -1, "0 ct") && text_is(VS_P_OSC_TUNE, 127, 0, -1, "+61 ct"), "FINE, TUNE");
    EXPECT(text_is(VS_P_KEY_SHIFT, -24, 0, -1, "-24 st") && text_is(VS_P_UNI_PITCH, 24, 0, -1, "0 st") && text_is(VS_P_UNI_PITCH, 48, 0, -1, "+24 st"), "KEY SHIFT, UNISON PITCH");
    EXPECT(text_is(VS_P_LFO1_SPEED, 0, 0, -1, "0.08 Hz") && text_is(VS_P_LFO1_SPEED, 127, 0, -1, "125.0 Hz"), "LFO SPEED in Hz");
    EXPECT(text_is(VS_P_LFO2_SPEED, 0, 1, -1, "16 BARS") && text_is(VS_P_LFO2_SPEED, 127, 1, -1, "1/64T"), "LFO SPEED with TEMPO");
    EXPECT(text_is(VS_P_DLY_TIME, 0, 0, -1, "MIN") && text_is(VS_P_DLY_TIME, 19, 0, -1, "1 BAR") && text_is(VS_P_ARP_BEAT, 11, 0, -1, "1/8"), "note values");
    EXPECT(text_is(VS_P_PAN, 64, 0, -1, "0") && text_is(VS_P_PAN, 0, 0, -1, "-64") && text_is(VS_P_EQ_TONE, 127, 0, -1, "+63"), "signed values");
    EXPECT(text_is(VS_P_FX_LEVEL, 100, 0, 0, "100") && text_is(VS_P_FX_LEVEL, 127, 0, 6, "+100%") && text_is(VS_P_FX_LEVEL, 0, 0, 9, "-100%") && text_is(VS_P_FX_LEVEL, 64, 0, 7, "0%") && text_is(VS_P_FX_LEVEL, 65, 0, 7, "+2%"), "EFFECT LEVEL: a level, the phasers' feedback");
    EXPECT(text_is(VS_P_DLY_ON, 1, 0, -1, "ON") && text_is(VS_P_ARP_ON, 0, 0, -1, "OFF"), "switches");
    EXPECT(text_is(VS_P_CTRL_SRC1, 45057, 0, -1, "CC 1") && text_is(VS_P_CTRL_SRC2, 57344, 0, -1, "BEND") && text_is(VS_P_CTRL_SRC1, 53248, 0, -1, "PRESS") && text_is(VS_P_CTRL_SRC1, -1, 0, -1, "NONE"), "routing sources");
    EXPECT(text_is(VS_P_CTRL_DST1, 44, 0, -1, "DEPTH") && text_is(VS_P_CTRL_DST2, -1, 0, -1, "NONE"), "routing assigns");
    EXPECT(text_is(VS_P_POLY, 16, 0, -1, "16") && text_is(VS_P_FLT_FREQ, 127, 0, -1, "127"), "plain numbers");
    EXPECT(!strcmp(vs_param_label(VS_P_FX_CTRL1, 3), "RATE") && !strcmp(vs_param_label(VS_P_FX_LEVEL, 6), "FEEDBK") && !strcmp(vs_param_label(VS_P_FX_CTRL2, -1), "CTRL 2") && !strcmp(vs_param_label(VS_P_FLT_FREQ, 3), "FREQ"), "labels: the EFFECT knobs by type");
    { char s[4]; EXPECT(vs_param_text(VS_P_OSC2_PITCH, 51, 0, -1, s, sizeof s) == 3 && !strcmp(s, "-12"), "a text is cut at cap, NUL ended"); }
    for (i = 0; i < VS_NPARAM; i++) {           /* every value of every parameter reads as something that fits */
        int32_t v, step = VS_PARAMS[i].max - VS_PARAMS[i].min > 200 ? 257 : 1;
        for (v = VS_PARAMS[i].min; v <= VS_PARAMS[i].max; v += step) {
            char s[12];
            n = vs_param_text(i, v, 1, 6, s, sizeof s);
            EXPECT(n > 0 && n < 11u, "a value's text is 1..10 characters");
        }
    }

    n = 0;
    for (i = 0; i < 128u; i++) {                /* the factory bank */
        memcpy(r.b, VS_FACTORY[i], VS_REC);
        if (vs_rec_empty(&r)) continue;
        n++;
        EXPECT(vs_patch_unpack(&r, &q) == 0, "a factory record unpacks");
        vs_patch_pack(&q, &r);
        EXPECT(!memcmp(r.b, VS_FACTORY[i], VS_REC), "a factory record is as vs_patch_pack would write it");
    }
    EXPECT(n == VS_FACTORY_N && n > 0, "the factory bank's count");
    vs_patch_init(&p); vs_patch_pack(&p, &r);
    EXPECT(!memcmp(r.b, VS_FACTORY[0], VS_REC), "factory 001 is the INIT patch, byte for byte");
    printf(fails ? "vs_patch_test: %u FAILED\n" : "vs_patch_test: ok (%u factory patches)\n", fails ? fails : n);
    return fails != 0;
}
