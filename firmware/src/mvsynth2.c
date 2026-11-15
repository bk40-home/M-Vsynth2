/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2: Synth2 (the open-source recreation of Synth1) for the M-VAVE FM-1, built on Dinghy.
 * The engine (vs_voice.h, vs_bus.h) plays Synth2's patches as Synth2 does (tests/run_compare.sh); this file is the
 * panel, the banks (vs_prog.h), the transfer (vs_xfer.h) and MIDI (proposal sections 7, 6, 8).
 *
 * The panel (proposal 7.1, 7.2; the pages: firmware/gen/vs_params.h VS_PAGES, all 99 parameters):
 *   LFO EDIT ENV SEL FX GLO   a section: the first press opens its page (the one last seen), the next steps
 *                             through its pages (GLO ends on GLOBAL: MIDI CH, VOICES, TEMPO)
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
 * MIDI in (USB or the TRS jack): notes (GLOBAL's channel, or any), velocity, the bend, CC 64, CC 120 / 123, program
 * change (a slot of the bank shown; an empty one is ignored, as in Synth2), bank select (CC 0: 0 FACTORY, 1..8),
 * channel pressure and every CC for the CTRL routes, the panel's CC map (proposal 8), clock (F8: the tempo while it
 * runs; else GLOBAL's TEMPO). Out: the keys' notes on channel 1. SysEx 7D: the transfer (vs_xfer.h).
 *
 * Rules of the road (the chip has no FPU and no libgcc): integers only, no float, no 64-bit division.
 * tests/host.c runs this file on a computer: the screen as a PNG, the sound as a WAV.
 * What platform.c offers: docs/API.md. The chip's rules: docs/RULES.md. */
#define APP_NAME "M-VSYNTH2"                    /* the rescue and crash screens (platform.c) */
#define STORE_MAGIC 0x3253564Du                /* "MVS2": our settings never read another fork's (store.h) */
#define APP_SYSEX_ID 0x7Du                      /* our SysEx's first byte (the transfer, vs_xfer.h; platform.c) */
#include <stdint.h>
static void app_idle(void);
static void app_sysex(const uint8_t *p, uint32_t n);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in (platform.c) */
#define APP_SYSEX app_sysex
#include "platform.c"
#include "vs_voice.h"                          /* the engine; its code is vs_dsp.c, built apart at -O2 */
#include "vs_prog.c"                           /* the banks (main loop) */
#include "vs_xfer.c"                           /* the transfer (main loop) */

/* ---- the sound: the ISR renders and times itself; the main loop reads the sums and asks for their clearing */
static vs_engine_t eng;
static volatile uint32_t cpu_sum, cpu_n, cpu_peak, cpu_clear;
#define UI_SCOPE_N 220u
static volatile int16_t scope[UI_SCOPE_N];      /* HOME's scope: the ISR fills it when asked (scope_req), vs_ui.c draws it */
static volatile uint32_t scope_req, scope_seq, scope_pk;   /* scope_pk: the last capture's peak (the tail still heard?) */

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t t0 = fm1_ticks(), dt;
    vs_engine_render(&eng, out, n);
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
} set;
static uint8_t set_dirty;
static uint32_t set_t;

/* ---- programs */
static vs_prog_t prog;
static vs_xfer_t xfer;
static uint32_t slot, slot_want, xfer_t;
static int32_t bank_want = -1;
static uint8_t edited, want_reload;
static char prog_name[VS_NAME + 1u];

static void prog_service(void)                  /* main loop: bring the slot asked for in, once the engine takes it */
{
    vs_patch_t p;
    uint32_t i;
    if (bank_want >= 0 && (uint32_t)bank_want != prog.bank) {   /* a bank change: the old one to flash first */
        if (prog.dirty && (eng.act || vs_prog_flush(&prog))) return;   /* (busy: the next loop) */
        vs_prog_load(&prog, (uint32_t)bank_want);
        set.bank = prog.bank; set_dirty = 1;
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
    if (vs_engine_load(&eng, &p)) return;       /* one is still landing: the next loop */
    slot = slot_want; want_reload = 0; edited = 0;
    set.slot = (uint8_t)slot; set_dirty = 1;
    for (i = 0; i < VS_NAME; i++) prog_name[i] = p.name[i];
    prog_name[VS_NAME] = 0;
    for (i = VS_NAME; i > 0u && prog_name[i - 1u] == ' '; i--) prog_name[i - 1u] = 0;   /* (centred: no trailing spaces) */
}

static void app_sysex(const uint8_t *p, uint32_t n)   /* platform_poll: main loop */
{
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
#include "vs_ui.c"                             /* the screens: knobs, graphs, themes */

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
            if (btn == VS_BTN_GLO) { scr = SCR_GLOBAL; gpage = 0; col = 0; need_full = 1; return; }
            i = first;
        }
        open_page(i);
    } else if (scr == SCR_GLOBAL && btn == VS_BTN_GLO) {
        if (gpage < 2u) { gpage++; col = 0; need_full = 1; return; }   /* GLOBAL 1/3, 2/3 (THEME, COLOUR), 3/3 (DIAG) */
        open_page(first);
    }
    else open_page(VS_PAGES[last_page[btn]].btn == btn ? last_page[btn] : first);
    last_page[btn] = (uint8_t)page;
}
static void edit(uint32_t id, int32_t e)
{
    const vs_param_info_t *pi = &VS_PARAMS[id];
    int32_t step = e;
    if (pi->max - pi->min >= 100 && (e > 1 || e < -1)) step = e * 2;   /* a fast turn on a wide range */
    {
        const int32_t v = pval(id) + step, m = pmax(id);
        vs_param_set(&eng.patch, id, v > m ? m : v);
    }
    ui_touch(id);
    edited = 1;
}
static void global_edit(uint32_t k, int32_t e)
{
    int32_t v;
    if (gpage == 2u) return;                    /* GLOBAL 3/3: DIAG, read only */
    if (gpage) {                                /* GLOBAL 2/3: THEME, COLOUR (vs_ui.c applies them) */
        if (k == 0u) { v = set.theme + e; set.theme = (uint8_t)(v < 0 ? 0 : v >= (int32_t)UI_NTHEME ? (int32_t)UI_NTHEME - 1 : v); }
        else if (k == 1u) set.colour = e > 0;
        else return;
        set_dirty = 1;
        ui_gtouch();
        return;
    }
    if (k == 0u) { v = set.ch + e; set.ch = (uint8_t)(v < 0 ? 0 : v > 16 ? 16 : v); }
    else if (k == 1u) { v = set.voices + e; set.voices = (uint8_t)(v < 1 ? 1 : v > (int32_t)VS_NV ? (int32_t)VS_NV : v); eng.nv = set.voices; }
    else if (k == 2u) { v = set.tempo + e; set.tempo = (uint8_t)(v < 40 ? 40 : v > 240 ? 240 : v); }
    else return;
    set_dirty = 1;
    ui_gtouch();
}

static void app_main(void)
{
    uint32_t panic_t = 0, prev = 0, k, cpu_t = 0, pk_slot[12] = {0}, pk_i = 0, avg_us = 0, peak_pc = 0, arp_t = 0, quiet_t = 0;
    uint8_t held[27] = {0};
    int32_t e;
    vs_engine_init(&eng);
    vs_xfer_init(&xfer);
    if (settings_load(&set, sizeof set) || set.ch > 16u || !set.voices || set.voices > VS_NV || set.tempo < 40u ||
        set.tempo > 240u || set.bank > VS_NBANK || set.oct < -2 || set.oct > 2 || set.theme >= UI_NTHEME || set.colour > 1u) {
        set.ch = 0; set.voices = VS_NV; set.tempo = 120; set.bank = 0; set.slot = 0; set.oct = 0; set.theme = 0; set.colour = 0;
    }
    eng.nv = set.voices;
    vs_prog_load(&prog, set.bank);
    slot = 0xFFFFu; slot_want = vs_prog_empty(&prog, set.slot) ? vs_prog_next(&prog, set.slot, 1) : set.slot;
    for (k = 0; k < 7u; k++) last_page[k] = 0;

    for (;;) {
        uint32_t keys, down, up, rel, b, base = (uint32_t)(53 + 12 * set.oct), silent;
        platform_poll();

        keys = fm1_in.notes;                    /* bit k: key k (0 = F3 .. 26 = G5) */
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); vs_engine_note_on(&eng, held[k], 100); midi_send(0x90u, held[k], 100); }
            if (up >> k & 1u) { vs_engine_note_off(&eng, held[k]); midi_send(0x80u, held[k], 0); }
        }
        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && set.oct > -2) { set.oct--; set_dirty = 1; }
        if (b & (1u << BTN_OCT_UP) && set.oct < 2) { set.oct++; set_dirty = 1; }
        if ((fm1_in.buttons & (1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) == (1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) {
            if (!panic_t) panic_t = fm1_ms ? fm1_ms : 1u;   /* both held 1 s: all notes off */
            else if (fm1_ms - panic_t >= 1000u && panic_t != 1u) { vs_engine_all_off(&eng); panic_t = 1u; }
        } else panic_t = 0;
        if (scr == SCR_SAVE && b & ~(1u << BTN_SAVE | 1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) { scr = SCR_HOME; need_full = 1; b &= ~(1u << BTN_HOME); }
        for (k = 0; k < 6u; k++) if (b >> SEC_BTN[k] & 1u) section(k);
        if (b >> BTN_ARP & 1u) arp_t = fm1_ms ? fm1_ms : 1u;   /* tap or hold: decided on release, or at 400 ms */
        if (arp_t && fm1_ms - arp_t >= 400u) { arp_t = 0; section(VS_BTN_ARP); }
        if (arp_t && rel >> BTN_ARP & 1u) { arp_t = 0; vs_param_set(&eng.patch, VS_P_ARP_ON, !eng.patch.v[VS_P_ARP_ON]); edited = 1; ui_touch(VS_P_ARP_ON); }
        if (b >> BTN_HOME & 1u) { scr = SCR_HOME; need_full = 1; }
        if (b >> BTN_SEQ & 1u) { scr = SCR_BANK; cursor = slot & 127u; need_full = 1; }
        if (b >> BTN_SAVE & 1u) {
            if (scr != SCR_SAVE) { scr = SCR_SAVE; save_bank = prog.bank ? prog.bank : 1u; save_slot = slot & 127u; need_full = 1; }
            else {                              /* write: the bank comes in if it is not the one shown */
                vs_patch_t p = eng.patch;
                if (save_bank != prog.bank) {
                    if (prog.dirty && vs_prog_flush(&prog)) save_bank = prog.bank;   /* (failed: stays) */
                    else { vs_prog_load(&prog, save_bank); set.bank = prog.bank; }
                }
                if (!vs_prog_write(&prog, save_slot, &p)) { slot = slot_want = save_slot; edited = 0; set.slot = (uint8_t)slot; set_dirty = 1; }
                scr = SCR_HOME; need_full = 1;
            }
        }
        if ((e = fm1_enc_take(ENC_PRESETS)) != 0) {
            if (scr == SCR_BANK) { cursor = (cursor + 128u + (uint32_t)e) & 127u; if (!vs_prog_empty(&prog, cursor)) slot_want = cursor; }
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
                else if (scr == SCR_HOME) { static const uint8_t KN[4] = {VS_P_FLT_FREQ, VS_P_FLT_RES, VS_P_FLT_AMOUNT, VS_P_AMP_GAIN}; edit(KN[k], e); }
                else if (scr == SCR_BANK && k == 0u) { int32_t nb = (int32_t)prog.bank + e; bank_want = nb < 0 ? 0 : nb > (int32_t)VS_NBANK ? (int32_t)VS_NBANK : nb; need_full = 1; }
                else if (scr == SCR_SAVE && k == 0u) { int32_t nb = (int32_t)save_bank + e; save_bank = (uint32_t)(nb < 1 ? 1 : nb > (int32_t)VS_NBANK ? (int32_t)VS_NBANK : nb); }
                else if (scr == SCR_SAVE && k == 1u) save_slot = (save_slot + 128u + (uint32_t)e) & 127u;
                else if (scr == SCR_GLOBAL) { global_edit(k, e); col = k; }
            }
        if ((e = fm1_enc_take(ENC_SELECT)) != 0) {
            if (scr == SCR_PAGE && VS_PAGES[page].id[col] != 0xFFu) { const uint32_t id = VS_PAGES[page].id[col], m = (uint32_t)pmax(id); int32_t v = pval(id) + e;   /* (fine: no fast turn) */
                vs_param_set(&eng.patch, id, v > (int32_t)m ? (int32_t)m : v); ui_touch(id); edited = 1; }
            else if (scr == SCR_BANK) { cursor = (cursor + 128u + (uint32_t)e) & 127u; if (!vs_prog_empty(&prog, cursor)) slot_want = cursor; }
            else if (scr == SCR_GLOBAL) global_edit(col, e);
        }
        midi_in();
        prog_service();
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
        /* flash, only when nothing has sounded for 0.5 s (an erase holds the interrupts off) */
        silent = eng.act == 0u;
        if (!silent) quiet_t = fm1_ms;
        if (silent && fm1_ms - quiet_t >= 500u) {
            if (prog.dirty) (void)vs_prog_flush(&prog);
            if (set_dirty && fm1_ms - set_t >= 2000u) { set_dirty = 0; set_t = fm1_ms; (void)settings_save(&set, sizeof set); }
        }

        if (fm1_ms - cpu_t >= 250u && !cpu_clear) {   /* the meter, 4 times a second; the peak over 12 of them: 3 s */
            uint32_t n = cpu_n, sum = cpu_sum, pk = 0;
            pk_slot[pk_i++ % 12u] = cpu_peak;
            cpu_clear = 1;
            cpu_t = fm1_ms;
            for (k = 0; k < 12u; k++) if (pk_slot[k] > pk) pk = pk_slot[k];
            avg_us = n ? sum / n / FM1_TICKS_PER_US : 0u;
            {   /* the split, over the same 250 ms: ticks / (n buffers x the buffer's ticks) */
                static uint32_t tv0, tb0;
                const uint32_t tv = eng.t_voice, tb = eng.t_bus, span = n * BUF_US * FM1_TICKS_PER_US / 100u;
                pc_voice = span ? (tv - tv0) / span : 0u; pc_bus = span ? (tb - tb0) / span : 0u;
                tv0 = tv; tb0 = tb;
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
        idle_ok = 0;
    }
}
