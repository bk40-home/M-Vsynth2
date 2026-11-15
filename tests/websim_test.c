/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Kris (bk40-home) */
/* The online simulator's platform (sim/mv_websim.c) tested natively, as M-vance's tests/websim_tiles_test.c is: the
 * real app and the sim backend built with cc, tests/websim_stub/emscripten.h in place of Emscripten's (no emcc), this
 * file playing the page.     tests/run_websim.sh   (exit 0 = every check passed)
 *
 * Checks: the app boots and draws (the screen is not one colour); a key held sounds (the audio is not silent) and the
 * key let go falls quiet; an encoder and a button reach the app (the screen changes); IDENT through wsim_sysex_in is
 * answered with an IDENT naming M-VSYNTH2 (the patch library's handshake, vs_xfer.h); LINK is answered with an EDIT,
 * and a knob turned is reported (the simulator's Link FM-1). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int wsim_pull(float *dst, int max_frames);
uint16_t *wsim_fb(void);
void wsim_keys(uint32_t mask);
void wsim_btn(int id, int down);
void wsim_enc(int id, int detents);
void wsim_sysex_in(const uint8_t *p, int n);

static uint32_t checks, fails, steps;
static uint8_t ident[64];
static int ident_n;
static float abuf[2048 * 2];
static uint16_t shot[240 * 240];

void wsim_midi_out(int st, int d1, int d2) { (void)st; (void)d1; (void)d2; }
static uint8_t edit[2][160];
static int edit_n;
void wsim_sysex_out(const uint8_t *p, int n)
{
    if (n >= 6 && p[4] == 0x15 && n <= 64) { memcpy(ident, p, (size_t)n); ident_n = n; }
    if (n == 145 && p[4] == 0x17) { memcpy(edit[edit_n & 1], p, 145); edit_n++; }   /* an EDIT (the link) */
}
static void ok(int c, const char *what) { checks++; if (!c) { fails++; printf("FAIL %s\n", what); } else printf("ok   %s\n", what); }
static float peak(void)                     /* drain the ring as the page does: the loudest sample */
{
    float m = 0;
    int n, i;
    while ((n = wsim_pull(abuf, 2048)) > 0)
        for (i = 0; i < 2 * n; i++) { float v = abuf[i] < 0 ? -abuf[i] : abuf[i]; if (v > m) m = v; }
    return m;
}
static int colours(void)                    /* distinct colours on the screen (capped) */
{
    static uint16_t seen[64];
    int k = 0, i, j;
    const uint16_t *fb = wsim_fb();
    for (i = 0; i < 240 * 240 && k < 64; i++) {
        for (j = 0; j < k && seen[j] != fb[i]; j++) {}
        if (j == k) seen[k++] = fb[i];
    }
    return k;
}
static float loud_on, loud_off;

/* platform_poll's yield (the browser's turn): the page's script, one step a call; ~5.8 ms of audio a step */
void wsim_test_yield(void)
{
    steps++;
    if (steps < 300) { peak(); return; }                         /* boot, ~1.7 s */
    if (steps == 300) { ok(colours() > 3, "boots and draws the screen"); memcpy(shot, wsim_fb(), sizeof shot); wsim_keys(1u << 7); return; }
    if (steps < 400) { float p = peak(); if (p > loud_on) loud_on = p; return; }
    if (steps == 400) { ok(loud_on > 0.01f, "a key held sounds"); wsim_keys(0); return; }
    if (steps < 900) { float p = peak(); if (steps > 800 && p > loud_off) loud_off = p; return; }
    if (steps == 900) { ok(loud_off < loud_on * 0.25f, "the key let go falls quiet"); wsim_btn(4, 1); return; }   /* ENV */
    if (steps == 905) { wsim_btn(4, 0); return; }
    if (steps == 960) { ok(memcmp(shot, wsim_fb(), sizeof shot) != 0, "a button reaches the app (the screen changes)"); memcpy(shot, wsim_fb(), sizeof shot); wsim_enc(2, 5); return; }
    if (steps == 1020) { ok(memcmp(shot, wsim_fb(), sizeof shot) != 0, "an encoder reaches the app (the screen changes)");
        { static const uint8_t m[] = {0xF0, 0x7D, 0x4D, 0x56, 0x05, 0x7B, 0xF7}; wsim_sysex_in(m, (int)sizeof m); } return; }
    if (steps == 1060) {
        ok(ident_n > 14 && memcmp(ident + 5, "M-VSYNTH2", 9) == 0, "IDENT is answered (the patch library's handshake)");
        { static const uint8_t m[] = {0xF0, 0x7D, 0x4D, 0x56, 0x07, 0x79, 0xF7}; wsim_sysex_in(m, (int)sizeof m); }   /* LINK */
        return;
    }
    if (steps == 1100) { ok(edit_n == 1, "LINK is answered with one EDIT (what plays)"); wsim_enc(2, 4); return; }
    if (steps == 1160) {
        ok(edit_n == 2 && memcmp(edit[0], edit[1], 145) != 0, "a knob turned while linked: one EDIT with the change");
        printf("%u checks, %u failed\n", checks, fails);
        exit(fails ? 1 : 0);
    }
    peak();
}
