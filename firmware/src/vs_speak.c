/* SPDX-License-Identifier: GPL-3.0-only */
/* The screen reader (fm1-reader, Charles Vestal; GitHub issue #1): what the screen shows, sent as text to a reader on
 * the computer (its web page speaks it, or hands it to VoiceOver, NVDA, JAWS or TalkBack). The protocol is fm1-base's
 * announce.c (copied in, unchanged: change it there and sync); this file is M-Vsynth2's side of it:
 *   ann_wire_send   the device's USB-MIDI SysEx queue, never waiting (a full queue: the message is dropped)
 *   speak()         once a main-loop pass, after the screen is drawn: what changed on the screen, as words
 *   speak_dump()    (host only) every screen, knob, value and message as the reader gets them, for tools/audit.mjs
 * The words: firmware/words.tsv -> tools/gen_words.py -> docs/words/<id>.json (published with the pages) and
 * firmware/gen/ann_words.h (the link the device sends). The screen keeps its short labels; the words file says them.
 * Rules: fm1-reader's skill/fm1-screen-reader/RULES.md. Nothing is built or sent until a reader says HELLO.
 * Cost: announce.c ~1.5 KB flash, ~250 B RAM; this file's text ~3 KB flash, sp ~500 B RAM; ~500 B stack (main loop
 * only, never the audio interrupt). Included by mvsynth2.c after vs_ui.c. */
#ifndef APP_VER
#define APP_VER "0.0"
#endif
#define ANN_NAME "M-Vsynth2 " APP_VER          /* the HELLO answer: the firmware and its version */
#include "ann_words.h"                          /* ANN_WORDS_URL: firmware/gen, made by tools/gen_words.py */
#include "announce.c"

#if defined(DINGHY_HOST)
/* the host (tests/host.c): each SAY to MVS2_SAID_LOG as "MSG <tab> text" ("WARN" when KEEP), HELLO as "HELLO" */
static int ann_wire_send(const uint8_t *p, uint32_t n)
{
    static FILE *log;
    static int tried;
    uint32_t i;
    if (!tried) { const char *f = getenv("MVS2_SAID_LOG"); tried = 1; if (f) log = fopen(f, "w"); }
    if (!log || n < 7u) return 0;
    fputs(p[4] == ANN_HELLO ? "HELLO\t" : p[5] & ANN_KEEP ? "WARN\t" : "MSG\t", log);
    for (i = 6; i + 1u < n; i++) fputc(p[i] ? (int)p[i] : '|', log);
    fputc('\n', log);
    fflush(log);
    return 0;
}
#elif defined(DINGHY_WEBSIM) || defined(DINGHY_DOCK)
static int ann_wire_send(const uint8_t *p, uint32_t n) { return sysex_send(p, n); }
#else
/* the FM-1: never waits. Not sent with no host, the bus asleep, another send under way, or no room for the whole
 * message (+1: the F7 an earlier cut-off reply is owed, usb.c sx_open) */
static int ann_wire_send(const uint8_t *p, uint32_t n)
{
    if (!usb.config || usb.suspended || sx_busy || SXQ - (so_w - so_r) < (n + 2u) / 3u + 1u)
        return -1;
    return ota_wire_send(p, n);
}
#endif

/* ---- the text: built in one buffer (not on the stack), cut at ANN_MAX */
static char sp_buf[ANN_MAX + 1];
static uint32_t sp_n;
static void sp_reset(void) { sp_n = 0; sp_buf[0] = 0; }
static void sp_cat(const char *s) { while (*s && sp_n < ANN_MAX) sp_buf[sp_n++] = *s++; sp_buf[sp_n] = 0; }
static void sp_num(uint32_t v) { char t[12]; *put_u(t, v) = 0; sp_cat(t); }
static void sp_trim(const char *s, uint32_t len)   /* a name: no trailing spaces */
{
    uint32_t n = len, i;
    while (n > 0u && s[n - 1u] == ' ') n--;
    for (i = 0; i < n && sp_n < ANN_MAX; i++) sp_buf[sp_n++] = s[i];
    sp_buf[sp_n] = 0;
}

static void sp_home_choice(uint32_t id);
/* a knob: "FREQ 64" (an effect's knobs are named by its type; one it doesn't use is "-": not said). A CONTROL
 * route's destination says its page too: "ASSIGN1 TO LFO 1 DEPTH", where the screen shows "DEPTH" */
static int sp_param(uint32_t id)
{
    char v[24];
    const char *lab;
    if (id >= VS_NPARAM) return 0;
    lab = vs_param_label(id, pval(VS_P_FX_TYPE));
    if (lab[0] == '-' && !lab[1]) return 0;
    sp_cat(lab);
    if (VS_PARAMS[id].kind == VS_K_DST && pval(id) >= 0 && pval(id) < (int32_t)VS_NPARAM) {
        sp_cat(" TO "); sp_home_choice((uint32_t)pval(id));
        return 1;
    }
    ptext(id, v, sizeof v);
    sp_cat(" "); sp_cat(v);
    return 1;
}
static void sp_knobs(const uint8_t ids[4])     /* a page's four, ", " between */
{
    uint32_t c, any = 0;
    for (c = 0; c < 4u; c++) {
        const uint32_t at = sp_n;
        if (any) sp_cat(", ");
        if (ids[c] != 0xFFu && sp_param(ids[c])) any = 1;
        else { sp_n = at; sp_buf[sp_n] = 0; }
    }
}
/* the program: "U1 SLOT 12, BRASS PAD" ("U1 SLOT 12, NO NAME"); edited: ", EDITED" */
static void sp_prog(int with_edited)
{
    sp_cat(vs_prog_bank_name(prog.bank)); sp_cat(" SLOT "); sp_num((slot & 127u) + 1u); sp_cat(", ");
    if (prog_name[0]) sp_cat(prog_name); else sp_cat("NO NAME");
    if (with_edited && edited) sp_cat(", EDITED");
}
/* the parameter a HOME knob is set to, by its page: "FILTER FREQ" (ARP's on/off, on no page: "ARP SWITCH") */
static void sp_home_choice(uint32_t id)         /* (a section's on/off, labelled as its page: "LFO 1 SWITCH") */
{
    const uint32_t pg = home_page(id);
    const char *t = pg < VS_NPAGE ? VS_PAGES[pg].title : "ARP", *l = vs_param_label(id, pval(VS_P_FX_TYPE));
    uint32_t i;
    for (i = 0; t[i] && t[i] == l[i]; i++) {}
    sp_cat(t); sp_cat(" ");
    sp_cat(t[i] == l[i] ? "SWITCH" : l);
}
/* a character of a name, as words: "CAPITAL B", "SMALL B", "7", "SPACE", "DASH" */
static void sp_char(char c)
{
    static const char SYM[] = "-_.+#&/()!'";
    static const char *const SYM_SAID[] = {"DASH", "UNDERSCORE", "DOT", "PLUS", "HASH", "AMPERSAND", "SLASH",
                                           "OPEN BRACKET", "CLOSE BRACKET", "EXCLAMATION MARK", "APOSTROPHE"};
    char t[2] = {c, 0};
    uint32_t i;
    if (c == ' ') { sp_cat("SPACE"); return; }
    if (c >= 'A' && c <= 'Z') { sp_cat("CAPITAL "); sp_cat(t); return; }
    if (c >= 'a' && c <= 'z') { t[0] = (char)(c - 32); sp_cat("SMALL "); sp_cat(t); return; }
    for (i = 0; SYM[i]; i++) if (SYM[i] == c) { sp_cat(SYM_SAID[i]); return; }
    sp_cat(t);
}
/* SAVE's target: "TO U1 SLOT 12, EMPTY SLOT" or "TO U1 SLOT 12, REPLACES BRASS PAD" (any bank: vs_prog_peek_name) */
static void sp_save_target(void)
{
    char nm[VS_NAME];
    sp_cat("TO "); sp_cat(vs_prog_bank_name(save_bank)); sp_cat(" SLOT "); sp_num(save_slot + 1u);
    if (vs_prog_peek_name(&prog, save_bank, save_slot, nm)) { sp_cat(", REPLACES "); sp_trim(nm, VS_NAME); }
    else sp_cat(", EMPTY SLOT");
}
/* BANK's row under the cursor: "BRASS PAD, 12 OF 128" ("EMPTY, 13 OF 128") */
static void sp_bank_row(void)
{
    if (vs_prog_empty(&prog, cursor)) sp_cat("EMPTY");
    else sp_trim((const char *)prog.r[cursor].b, VS_NAME);
    sp_cat(", "); sp_num(cursor + 1u); sp_cat(" OF "); sp_num(VS_NPROG);
}
/* GLOBAL's knob c, as its cell shows it */
static void sp_global_cell(uint32_t c)
{
    static const char *const KL[VS_HOME_N] = {"KNOB 1", "KNOB 2", "KNOB 3", "KNOB 4"};
    const int ext = clk_t && fm1_ms - clk_t < 1000u;
    if (gpage == GP_KNOBS) {
        if (c >= VS_HOME_N) return;
        sp_cat(KL[c]); sp_cat(" IS "); sp_home_choice(eng.patch.home[c]);
    } else if (gpage == GP_MAIN) {
        if (c == 0u) { sp_cat("MIDI CH "); if (set.ch) sp_num(set.ch); else sp_cat("ANY"); }
        else if (c == 1u) { sp_cat("VOICES "); sp_num(set.voices); }
        else if (c == 2u) { sp_cat("TEMPO "); sp_num(set.tempo); }
        else if (c == 3u) sp_cat(ext ? "CLOCK EXT" : "CLOCK INT");
    } else if (gpage == GP_LOOK) {
        if (c == 0u) { sp_cat("THEME "); sp_cat(UI_THEMES[set.theme].name); }
        else if (c == 1u) sp_cat(set.colour ? "COLOUR PATCH" : "COLOUR THEME");
    } else {
        if (c == 0u) { sp_cat("CPU "); sp_num(dg_avg); sp_cat("%"); }
        else if (c == 1u) { sp_cat("PEAK "); sp_num(dg_peak); sp_cat("%"); }
        else if (c == 2u) { sp_cat("CORES "); sp_num(vs_core1_online() ? 2u : 1u); }
        else if (c == 3u) { sp_cat("DROPPED "); sp_num(eng.shed); }
    }
}

/* the screen as it is: its name first, then what is on it. all: what a glance takes in too (the program at the
 * foot; WHERE and the first HELLO) */
static void sp_front(int all)
{
    uint32_t c, any;
    sp_reset();
    if (scr == SCR_PAGE) {
        const vs_page_t *pg = &VS_PAGES[page];
        sp_cat(pg->title);
        if (pg->of > 1u) { sp_cat(" PAGE "); sp_num(pg->n); sp_cat(" OF "); sp_num(pg->of); }
        sp_cat(". ");
        sp_knobs(pg->id);
        if (all) { sp_cat(". PATCH "); sp_prog(1); }
    } else if (scr == SCR_HOME) {
        sp_cat("HOME. "); sp_prog(1); sp_cat(". ");
        sp_knobs(eng.patch.home);
    } else if (scr == SCR_BANK) {
        sp_cat("BANK "); sp_cat(vs_prog_bank_name(prog.bank)); sp_cat(". ");
        sp_bank_row();
    } else if (scr == SCR_SAVE) {
        sp_cat("SAVE "); sp_save_target(); sp_cat(". NAME ");
        if (save_name[0] != ' ' || save_pos) sp_trim(save_name, VS_NAME); else sp_cat("EMPTY");
        sp_cat(". SAVE AGAIN TO WRITE, ANY OTHER BUTTON TO LEAVE");
        if (save_bank != prog.bank) sp_cat(". THE BANK LOADS ON SAVE");
    } else {                                    /* GLOBAL */
        if (gpage == GP_KNOBS) sp_cat("HOME KNOBS. ");
        else { sp_cat("GLOBAL "); sp_num(gpage); sp_cat(" OF 3. "); }
        for (c = 0, any = 0; c < 4u; c++) {      /* (DIAG's meters too: WHERE and the page say them, a change doesn't) */
            const uint32_t at = sp_n;
            uint32_t was;
            if (any) sp_cat(", ");
            was = sp_n;
            sp_global_cell(c);
            if (sp_n == was) { sp_n = at; sp_buf[sp_n] = 0; } else any = 1;
        }
        if (all) { sp_cat(". PATCH "); sp_prog(1); }
    }
}

/* what the reader was last told, to say only what changed */
static struct {
    uint8_t started;
    int8_t oct;
    uint32_t ctx, hot_t, gl_t, bank, slot, cursor, sbank, sslot, spos;
    char schar;
    char knob[ANN_MAX + 1];                     /* the last readout (a knob that didn't change says nothing) */
} sp;

static uint32_t sp_ctx(void)                    /* the screen, its page: a change is a new screen (said when it settles) */
{
    uint32_t h = hsh(2166136261u, scr);
    if (scr == SCR_PAGE) h = hsh(h, page);
    if (scr == SCR_GLOBAL) h = hsh(h, gpage);
    if (scr == SCR_BANK) h = hsh(h, prog.bank);
    return h;
}
static void sp_mark(void)                       /* all that is said now, or need not be */
{
    sp.ctx = sp_ctx(); sp.hot_t = hot_t; sp.gl_t = gl_t; sp.bank = prog.bank; sp.slot = slot; sp.cursor = cursor;
    sp.sbank = save_bank; sp.sslot = save_slot; sp.spos = save_pos; sp.schar = save_name[save_pos]; sp.oct = set.oct;
    sp.knob[0] = 0;
}
static void sp_later_if_new(uint32_t quiet_ms)  /* a readout: said once it settles, if it is not what was said */
{
    uint32_t i;
    for (i = 0; sp_buf[i] && sp_buf[i] == sp.knob[i]; i++) {}
    if (sp_buf[i] == sp.knob[i] || !sp_n) return;
    for (i = 0; i <= sp_n; i++) sp.knob[i] = sp_buf[i];
    ann_later(sp_buf, ANN_NOW, quiet_ms, fm1_ms);
}

/* the messages: the answer to SAVE, all notes off */
static void speak_saved(int ok)
{
    if (!ann_on()) return;
    sp_reset();
    if (ok) { sp_cat("SAVED "); sp_prog(0); ann_say(sp_buf, 0, ANN_NOW); }
    else ann_say("NOT SAVED: THE BANK COULD NOT BE WRITTEN", 0, ANN_NOW | ANN_KEEP);
    sp.slot = slot; sp.bank = prog.bank;        /* (the program it now is, and HOME it goes back to: said here, not */
    sp.ctx = sp_ctx();                          /*  again as a change that would cut this off) */
}
static void speak_panic(void) { if (ann_on()) ann_say("ALL NOTES OFF", 0, ANN_NOW); }

static void speak(void)
{
    const int where = ann_where();
    uint32_t c;
    if (!ann_on()) { sp.started = 0; return; }
    if (!sp.started || where) {                 /* everything on the screen, now */
        sp.started = 1;
        sp_front(1);
        ann_say(sp_buf, 0, ANN_NOW);
        sp_mark();
        return;
    }
    if (set.oct != sp.oct) {                    /* OCT- / OCT+: an answer to a button, now */
        char t[16], *q = put_s(t, "OCTAVE ");
        sp.oct = set.oct;
        if (set.oct) q = put_u(put_s(q, set.oct > 0 ? "UP " : "DOWN "), (uint32_t)(set.oct > 0 ? set.oct : -set.oct));
        else q = put_s(q, "0");
        *q = 0;
        ann_say(t, 0, ANN_NOW);
    }
    c = sp_ctx();
    if (c != sp.ctx) {                          /* a new screen or page: said when page flipping settles */
        sp_front(0);
        ann_later(sp_buf, ANN_NOW, 200u, fm1_ms);
        sp_mark();
        return;
    }
    if (scr == SCR_BANK) {                      /* the cursor (it plays the slot: no program change said too) */
        sp.bank = prog.bank; sp.slot = slot;
        if (cursor != sp.cursor) { sp.cursor = cursor; sp_reset(); sp_bank_row(); sp_later_if_new(120u); }
    } else if (scr == SCR_SAVE) {               /* a knob: the bank, the slot, the cursor, the character */
        sp_reset();
        if (save_bank != sp.sbank || save_slot != sp.sslot) { sp_save_target(); }
        else if (save_pos != sp.spos) { sp_cat("CURSOR "); sp_num(save_pos + 1u); sp_cat(", "); sp_char(save_name[save_pos]); }
        else if (save_name[save_pos] != sp.schar) sp_char(save_name[save_pos]);
        sp.sbank = save_bank; sp.sslot = save_slot; sp.spos = save_pos; sp.schar = save_name[save_pos];
        if (sp_n) sp_later_if_new(120u);
        return;
    } else if (prog.bank != sp.bank || slot != sp.slot) {   /* PRESETS, a program change, a bank select */
        if (slot_want == slot && !want_reload) {             /* (once it is in: the name is the new one's) */
            sp.bank = prog.bank; sp.slot = slot;
            sp_reset(); sp_prog(0);
            sp.knob[0] = 0;
            ann_later(sp_buf, ANN_NOW, 150u, fm1_ms);
        }
    }
    if (scr == SCR_GLOBAL && gl_t != sp.gl_t) {  /* GLOBAL's knobs: the one turned (ui_gtouch; DIAG's meters: not) */
        sp.gl_t = gl_t;
        sp_reset(); sp_global_cell(col);
        sp_later_if_new(120u);
    }
    if (hot_t != sp.hot_t) {                    /* a parameter turned, or ARP tapped: "CUTOFF 64" */
        sp.hot_t = hot_t;
        sp_reset();
        if (sp_param(big_id)) sp_later_if_new(120u);
    }
}

#ifdef DINGHY_HOST
/* every screen, knob, value and message as a reader gets them: "KIND <tab> text", for fm1-reader's tools/audit.mjs
 * (tests/run_speak.sh). MVS2_SPEAKDUMP=FILE runs it instead of the app. */
static void speak_dump(const char *path)
{
    FILE *f = fopen(path, "w");
    const vs_patch_t keep = eng.patch;
    const uint32_t keep_scr = scr, keep_page = page, keep_g = gpage;
    uint32_t k, c, id, t;
    int32_t v;
    if (!f) { perror(path); exit(1); }
    if (slot >= VS_NPROG) {                     /* (before the main loop: the first program, as it will be) */
        slot = slot_want;
        for (k = 0; k < VS_NAME; k++) prog_name[k] = (char)prog.r[slot].b[k];
        prog_name[VS_NAME] = 0;
        for (k = VS_NAME; k > 0u && prog_name[k - 1u] == ' '; k--) prog_name[k - 1u] = 0;
    }
#define OUT(kind) fprintf(f, "%s\t%s\n", kind, sp_buf)
    ann.on = 1;
    for (k = 0; k < VS_NPAGE; k++) {            /* the pages, as reached and as WHERE says them */
        scr = SCR_PAGE; page = k;
        sp_front(0); OUT("SCREEN");
        sp_front(1); OUT("WHERE");
    }
    for (id = 0; id < VS_NPARAM; id++) {        /* every value of every parameter, with each thing that changes its words */
        const int lfo = id == VS_P_LFO1_SPEED || id == VS_P_LFO2_SPEED, fx = id >= VS_P_FX_CTRL1 && id <= VS_P_FX_LEVEL;
        const vs_param_info_t *p = &VS_PARAMS[id];
        for (t = 0; t < (lfo ? 2u : fx ? 10u : 1u); t++) {
            eng.patch = keep;
            if (lfo) eng.patch.v[id == VS_P_LFO1_SPEED ? VS_P_LFO1_TEMPO : VS_P_LFO2_TEMPO] = (int32_t)t;
            if (fx) eng.patch.v[VS_P_FX_TYPE] = (int32_t)t;
            if (p->kind == VS_K_SRC) {
                static const int32_t S[] = {-1, 0, 0xB001, 0xB040, 0xB07F, 0xE000, 0xD000};
                for (c = 0; c < sizeof S / sizeof S[0]; c++) { eng.patch.v[id] = S[c]; sp_reset(); if (sp_param(id)) OUT("KNOB"); }
                continue;
            }
            for (v = p->min; v <= (p->kind == VS_K_DST ? (int32_t)VS_NPARAM - 1 : pmax(id)); v++) {
                eng.patch.v[id] = v;
                sp_reset();
                if (sp_param(id)) OUT("KNOB");
            }
        }
    }
    eng.patch = keep;
    scr = SCR_HOME; sp_front(0); OUT("SCREEN"); sp_front(1); OUT("WHERE");
    edited = 1; sp_front(0); OUT("SCREEN"); edited = 0;
    scr = SCR_BANK;
    for (cursor = 0; cursor < VS_NPROG; cursor++) { sp_reset(); sp_bank_row(); OUT("ROW"); }
    cursor = 0; sp_front(0); OUT("SCREEN");
    scr = SCR_SAVE; save_bank = 1; save_slot = 0; save_begin();
    sp_front(0); OUT("SCREEN");
    save_bank = prog.bank ? prog.bank + 1u : 2u; sp_front(0); OUT("SCREEN");
    for (k = 0; k < NAME_NCH; k++) { sp_reset(); sp_char(NAME_CHARS[k]); OUT("VALUE"); }
    sp_reset(); sp_cat("CURSOR "); sp_num(VS_NAME); sp_cat(", "); sp_char('A'); OUT("KNOB");
    scr = SCR_GLOBAL;
    for (gpage = 0; gpage < GP_N; gpage++) { sp_front(0); OUT("SCREEN"); }
    gpage = GP_KNOBS;
    for (k = 0; k < home_n; k++) { eng.patch.home[0] = home_list[k]; sp_reset(); sp_global_cell(0); OUT("KNOB"); }
    eng.patch = keep;
    gpage = GP_MAIN;
    { const uint8_t ch = set.ch; for (set.ch = 0; set.ch <= 16u; set.ch++) { sp_reset(); sp_global_cell(0); OUT("KNOB"); } set.ch = ch; }
    sp_reset(); sp_global_cell(3); OUT("KNOB");
    gpage = GP_LOOK;
    { const uint8_t th = set.theme; for (set.theme = 0; set.theme < UI_NTHEME; set.theme++) { sp_reset(); sp_global_cell(0); OUT("KNOB"); } set.theme = th; }
    sp_reset(); sp_global_cell(1); OUT("KNOB");
    fprintf(f, "MSG\tOCTAVE UP 2\nMSG\tOCTAVE DOWN 1\nMSG\tOCTAVE 0\nMSG\tALL NOTES OFF\n");
    sp_reset(); sp_cat("SAVED "); sp_prog(0); OUT("MSG");
    fprintf(f, "WARN\tNOT SAVED: THE BANK COULD NOT BE WRITTEN\n");
#undef OUT
    fclose(f);
    scr = keep_scr; page = keep_page; gpage = keep_g; ann.on = 0;
}
#endif
