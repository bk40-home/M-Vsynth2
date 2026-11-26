/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2: Synth2 (the open-source recreation of Synth1) for the M-VAVE FM-1, built on Dinghy.
 * The engine (vs_voice.h, vs_bus.h) plays Synth2's patches as Synth2 does (tests/run_compare.sh); this file is the
 * panel, the banks (vs_prog.h), the transfer (vs_xfer.h) and MIDI (proposal sections 7, 6, 8).
 *
 * The panel (proposal 7.1, 7.2; the pages: firmware/gen/vs_params.h VS_PAGES, all 99 parameters):
 *   LFO EDIT ENV SEL FX GLO   a section: the first press opens its page (the one last seen), the next steps
 *                             through its pages (GLO ends on GLOBAL: MIDI CH, VOICES, TEMPO; THEME, COLOUR, BT MIDI;
 *                             KEYS: TOUCH, CURVE, SENS; DIAG)
 *   ARP        a tap: the arp on / off (ARP ON, parameter 59: the patch); held: the ARP page
 *   KNOB1..4   the page's four columns (HOME: FREQ, RES, AMOUNT, GAIN; BANK: the bank; SAVE: bank and slot)
 *   SELECT     the column last turned, a step at a time (BANK: the cursor)
 *   ALGORITHM  every page in turn
 *   PRESETS    the program: the slots of the bank shown that hold a patch
 *   HOME       the home screen          SEQ   the bank browser (moving the cursor plays the slot)
 *   SAVE       the save screen: KNOB1 the bank (U1..U8), KNOB2 the slot; SAVE again writes (to flash once nothing
 *              sounds), any other button leaves
 *   OCT- OCT+  the octave, -2..+2; both held 1 s: all notes off
 * The screens (vs_ui.c): a page (the section, a graph of what it does, four knobs, the program at the foot); HOME
 * (the program, a scope, the four knobs, CPU and its 3 s peak); BANK (nine slots around the cursor); SAVE; GLOBAL 1/2
 * (MIDI CH, VOICES, TEMPO, CLOCK) and 2/2 (THEME, COLOUR); the transfer at the foot while the computer talks. Values
 * read as Synth2 shows them (vs_param_text).
 *
 * MIDI in (USB, the TRS jack or Bluetooth: GLOBAL 2/4 BLUETOOTH): notes (GLOBAL's channel, or any), velocity, the bend, CC 64, CC 120 / 123, program
 * change (a slot of the bank shown; an empty one is ignored, as in Synth2), bank select (CC 0: 0 FACTORY, 1..8),
 * channel pressure and every CC for the CTRL routes, the panel's CC map (proposal 8), clock (F8: the tempo while it
 * runs; else GLOBAL's TEMPO). Out: the keys' notes on channel 1 (with GLOBAL KEYS TOUCH ON: their velocity, and
 * the pressure of F3..C5 as channel pressure, which feeds the CTRL routes' PRESS too: fm1-kit fk_touch). SysEx 7D 4D 56: the transfer (vs_xfer.h); 7D 53 52: the
 * screen reader (vs_speak.c, fm1-reader: the screen as words, silent until a reader says HELLO).
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "M-VSYNTH2"                    /* the rescue and crash screens (platform.c) */
#define FK_BLE_NAME "M-Vsynth2"                 /* Bluetooth MIDI: "M-Vsynth2 XXXX" (platform.c, fm1-kit fk_ble) */
#define STORE_MAGIC 0x3253564Du                /* "MVS2": our settings never read another fork's (store.h) */
#define APP_SYSEX_ID 0x7Du                      /* our SysEx's first byte (the transfer, vs_xfer.h; platform.c) */
#include <stdint.h>
static void app_idle(void);
static void app_sysex(const uint8_t *p, uint32_t n);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#define APP_SYSEX app_sysex
#define SXQ 128u                                /* the SysEx send queue holds a whole screen-reader SAY (vs_speak.c; usb.c) */
#include "platform.c"
#ifndef FK_BLE
#define FK_BLE 0                                /* (platform.c sets it on the FM-1) */
#endif
#if !FK_BLE                                     /* (a computer, the browser, fm1-dock, --no-ble: no Bluetooth) */
enum { FK_BLE_OFF, FK_BLE_ADV, FK_BLE_LINKED };
static void fk_ble_enable(int on) { (void)on; }
static int fk_ble_state(void) { return FK_BLE_OFF; }
#endif
#include "vs_voice.h"                          /* the engine; its code is vs_dsp.c, built apart at -O2 */
#include "vs_prog.c"                           /* the banks (main loop) */
#include "vs_xfer.c"                           /* the transfer (main loop) */

/* ---- the sound: the ISR renders and times itself; the main loop reads the sums and asks for their clearing */
static vs_engine_t eng;
static volatile uint32_t cpu_sum, cpu_n, cpu_peak, cpu_clear;
#define UI_SCOPE_N 220u
static volatile int16_t scope[UI_SCOPE_N];      /* HOME's scope: the ISR fills it when asked (scope_req), vs_ui.c draws it */
static volatile uint32_t scope_req, scope_seq, scope_pk;   /* scope_pk: the last capture's peak (the tail still heard?) */
/* the output still heard (issue #11): a voice's release is over before the FX's tail is (the delay's echoes, the
 * chorus), and a flash erase there (the settings, a bank: ~50 ms a sector with the interrupts off) cut the tail with
 * a glitch and stalled the LED scan. The ISR sets it when a buffer has a sample over OUT_HEARD; the main loop takes it */
#define OUT_HEARD 32                            /* Q15: about -60 dBFS */
static volatile uint8_t out_heard;

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), dt, i;
    vs_engine_render(&eng, out, n);
    for (i = 0; i + 1u < 2u * n; i += 8u)      /* every 4th frame, L and R: a tail is far slower than that */
        if (out[i] > OUT_HEARD || out[i] < -OUT_HEARD || out[i + 1u] > OUT_HEARD || out[i + 1u] < -OUT_HEARD) { out_heard = 1; break; }
    dt = fm1_ticks() - t0;
    if (cpu_clear) { cpu_sum = 0; cpu_n = 0; cpu_peak = 0; cpu_clear = 0; }
    cpu_sum += dt;
    cpu_n++;
    if (dt > cpu_peak) cpu_peak = dt;
    if (scope_req && n >= 256u) {               /* the scope: 220 frames of L + R from a rising zero crossing */
        uint32_t i, s0 = 0;
        for (i = 0; i < 35u; i++)
            if (out[2u * i] + out[2u * i + 1u] < 0 && out[2u * i + 2u] + out[2u * i + 3u] >= 0) { s0 = i + 1u; break; }
        uint32_t pk = 0;
        for (i = 0; i < UI_SCOPE_N; i++) {
            const int32_t v = (out[2u * (s0 + i)] + out[2u * (s0 + i) + 1u]) / 2;
            scope[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
            if ((uint32_t)(v < 0 ? -v : v) > pk) pk = (uint32_t)(v < 0 ? -v : v);
        }
        scope_pk = pk;
        scope_seq++;
        scope_req = 0;
    }
}

/* ---- settings (flash: settings_save, once nothing sounds) */
static struct {
    uint8_t ch;                                 /* MIDI channel: 0 any, 1..16 */
    uint8_t voices;                             /* GLOBAL VOICES: 1..VS_NV */
    uint8_t tempo;                              /* GLOBAL TEMPO, bpm: 40..240 */
    uint8_t bank, slot;                         /* what played last */
    int8_t oct;
    uint8_t theme, colour;                      /* GLOBAL 2/2: the screen's theme (vs_ui.c UI_THEMES), COLOUR 0 THEME, 1 PATCH */
    uint8_t bt;                                 /* GLOBAL 2/4 BLUETOOTH: 1 ON (Kris 19 Nov). A record from before
                                                   (one byte shorter) still loads, with ON (app_main) */
    uint8_t touch;                              /* GLOBAL 3/4 KEYS (Kris 22 Nov): TOUCH 1 ON (velocity and pressure from
                                                   the keys, fm1-kit fk_touch), VEL CURVE and PRESS SENS -5..+5. Last: a
                                                   record from before still loads, with TOUCH OFF, 0, 0 */
    int8_t vcurve, psens;
    uint8_t ffav[VS_NPROG / 8u];                /* FACTORY's favourites (Kris 26 Nov): a bit a slot, as FACTORY can't be
                                                   written (U1..U8 keep the star in the patch, VS_REC_FAV). Last: a record
                                                   from before still loads, with none */
} set;
static uint8_t set_dirty;
/* a setting changed: saved once settings have rested 2 s (scrolling PRESETS or turning a GLOBAL knob is never cut by
 * a flash erase, which holds the interrupts off and so loses the knob's steps), and never while GLOBAL is up: leaving
 * it saves (Kris 19 Nov) */
static uint32_t set_chg;
static void set_changed(void) { set_dirty = 1; set_chg = fm1_ms; }
static int ffav_get(uint32_t s) { return s < VS_NPROG && (set.ffav[s >> 3] >> (s & 7u) & 1u); }
static int ffav_set(uint32_t s, int on)         /* 1 changed (saved with the settings), 0 already so */
{
    const uint8_t m = (uint8_t)(1u << (s & 7u));
    if (s >= VS_NPROG || ffav_get(s) == (on != 0)) return 0;
    if (on) set.ffav[s >> 3] |= m; else set.ffav[s >> 3] &= (uint8_t)~m;
    set_changed();
    return 1;
}

/* ---- programs */
static vs_prog_t prog;
static vs_xfer_t xfer;
static uint32_t slot, slot_want, xfer_t;
static int32_t bank_want = -1;
static uint8_t edited, want_reload;
static uint8_t flash_ok_now;                    /* nothing heard for 0.5 s: a flash erase may go now (main loop, #11) */
static char prog_name[VS_NAME + 1u];

static void prog_service(void)                  /* main loop: bring the slot asked for in, once the engine takes it */
{
    vs_patch_t p;
    uint32_t i;
    if (bank_want >= 0 && (uint32_t)bank_want != prog.bank) {   /* a bank change: the old one to flash first */
        if (prog.dirty && (!flash_ok_now || vs_prog_flush(&prog))) return;   /* (heard: the next loop; #11) */
        vs_prog_load(&prog, (uint32_t)bank_want);
        set.bank = prog.bank; set_changed();
        slot_want = vs_prog_empty(&prog, slot_want) ? vs_prog_next(&prog, slot_want, 1) : slot_want;
        want_reload = 1;
    }
    bank_want = -1;
    if (xfer.play_due) {                        /* an audition from the computer: plays, nothing saved */
        if (!vs_engine_load(&eng, &xfer.play)) {
            xfer.play_due = 0;
            for (i = 0; i < VS_NAME; i++) prog_name[i] = xfer.play.name[i];
            for (i = VS_NAME; i > 0u && prog_name[i - 1u] == ' '; i--) prog_name[i - 1u] = 0;
            edited = 1;
        }
        return;
    }
    if (slot_want == slot && !want_reload) return;
    if (vs_prog_get(&prog, slot_want, &p)) { slot_want = slot; want_reload = 0; return; }   /* empty: stays */
    if (!prog.bank) p.fav = (uint8_t)ffav_get(slot_want);   /* (FACTORY's star: SAVE carries it to a user slot) */
    if (vs_engine_load(&eng, &p)) return;       /* one is still landing: the next loop */
    slot = slot_want; want_reload = 0; edited = 0;
    set.slot = (uint8_t)slot; set_changed();
    for (i = 0; i < VS_NAME; i++) prog_name[i] = p.name[i];
    prog_name[VS_NAME] = 0;
    for (i = VS_NAME; i > 0u && prog_name[i - 1u] == ' '; i--) prog_name[i - 1u] = 0;   /* (centred: no trailing spaces) */
}

static int ann_frame(const uint8_t *p, uint32_t n, uint32_t now);   /* the screen reader's (vs_speak.c, announce.c) */
static void app_sysex(const uint8_t *p, uint32_t n)   /* platform_poll: main loop */
{
    if (ann_frame(p, n, fm1_ms)) return;        /* 7D 53 52: the screen reader (ours, the transfer, is 7D 4D 56) */
    if (!vs_xfer_rx(&xfer, &prog, p, n, eng.act == 0u)) return;
    xfer_t = fm1_ms ? fm1_ms : 1u;
    if (xfer.select_bank >= 0) {
        bank_want = xfer.select_bank;
        slot_want = (uint32_t)xfer.select_slot;
        if (prog.bank == (uint32_t)bank_want) want_reload = 1;
        xfer.select_bank = xfer.select_slot = -1;
    }
}

/* ---- MIDI clock (the tempo while it runs) and the LCD's idle hook */
static uint32_t idle_ok, clk_t, clk_n, clk_t0;

/* ---- the screen (vs_ui.c draws it) */
#define BUF_US (HALF_FRAMES * 1000000u / FS)    /* the buffer: 256 frames at 44.1 kHz = 5.8 ms */

static char *put_u(char *p, uint32_t v)
{
    char t[10];
    uint32_t n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) *p++ = t[--n];
    return p;
}
static char *put_s(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_3(char *p, uint32_t v) { *p++ = (char)('0' + v / 100u % 10u); *p++ = (char)('0' + v / 10u % 10u); *p++ = (char)('0' + v % 10u); return p; }

enum { SCR_HOME, SCR_PAGE, SCR_BANK, SCR_SAVE, SCR_GLOBAL };
static uint32_t scr = SCR_HOME, page, col, scr_drawn = ~0u, page_drawn = ~0u;
static uint8_t last_page[7];                    /* a section's page last seen (index into VS_PAGES) */
static uint32_t cursor, save_bank = 1, save_slot, need_full = 1;
/* SEQ's FAVOURITES (issue #12, Kris 25 Nov): KNOB 1 past U8 lists every starred patch, FACTORY's then U1..U8's;
 * moving the cursor plays one (its bank comes in). KNOB 2 stars (right) or unstars (left) the slot under the cursor.
 * U1..U8: the star is in the patch (vs_patch.h VS_REC_FAV), so SAVE and the library page carry it. FACTORY (Kris
 * 26 Nov): a bit in the settings (set.ffav), as FACTORY can't be written; SAVE still carries it to a user slot. The
 * list is built when it is opened and when something changes it (a star, the computer writing a bank) */
static uint8_t fav_view;
static uint32_t fav_n, fav_xfer, fav_t;         /* how many; xfer_t when built; when KNOB 2 last starred (the reader) */
static vs_fav_t fav_list[VS_FAV_MAX];
static void fav_build(void)                     /* FACTORY's first (set.ffav), then U1..U8 */
{
    uint32_t n = 0, s, k;
    for (s = 0; s < VS_NPROG && n < VS_FAV_MAX; s++) {
        if (!ffav_get(s) || VS_FACTORY[s][VS_NAME] == VS_REC_EMPTY) continue;
        fav_list[n].bank = 0; fav_list[n].slot = (uint8_t)s;
        fav_list[n].color = (uint8_t)(VS_FACTORY[s][VS_REC - 1u] & (VS_REC_FAV - 1u));
        for (k = 0; k < VS_NAME; k++)
            fav_list[n].name[k] = VS_FACTORY[s][k] >= 32u && VS_FACTORY[s][k] <= 126u ? (char)VS_FACTORY[s][k] : ' ';
        n++;
    }
    fav_n = n + vs_prog_favs(&prog, fav_list + n, VS_FAV_MAX - n);
    fav_xfer = xfer_t;
    if (cursor >= fav_n) cursor = fav_n ? fav_n - 1u : 0u;
}
static void fav_open(void)                      /* the list, the cursor on the playing patch if it is in it */
{
    uint32_t i;
    fav_view = 1; cursor = 0; fav_build();
    for (i = 0; i < fav_n; i++) if (fav_list[i].bank == prog.bank && fav_list[i].slot == slot) { cursor = i; break; }
}
static void bank_cursor(int32_t e)              /* PRESETS / SELECT on SEQ: the cursor, and the slot it plays */
{
    if (fav_view) {
        int32_t c;
        if (!fav_n) return;
        c = ((int32_t)cursor + e) % (int32_t)fav_n;
        cursor = (uint32_t)(c < 0 ? c + (int32_t)fav_n : c);
        bank_want = fav_list[cursor].bank; slot_want = fav_list[cursor].slot;   /* (prog_service: the bank first) */
        return;
    }
    cursor = (cursor + 128u + (uint32_t)e) & 127u;
    if (!vs_prog_empty(&prog, cursor)) slot_want = cursor;
}
/* a slot of the bank in RAM: starred? (FACTORY's from the settings, U1..U8's in the patch) */
static int slot_fav(uint32_t sl)
{
    return vs_prog_empty(&prog, sl) ? 0 : prog.bank ? vs_prog_fav(&prog, sl) : ffav_get(sl);
}
static void fav_star(int on)                    /* KNOB 2 on SEQ */
{
    uint32_t s = cursor, b = prog.bank;
    if (fav_view) {                             /* (only unstarring here; a user entry's bank is in once it plays) */
        if (on || cursor >= fav_n) return;
        b = fav_list[cursor].bank; s = fav_list[cursor].slot;
        if (b && b != prog.bank) return;
    }
    if (!b) { if (VS_FACTORY[s][VS_NAME] == VS_REC_EMPTY || !ffav_set(s, on)) return; }   /* FACTORY: the settings */
    else if (vs_prog_set_fav(&prog, s, on) <= 0) return;   /* (empty, or already so) */
    if (b == prog.bank && s == slot) eng.patch.fav = (uint8_t)(on != 0);   /* (SAVE keeps it) */
    fav_t = fm1_ms ? fm1_ms : 1u;
    if (fav_view) fav_build();
}
/* SAVE names the patch (issue #4): KNOB 3 moves the cursor, KNOB 4 (or SELECT) the character under it */
static char save_name[VS_NAME];
static uint32_t save_pos;
static const char NAME_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz-_.+#&/()!'";
#define NAME_NCH (sizeof NAME_CHARS - 1u)
static void save_begin(void)                    /* the name as it is, the cursor after its last character */
{
    uint32_t i, n = 0;
    for (i = 0; i < VS_NAME; i++) { save_name[i] = prog_name[i] && n == i ? prog_name[i] : ' '; if (prog_name[i] && n == i) n++; }
    save_pos = n < VS_NAME ? n : VS_NAME - 1u;
}
static void save_char(int32_t e)                /* the character under the cursor, a step through NAME_CHARS */
{
    uint32_t i = 0;
    int32_t k;
    while (i < NAME_NCH && NAME_CHARS[i] != save_name[save_pos]) i++;
    if (i == NAME_NCH) i = 0;                   /* (one not in the list: from the space) */
    k = ((int32_t)i + e) % (int32_t)NAME_NCH;
    save_name[save_pos] = NAME_CHARS[k < 0 ? k + (int32_t)NAME_NCH : k];
}
static uint32_t big_id = 0xFFu;                 /* the parameter last edited: the knob in hand (vs_ui.c ui_touch) */

static const uint8_t SEC_BTN[7] = {BTN_LFO, BTN_EDIT, BTN_ENV, BTN_SEL, BTN_FX, BTN_GLO, BTN_ARP};   /* VS_BTN_* order */

/* POLY reads and edits as 1..8 (VS_NV, the voices built in): a patch's 9..32 is kept as it came (Synth2's own) until
 * edited, and plays 8 either way (vs_voice.c voice_limit) */
static int32_t pmax(uint32_t id) { return id == VS_P_POLY ? (int32_t)VS_NV : VS_PARAMS[id].max; }
static int32_t pval(uint32_t id) { const int32_t v = eng.patch.v[id], m = pmax(id); return v > m ? m : v; }
static void ptext(uint32_t id, char *s, uint32_t cap)
{
    const uint32_t lfo = id == VS_P_LFO1_SPEED ? VS_P_LFO1_TEMPO : id == VS_P_LFO2_SPEED ? VS_P_LFO2_TEMPO : 0u;
    vs_param_text(id, pval(id), lfo ? pval(lfo) != 0 : 0, pval(VS_P_FX_TYPE), s, cap);
}
static uint32_t pc_voice, pc_bus;                /* the split: voices, bus, % of the buffer's time */
static uint32_t pc_cpu1, pc_wait;                /* per core: CPU1's voice render, CPU0's wait at the join (% of the buffer) */

/* HOME KNOBS (GLO, Kris 18 Nov): what HOME's KNOB 1..4 turn, kept with the patch (eng.patch.home, vs_patch.h). A
 * knob there steps through the parameters a HOME knob may take, in page order (LFO 1 .. ARP, as ALGORITHM walks
 * them), then any on no page (ARP's on/off, on its button) */
static uint8_t home_list[VS_NALL], home_n;
static void home_list_build(void)
{
    uint8_t seen[VS_NALL];
    uint32_t k, c;
    for (k = 0; k < VS_NALL; k++) seen[k] = 0;
    home_n = 0;
    for (k = 0; k < VS_NPAGE; k++)
        for (c = 0; c < 4u; c++) {
            const uint32_t id = VS_PAGES[k].id[c];
            if (id < VS_NALL && !seen[id] && vs_home_ok(id)) { seen[id] = 1; home_list[home_n++] = (uint8_t)id; }
        }
    for (k = 0; k < VS_NALL; k++) if (!seen[k] && vs_home_ok(k)) home_list[home_n++] = (uint8_t)k;
}
static uint32_t home_pos(uint32_t id) { uint32_t i; for (i = 0; i < home_n; i++) if (home_list[i] == id) return i; return 0; }
static uint32_t home_page(uint32_t id)          /* the page a parameter is on (VS_NPAGE: none, ARP's on/off) */
{
    uint32_t k, c;
    for (k = 0; k < VS_NPAGE; k++) for (c = 0; c < 4u; c++) if (VS_PAGES[k].id[c] == id) return k;
    return VS_NPAGE;
}
#include "vs_ui.c"                             /* the screens: knobs, graphs, themes */
#include "vs_speak.c"                          /* the screen reader: the screen as words (fm1-reader, issue #1) */

/* ---- MIDI */
static void midi_in(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2) != 0) {
        const uint32_t ty = st & 0xF0u, ch = (st & 0x0Fu) + 1u;
        if (st == 0xF8u) {                      /* clock: the tempo from a beat's 24 ticks */
            if (!clk_t || fm1_ms - clk_t > 500u) { clk_n = 0; clk_t0 = fm1_ms; }
            clk_t = fm1_ms ? fm1_ms : 1u;
            if (++clk_n == 24u) {
                const uint32_t ms = fm1_ms - clk_t0;
                if (ms >= 200u && ms <= 3000u) eng.bpm_q8 = 60000u * 256u / ms;
                clk_n = 0; clk_t0 = fm1_ms;
            }
            continue;
        }
        if (st >= 0xF0u || (set.ch && ch != set.ch)) continue;
        if (ty == 0x90u && d2) vs_engine_note_on(&eng, d1, d2);
        else if (ty == 0x80u || ty == 0x90u) vs_engine_note_off(&eng, d1);
        else if (ty == 0xE0u) eng.bend14 = (int32_t)((uint32_t)d1 | (uint32_t)d2 << 7) - 8192;
        else if (ty == 0xC0u) { if (d1 < VS_NPROG && !vs_prog_empty(&prog, d1)) slot_want = d1; }
        else if (ty == 0xD0u) eng.pressure = d1;        /* the CTRL routes' sources */
        else if (ty == 0xB0u) {
            eng.cc[d1 & 127u] = d2;
            if (d1 == 0u) { if (d2 <= VS_NBANK) bank_want = d2; }    /* bank select */
            else if (d1 == 64u) vs_engine_sustain(&eng, d2 >= 64u);
            else if (d1 == 120u || d1 == 123u) vs_engine_all_off(&eng);
            else if (vs_cc_apply(&eng.patch, d1, d2) >= 0) edited = 1;   /* the panel's CC map */
        }
    }
}
static void app_idle(void)
{
    if (idle_ok) midi_in();
}

/* ---- the panel */
static void open_page(uint32_t p) { page = p; scr = SCR_PAGE; col = 0; need_full = 1; big_id = 0xFFu; }
static void section(uint32_t btn)               /* a section button: open its page, or step through them */
{
    uint32_t k, first = VS_NPAGE, i;
    for (k = 0; k < VS_NPAGE; k++) if (VS_PAGES[k].btn == btn) { first = k; break; }
    if (first == VS_NPAGE) return;
    if (scr == SCR_PAGE && VS_PAGES[page].btn == btn) {
        i = page + 1u;
        if (i >= VS_NPAGE || VS_PAGES[i].btn != btn) {
            if (btn == VS_BTN_GLO) { scr = SCR_GLOBAL; gpage = GP_KNOBS; col = 0; need_full = 1; return; }
            i = first;
        }
        open_page(i);
    } else if (scr == SCR_GLOBAL && btn == VS_BTN_GLO) {
        if (gpage + 1u < GP_N) { gpage++; col = 0; need_full = 1; return; }   /* HOME KNOBS, GLOBAL 1/4 .. 4/4 */
        open_page(first);
    }
    else open_page(VS_PAGES[last_page[btn]].btn == btn ? last_page[btn] : first);
    last_page[btn] = (uint8_t)page;
}
/* The CTRL routes step over a discrete list, not raw integers (else a turn past CC 0 lands on 0xAFFF = 45055, #8).
 * SRC: NONE, CC 0..127 (0xB0nn), BEND (0xE000), PRESS (0xD000). DST: NONE, then every routable parameter (the ones
 * routes_apply honours: range 0..127), in index order. A value that is neither (old data from the bug) counts as
 * NONE, so the next turn recovers onto the list. */
#define SRC_BEND 0xE000
#define SRC_PRESS 0xD000
#define SRC_N (1 + 128 + 2)                     /* NONE + CC0..127 + BEND + PRESS */
static int32_t src_ord(int32_t v)               /* value -> list position 0..SRC_N-1 */
{
    if (v < 0) return 0;
    if ((v & 0xF000) == 0xB000 && (v & 0xFF) < 128) return 1 + (v & 0x7F);
    if ((v & 0xF000) == SRC_BEND) return 1 + 128;
    if ((v & 0xF000) == SRC_PRESS) return 1 + 128 + 1;
    return 0;                                    /* unrecognised (e.g. 0xAFFF): treat as NONE */
}
static int32_t src_val(int32_t ord)             /* list position -> value */
{
    if (ord <= 0) return -1;
    if (ord <= 128) return 0xB000 + (ord - 1);   /* CC 0..127 */
    return ord == 129 ? SRC_BEND : SRC_PRESS;
}
static int is_route_dst(uint32_t id) { return id < VS_NPARAM && VS_PARAMS[id].min == 0 && VS_PARAMS[id].max == 127; }
static int32_t dst_step(int32_t cur, int32_t e)
{
    uint32_t i;
    int32_t n = 0, ord = 0;                      /* ord 0 = NONE; 1..n = routable params in index order */
    for (i = 0; i < VS_NPARAM; i++) if (is_route_dst(i)) { n++; if ((int32_t)i <= cur) ord = n; }
    ord += e;
    if (ord < 0) ord = 0;
    if (ord > n) ord = n;
    if (ord == 0) return -1;
    for (i = 0; i < VS_NPARAM; i++) if (is_route_dst(i) && --ord == 0) return (int32_t)i;
    return -1;
}
static void edit(uint32_t id, int32_t e)
{
    const vs_param_info_t *pi = &VS_PARAMS[id];
    if (pi->kind == VS_K_SRC) {
        int32_t o = src_ord(pval(id)) + e;
        vs_param_set(&eng.patch, id, src_val(o < 0 ? 0 : o >= SRC_N ? SRC_N - 1 : o));
    } else if (pi->kind == VS_K_DST) {
        vs_param_set(&eng.patch, id, dst_step(pval(id), e));
    } else {
        int32_t step = e;
        if (pi->max - pi->min >= 100 && (e > 1 || e < -1)) step = e * 2;   /* a fast turn on a wide range */
        const int32_t v = pval(id) + step, m = pmax(id);
        vs_param_set(&eng.patch, id, v > m ? m : v);
    }
    ui_touch(id);
    edited = 1;
}
static void global_edit(uint32_t k, int32_t e)
{
    static uint32_t t_k[4];                     /* when knob k last moved: M-vance's fast turn (mv_ui.c) */
    int32_t v;
    const int fast = k < 4u && fm1_ms - t_k[k] < 40u;   /* detents < 40 ms apart: x4 on the long scrolls only */
    if (k < 4u) t_k[k] = fm1_ms;
    if (gpage == GP_DIAG) return;               /* GLOBAL 4/4: DIAG, read only */
    if (fast && (gpage == GP_KNOBS || (gpage == GP_MAIN && k == 2u))) e *= 4;   /* HOME KNOBS' list, TEMPO (not MIDI CH, VOICES, choices) */
    if (gpage == GP_KNOBS) {                    /* HOME KNOBS: the patch's, so it is edited (SAVE keeps it) */
        if (k >= VS_HOME_N || !home_n) return;
        v = (int32_t)home_pos(eng.patch.home[k]) + e;
        eng.patch.home[k] = home_list[v < 0 ? 0 : v >= (int32_t)home_n ? (int32_t)home_n - 1 : v];
        edited = 1;
        ui_gtouch();
        return;
    }
    if (gpage == GP_KEYS) {                     /* GLOBAL 3/4 KEYS: TOUCH, VEL CURVE, PRESS SENS (LAST VEL: read only) */
        if (k == 0u) { if (set.touch == (e > 0)) return; set.touch = e > 0; fk_touch_enable(set.touch); }   /* (the keys
                                                   change hands: the held ones end, as any key let go) */
        else if (k == 1u) { v = set.vcurve + e; set.vcurve = (int8_t)(v < -5 ? -5 : v > 5 ? 5 : v); }
        else if (k == 2u) { v = set.psens + e; set.psens = (int8_t)(v < -5 ? -5 : v > 5 ? 5 : v); }
        else return;
        fk_touch_set(set.vcurve, set.psens);
        set_changed();
        ui_gtouch();
        return;
    }
    if (gpage == GP_LOOK) {                     /* GLOBAL 2/4: THEME, COLOUR (vs_ui.c applies them), BLUETOOTH */
        if (k == 0u) { v = set.theme + e; set.theme = (uint8_t)(v < 0 ? 0 : v >= (int32_t)UI_NTHEME ? (int32_t)UI_NTHEME - 1 : v); }
        else if (k == 1u) set.colour = e > 0;
        else if (k == 2u) { if (set.bt == (e > 0)) return; set.bt = e > 0; fk_ble_enable(set.bt); }   /* (the first ON
                                                   holds the main loop ~0.5 s while the radio calibrates) */
        else return;
        set_changed();
        ui_gtouch();
        return;
    }
    if (k == 0u) { v = set.ch + e; set.ch = (uint8_t)(v < 0 ? 0 : v > 16 ? 16 : v); }
    else if (k == 1u) { v = set.voices + e; set.voices = (uint8_t)(v < 1 ? 1 : v > (int32_t)VS_NV ? (int32_t)VS_NV : v); eng.nv = set.voices; }
    else if (k == 2u) { v = set.tempo + e; set.tempo = (uint8_t)(v < 40 ? 40 : v > 240 ? 240 : v); }
    else return;
    set_changed();
    ui_gtouch();
}

/* a note key's velocity as it goes down: the FM-1's from GLOBAL KEYS (fm1-kit fk_touch: 100 while TOUCH is OFF); the
 * SMK-37's from its own keybed (smk_input.h: the factory curves), as before fk_touch (issue #10) */
#if BOARD_SMK37
#define KEY_VEL(k) (fm1_in.key_vel[SMK_KEY_F3 + (k)] ? (uint32_t)fm1_in.key_vel[SMK_KEY_F3 + (k)] : 100u)
#else
#define KEY_VEL(k) fk_touch_vel(k)
#endif

static void app_main(void)
{
    uint32_t panic_t = 0, prev = 0, k, cpu_t = 0, pk_slot[12] = {0}, pk_i = 0, avg_us = 0, peak_pc = 0, arp_t = 0, quiet_t = 0;
    uint8_t held[27] = {0};
    int32_t e;
    vs_engine_init(&eng);
    vs_xfer_init(&xfer);
    if (settings_load(&set, sizeof set) == 0) {}
    else {
        uint32_t i;
        if (settings_load(&set, __builtin_offsetof(__typeof__(set), ffav)) == 0) {}   /* (before FACTORY's stars) */
        else if (settings_load(&set, __builtin_offsetof(__typeof__(set), touch)) == 0) set.touch = 0xFFu;   /* (before KEYS) */
        else if (settings_load(&set, __builtin_offsetof(__typeof__(set), bt)) == 0) { set.bt = 1; set.touch = 0xFFu; }   /* (before BLUETOOTH: ON) */
        else set.bt = 0xFFu;                    /* (none: the defaults below) */
        for (i = 0; i < sizeof set.ffav; i++) set.ffav[i] = 0;
    }
    if (set.bt > 1u || set.ch > 16u || !set.voices || set.voices > VS_NV || set.tempo < 40u ||
        set.tempo > 240u || set.bank > VS_NBANK || set.oct < -2 || set.oct > 2 || set.theme >= UI_NTHEME || set.colour > 1u) {
        set.ch = 0; set.voices = VS_NV; set.tempo = 120; set.bank = 0; set.slot = 0; set.oct = 0; set.theme = 0; set.colour = 0;
        set.bt = 1;
    }
    if (set.touch > 1u || set.vcurve < -5 || set.vcurve > 5 || set.psens < -5 || set.psens > 5) {
        set.touch = 0; set.vcurve = 0; set.psens = 0;   /* KEYS: TOUCH OFF, as fm1-base tuned it */
    }
    fk_ble_enable(set.bt);                      /* ON: the radio calibrates (~0.5 s; the first second silent), advertises */
    fk_touch_set(set.vcurve, set.psens);
    fk_touch_enable(set.touch);
    eng.nv = set.voices;
    home_list_build();
    vs_prog_load(&prog, set.bank);
    slot = 0xFFFFu; slot_want = vs_prog_empty(&prog, set.slot) ? vs_prog_next(&prog, set.slot, 1) : set.slot;
    for (k = 0; k < 7u; k++) last_page[k] = 0;
#ifdef DINGHY_HOST
    if (getenv("MVS2_SPEAKDUMP")) { speak_dump(getenv("MVS2_SPEAKDUMP")); exit(0); }   /* tests/run_speak.sh */
#endif

    for (;;) {
        uint32_t keys, down, up, rel, b, base = (uint32_t)(53 + 12 * set.oct), silent;
        platform_poll();

        fk_touch_poll();                        /* GLOBAL KEYS TOUCH ON: the keys' velocity and pressure (fm1-kit fk_touch) */
        keys = fk_touch_keys();                 /* bit k: key k (0 = F3 .. 26 = G5); TOUCH OFF: fm1_in.notes */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) {               /* (TOUCH OFF: 100, as ever; the SMK-37: its keybed's own) */
                const uint32_t vel = KEY_VEL(k);
                held[k] = (uint8_t)(base + k); vs_engine_note_on(&eng, held[k], vel); midi_send(0x90u, held[k], vel);
            }
            if (up >> k & 1u) { vs_engine_note_off(&eng, held[k]); midi_send(0x80u, held[k], 0); }
        }
#if BOARD_SMK37
        {   /* the SMK-37's outer keys, C3..E3 and G#5..C6 (issue #10): not in the 27 bits, events of their own
               (smk_input.h smk_extkey_take: keybed key << 8 | velocity, 0 = let go). Each ends the note it began,
               whatever the octave is by then */
            static uint8_t xheld[FM1_NKEY];
            int32_t x;
            while ((x = smk_extkey_take()) >= 0) {
                const uint32_t kk = (uint32_t)x >> 8 & 0xFFu, vel = (uint32_t)x & 0x7Fu;
                const int32_t n = (int32_t)base + (int32_t)kk - (int32_t)SMK_KEY_F3;
                if (kk >= FM1_NKEY) continue;
                if (vel && n >= 0 && n <= 127) {
                    if (xheld[kk]) { vs_engine_note_off(&eng, xheld[kk]); midi_send(0x80u, xheld[kk], 0); }
                    xheld[kk] = (uint8_t)n; vs_engine_note_on(&eng, (uint32_t)n, vel); midi_send(0x90u, (uint32_t)n, vel);
                } else if (!vel && xheld[kk]) {
                    vs_engine_note_off(&eng, xheld[kk]); midi_send(0x80u, xheld[kk], 0); xheld[kk] = 0;
                }
            }
        }
#endif
        {   /* the keys' pressure (F3..C5, the hardest-pressed one) as channel pressure: the CTRL routes' PRESS, and out */
            static uint8_t press_out;
            const uint32_t pr = fk_touch_channel_pressure();   /* (TOUCH OFF: 0) */
            if (pr != press_out) { press_out = (uint8_t)pr; eng.pressure = (uint8_t)pr; midi_send(0xD0u, pr, 0); }
        }
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; set_changed(); }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; set_changed(); }
        if ((fm1_in.buttons & (1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) == (1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) {
            if (!panic_t) panic_t = fm1_ms ? fm1_ms : 1u;   /* both held 1 s: all notes off */
            else if (fm1_ms - panic_t >= 1000u && panic_t != 1u) { vs_engine_all_off(&eng); panic_t = 1u; speak_panic(); }
        } else panic_t = 0;
        if (scr == SCR_SAVE && b & ~(1u << BTN_SAVE | 1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) { scr = SCR_HOME; need_full = 1; b &= ~(1u << BTN_HOME); }
        for (k = 0; k < 6u; k++) if (b >> SEC_BTN[k] & 1u) section(k);
        if (b >> BTN_ARP & 1u) arp_t = fm1_ms ? fm1_ms : 1u;   /* tap or hold: decided on release, or at 400 ms */
        if (arp_t && fm1_ms - arp_t >= 400u) { arp_t = 0; section(VS_BTN_ARP); }
        if (arp_t && rel >> BTN_ARP & 1u) { arp_t = 0; vs_param_set(&eng.patch, VS_P_ARP_ON, !eng.patch.v[VS_P_ARP_ON]); edited = 1; ui_touch(VS_P_ARP_ON); }
        if (b >> BTN_HOME & 1u) { scr = SCR_HOME; need_full = 1; }
        if (b >> BTN_SEQ & 1u) { scr = SCR_BANK; need_full = 1; if (fav_view) fav_open(); else cursor = slot & 127u; }
        if (b >> BTN_SAVE & 1u) {
            if (scr != SCR_SAVE) { scr = SCR_SAVE; save_bank = prog.bank ? prog.bank : 1u; save_slot = slot & 127u; save_begin(); need_full = 1; }
            else {                              /* write: the bank comes in if it is not the one shown */
                vs_patch_t p = eng.patch;
                uint32_t i;
                for (i = 0; i < VS_NAME; i++) p.name[i] = save_name[i];   /* the name as edited */
                if (save_bank != prog.bank) {
                    if (prog.dirty && vs_prog_flush(&prog)) save_bank = prog.bank;   /* (failed: stays) */
                    else { vs_prog_load(&prog, save_bank); set.bank = prog.bank; }
                }
                const int ok = !vs_prog_write(&prog, save_slot, &p);
                if (ok) {
                    slot = slot_want = save_slot; edited = 0; set.slot = (uint8_t)slot; set_changed();
                    for (i = 0; i < VS_NAME; i++) eng.patch.name[i] = prog_name[i] = save_name[i];
                    prog_name[VS_NAME] = 0;
                    for (i = VS_NAME; i > 0u && prog_name[i - 1u] == ' '; i--) prog_name[i - 1u] = 0;
                }
                scr = SCR_HOME; need_full = 1;
                speak_saved(ok);                /* (after the screen is HOME: it is the answer, not cut off by HOME) */
            }
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            if (scr == SCR_BANK) bank_cursor(e);
            else {
                while (e > 0) { slot_want = vs_prog_next(&prog, slot_want, 1); e--; }
                while (e < 0) { slot_want = vs_prog_next(&prog, slot_want, -1); e++; }
            }
        }
        if ((e = fm1_enc_take(ENC_ALGORITHM)) != 0) {
            int32_t p = scr == SCR_PAGE ? (int32_t)page : e > 0 ? -1 : (int32_t)VS_NPAGE;
            p += e;
            p = p < 0 ? (int32_t)VS_NPAGE - 1 : p >= (int32_t)VS_NPAGE ? 0 : p;
            open_page((uint32_t)p);
            last_page[VS_PAGES[page].btn] = (uint8_t)page;
        }
        for (k = 0; k < 4u; k++)
            if ((e = fm1_enc_take(ENC_KNOB1 + k)) != 0) {
                if (scr == SCR_PAGE && VS_PAGES[page].id[k] != 0xFFu) { edit(VS_PAGES[page].id[k], e); col = k; }
                else if (scr == SCR_HOME) edit(eng.patch.home[k], e);   /* (the patch's HOME KNOBS) */
                else if (scr == SCR_BANK && k == 0u) {   /* FACTORY, U1..U8, then FAVOURITES */
                    int32_t nb = (fav_view ? (int32_t)VS_NBANK + 1 : (int32_t)prog.bank) + e;
                    nb = nb < 0 ? 0 : nb > (int32_t)VS_NBANK + 1 ? (int32_t)VS_NBANK + 1 : nb;
                    if (nb == (int32_t)VS_NBANK + 1) { if (!fav_view) fav_open(); }
                    else { if (fav_view) { fav_view = 0; cursor = slot & 127u; } bank_want = nb; }
                    need_full = 1;
                }
                else if (scr == SCR_BANK && k == 1u) fav_star(e > 0);
                else if (scr == SCR_SAVE && k == 0u) { int32_t nb = (int32_t)save_bank + e; save_bank = (uint32_t)(nb < 1 ? 1 : nb > (int32_t)VS_NBANK ? (int32_t)VS_NBANK : nb); }
                else if (scr == SCR_SAVE && k == 1u) save_slot = (save_slot + 128u + (uint32_t)e) & 127u;
                else if (scr == SCR_SAVE && k == 2u) { int32_t q = (int32_t)save_pos + e; save_pos = (uint32_t)(q < 0 ? 0 : q > (int32_t)VS_NAME - 1 ? (int32_t)VS_NAME - 1 : q); }
                else if (scr == SCR_SAVE && k == 3u) save_char(e);
                else if (scr == SCR_GLOBAL) { global_edit(k, e); col = k; }
            }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            if (scr == SCR_PAGE && VS_PAGES[page].id[col] != 0xFFu) { const uint32_t id = VS_PAGES[page].id[col], kd = VS_PARAMS[id].kind;
                if (kd == VS_K_SRC || kd == VS_K_DST) edit(id, e);   /* the routes step over their list (not raw: #8) */
                else { const uint32_t m = (uint32_t)pmax(id); int32_t v = pval(id) + e;   /* (fine: no fast turn) */
                    vs_param_set(&eng.patch, id, v > (int32_t)m ? (int32_t)m : v); ui_touch(id); edited = 1; } }
            else if (scr == SCR_BANK) bank_cursor(e);
            else if (scr == SCR_SAVE) save_char(e);
            else if (scr == SCR_GLOBAL) global_edit(col, e);
        }
        midi_in();
        prog_service();
        if (scr == SCR_BANK && fav_view && fav_xfer != xfer_t) fav_build();   /* (the computer wrote a bank) */
        if (!(clk_t && fm1_ms - clk_t < 1000u)) eng.bpm_q8 = (uint32_t)set.tempo << 8;   /* no clock: GLOBAL's tempo */
        {   /* the transfer's answers, one a pass */
            static uint8_t msg[VS_XF_MSG_MAX];
            uint32_t n = vs_xfer_next(&xfer, &prog, msg, sizeof msg);
            if (n && sysex_send(msg, n)) xfer.rd_n = 0;   /* the computer stopped reading */
            if (!n) {                           /* nothing else to send: the link's report (the simulator's Link FM-1) */
                n = vs_xfer_link(&xfer, &eng.patch, xfer.play_due || eng.load_req || slot_want != slot || want_reload,
                                 fm1_ms, msg, sizeof msg);
                if (n && sysex_send(msg, n)) xfer.link_t = 0;   /* the computer stopped reading: unlinked */
            }
            if (n) xfer_t = fm1_ms ? fm1_ms : 1u;
        }
        {   /* leaving GLOBAL: its settings save at the next silence, the 2 s rest waived (set_changed) */
            static uint32_t scr_was;
            if (scr_was == SCR_GLOBAL && scr != SCR_GLOBAL) set_chg = fm1_ms - 2000u;
            scr_was = scr;
        }
        /* flash, only when nothing has been heard for 0.5 s (an erase holds the interrupts off): no voice, no arp
           waiting for its next step, and no tail at the output (issue #11: out_heard) */
        silent = eng.act == 0u && eng.arp_n == 0u && !out_heard;
        out_heard = 0;
        if (!silent) quiet_t = fm1_ms;
        flash_ok_now = silent && fm1_ms - quiet_t >= 500u;
        if (flash_ok_now) {
            if (prog.dirty) (void)vs_prog_flush(&prog);
            if (set_dirty && scr != SCR_GLOBAL && fm1_ms - set_chg >= 2000u) { set_dirty = 0; (void)settings_save(&set, sizeof set); }
        }

        if (fm1_ms - cpu_t >= 250u && !cpu_clear) {   /* the meter, 4 times a second; the peak over 12 of them: 3 s */
            uint32_t n = cpu_n, sum = cpu_sum, pk = 0;
            pk_slot[pk_i++ % 12u] = cpu_peak;
            cpu_clear = 1;
            cpu_t = fm1_ms;
            for (k = 0; k < 12u; k++) if (pk_slot[k] > pk) pk = pk_slot[k];
            avg_us = n ? sum / n / FM1_TICKS_PER_US : 0u;
            {   /* the split, over the same 250 ms: ticks / (n buffers x the buffer's ticks) */
                static uint32_t tv0, tb0, jt0, wt0;
                const uint32_t tv = eng.t_voice, tb = eng.t_bus, span = n * BUF_US * FM1_TICKS_PER_US / 100u;
                const uint32_t jt = vs_core1_job_ticks(), wt = vs_core1_wait_ticks();   /* CPU1 render, CPU0 wait */
                pc_voice = span ? (tv - tv0) / span : 0u; pc_bus = span ? (tb - tb0) / span : 0u;
                pc_cpu1 = span ? (jt - jt0) / span : 0u; pc_wait = span ? (wt - wt0) / span : 0u;
                tv0 = tv; tb0 = tb; jt0 = jt; wt0 = wt;
            }
            peak_pc = pk / FM1_TICKS_PER_US * 100u / BUF_US;
        }

        /* the LEDs: the section on screen, ARP on, SAVE when there is something to save */
        for (k = 0; k < 6u; k++) fm1_led_key(SEC_BTN[k], scr == SCR_PAGE ? VS_PAGES[page].btn == k : scr == SCR_GLOBAL && k == VS_BTN_GLO);
        fm1_led_key(BTN_ARP, eng.patch.v[VS_P_ARP_ON] != 0);
        fm1_led_key(BTN_SAVE, edited || prog.dirty || scr == SCR_SAVE);
        fm1_led_key(BTN_HOME, scr == SCR_HOME);
        fm1_led_key(BTN_SEQ, scr == SCR_BANK);
        fm1_led_key(BTN_OCT_DOWN, set.oct < 0);
        fm1_led_key(BTN_OCT_UP, set.oct > 0);
        {   /* the keys: lit while their note plays (pressed, MIDI, the arp), glowing while it rings on in its release */
            uint32_t on[4], rl[4], n;
            vs_engine_lit(&eng, on, rl);
            for (k = 0; k < 27u; k++) {
                n = base + k;
                fm1_led_key(KEY_LED(k), (keys >> k & 1u) || (on[n >> 5] >> (n & 31u) & 1u));
                fm1_led_key_glow(KEY_LED(k), rl[n >> 5] >> (n & 31u) & 1u);
            }
        }

        idle_ok = 1;                            /* the drawing below may read MIDI meanwhile (LCD_IDLE) */
        if (scr != scr_drawn || (scr == SCR_PAGE && page != page_drawn)) { need_full = 1; scr_drawn = scr; page_drawn = page; }
        ui_draw(avg_us, peak_pc);
        need_full = 0;
        speak();                                /* the screen reader: what changed (nothing while none listens) */
        ann_tick(fm1_ms);
        idle_ok = 0;
    }
}
