/* SPDX-License-Identifier: GPL-3.0-only */
/* The law tables (firmware/gen/vs_tables.h) on this computer. tools/gen_tables.py already checked each against
 * Synth2's double as it wrote it; this checks what the firmware's C sees: the header compiles under the strict
 * flags, the tables have the sizes the engine indexes, their ends are what the laws say, and those that must rise
 * or fall do (an interpolated read between two entries then stays between them). */
#include <math.h>
#include <stdio.h>
#include "vs_tables.h"

static unsigned fails;
#define EXPECT(c, what) do { if (!(c)) { printf("FAIL %s (line %d)\n", what, __LINE__); fails++; } } while (0)
#define N(t) (sizeof(t) / sizeof((t)[0]))
#define RISES(t, strict, what) do { size_t i_; for (i_ = 1; i_ < N(t); i_++) if ((t)[i_] < (t)[i_ - 1] || ((strict) && (t)[i_] == (t)[i_ - 1])) { EXPECT(0, what); break; } } while (0)
#define FALLS(t, what) do { size_t i_; for (i_ = 1; i_ < N(t); i_++) if ((t)[i_] > (t)[i_ - 1]) { EXPECT(0, what); break; } } while (0)

int main(void)
{
    uint32_t i;
    EXPECT(N(VS_EXP2_Q30) == 257 && VS_EXP2_Q30[0] == 1u << 30 && VS_EXP2_Q30[256] == 1u << 31, "EXP2: 1.0 to 2.0");
    RISES(VS_EXP2_Q30, 1, "EXP2 rises");
    EXPECT(N(VS_NOTE_INC_Q32) == 12, "NOTE_INC: an octave");
    EXPECT(fabs(VS_NOTE_INC_Q32[9] / 4294967296.0 * 44100.0 / 32.0 - 440.0) < 1e-3, "NOTE_INC: A9 >> 5 is 440 Hz");
    for (i = 1; i < 12u; i++)
        EXPECT(fabs((double)VS_NOTE_INC_Q32[i] / VS_NOTE_INC_Q32[i - 1] - pow(2.0, 1.0 / 12.0)) < 1e-8, "NOTE_INC: a semitone apart");
    EXPECT(N(VS_ATK_STEP_Q30) == 128 && N(VS_ENV_C1_Q31) == 128 && N(VS_ENV_C16_Q31) == 128, "envelope tables: a knob each");
    FALLS(VS_ATK_STEP_Q30, "a longer attack is a smaller step");
    FALLS(VS_ENV_C1_Q31, "a longer decay is a smaller coefficient");
    EXPECT(fabs(VS_ATK_STEP_Q30[0] / 1073741824.0 - 1.0 / (0.0005 * 44100.0)) < 1e-5, "attack 0 is 0.5 ms");
    for (i = 0; i < 128u; i++) {                /* 16 frames of C1 are one block of C16 */
        double c1 = VS_ENV_C1_Q31[i] / 2147483648.0, c16 = VS_ENV_C16_Q31[i] / 2147483648.0;
        EXPECT(fabs((1.0 - pow(1.0 - c1, 16.0)) - c16) < 2e-4 * c16 + 1e-9, "C16 is 16 frames of C1");
    }
    EXPECT(N(VS_AMP_SUSTAIN_Q11) == 128 && VS_AMP_SUSTAIN_Q11[0] == 0 && VS_AMP_SUSTAIN_Q11[127] == 2048, "amp sustain: 0 to 1.0");
    RISES(VS_AMP_SUSTAIN_Q11, 0, "amp sustain rises");
    EXPECT(N(VS_AMP_GAIN_Q12) == 128 && VS_AMP_GAIN_Q12[0] == 0 && VS_AMP_GAIN_Q12[64] == 4096, "GAIN: silence at 0, 1.0 at 64");
    RISES(VS_AMP_GAIN_Q12, 0, "GAIN rises");
    EXPECT(N(VS_DB_GAIN_Q15) == 241 && VS_DB_GAIN_Q15[0] == 32768 && VS_DB_GAIN_Q15[160] == 3277, "DB_GAIN: 0 dB is 1.0, -20 dB is 0.1");
    EXPECT(N(VS_FLT_VEL_Q15) == 128, "FLT_VEL: a velocity each");
    EXPECT(N(VS_FM_DEV_Q32) == 128 && VS_FM_DEV_Q32[0] == 0, "FM 0 is no FM");
    EXPECT(fabs(VS_FM_DEV_Q32[127] / 4294967296.0 - 0.5) < 2e-3, "FM 127 deviates half a cycle a frame");
    RISES(VS_FM_DEV_Q32, 1, "FM rises");
    EXPECT(N(VS_SVF_F_Q30) == 509 && N(VS_LAD_A_Q31) == 509 && N(VS_LAD_COMP_Q15) == 509, "filter tables: 0..127 in quarters");
    RISES(VS_SVF_F_Q30, 0, "SVF f rises");
    RISES(VS_LAD_A_Q31, 0, "LP24 a rises");
    EXPECT(VS_SVF_F_Q30[500] == VS_SVF_F_Q30[508], "SVF f is held from knob 125");
    EXPECT(fabs(VS_SVF_F_Q30[0] / 1073741824.0 - 2.0 * sin(3.14159265358979 * 16.7 / 88200.0)) < 1e-8, "SVF f at knob 0: 16.7 Hz");
    EXPECT(N(VS_LIMIT_Q16) == 2050 && VS_LIMIT_Q16[0] == 65535 && VS_LIMIT_Q16[12] >= 65534, "the limiter: 1.0 at rest and under its floor");
    FALLS(VS_LIMIT_Q16, "the limiter's gain falls");
    EXPECT(N(VS_AGC_Q16) == 513 && VS_AGC_Q16[44] == 65535 && VS_AGC_Q16[45] < 65535, "the AGC: 1.0 up to its threshold 5.5");
    FALLS(VS_AGC_Q16, "the AGC's gain falls");
    EXPECT(N(VS_SAT_A_Q16) == 128 && VS_SAT_A_Q16[0] == 0, "SATURATION 0 is none");
    RISES(VS_SAT_A_Q16, 0, "SATURATION rises");
    EXPECT(N(VS_KBD_TRACK_Q16) == 128 && VS_KBD_TRACK_Q16[0] == 0, "KBD TRACK 0 is none");
    EXPECT(N(VS_LFO_INC_Q32) == 128 && N(VS_LFO_STEPS) == 128 && VS_LFO_STEPS[127] == 640, "LFO: a knob each; depth 127 is 6000 cents");
    EXPECT(fabs(VS_LFO_INC_Q32[0] / 4294967296.0 * 44100.0 * 12.0 - 1.0) < 1e-4, "LFO SPEED 0 is a 12 s cycle");
    RISES(VS_LFO_INC_Q32, 1, "LFO SPEED rises");
    RISES(VS_LFO_STEPS, 0, "LFO pitch depth rises");
    EXPECT(N(VS_UNI_DETUNE_Q8) == 128 && VS_UNI_DETUNE_Q8[0] == 0, "UNISON DETUNE 0 is none");
    EXPECT(N(VS_PORTA_A_Q31) == 128 && VS_PORTA_A_Q31[0] == 0, "PORTAMENTO 0 is off");
    EXPECT(N(VS_SLOT_OSC1_Q32) == VS_NSLOT && N(VS_SLOT_OSC2_Q32) == VS_NSLOT && N(VS_SLOT_COPY_Q32) == VS_NSLOT * 8u, "slot phases: VS_NSLOT slots");
    EXPECT((VS_OSC1_PHASE_MASK & 31u) == 0 && VS_NOISE_SEED < (1u << 20) && VS_T_CTL == 16u && VS_T_FS == 44100u, "constants");
    printf(fails ? "vs_tables_test: %u FAILED\n" : "vs_tables_test: ok\n", fails);
    return fails != 0;
}
