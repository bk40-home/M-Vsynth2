/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2 panel: Synth1's sections as pages on the FM-1, built as M-vance's panel is (mv_ui.h): a section button
 * steps through its pages, KNOB1..4 edit the page's four columns, SELECT single-steps the knob touched last,
 * ALGORITHM walks every page, PRESETS picks the program, HOME goes home. The page table is generated
 * (firmware/gen/vs_params.h from tools/vsynth2-params.yaml), and the cheat sheet from the page table.
 *
 *   LFO    LFO 1 (2: WAVE SPEED DEPTH DEST / LFO 1 TEMPO KEYSYNC), LFO 2 (2, the same)
 *   EDIT   OSC 1 (SHAPE DETUNE FM PHASE), SUB OSC (GAIN SHAPE OCTAVE), OSC 2 (SHAPE PITCH FINE TRACK),
 *          OSC MIX (MIX P/W SYNC RING), OSC COMMON (KEYSHFT TUNE M.ENV M.DEST)
 *   ENV    MOD ENV (ATTACK DECAY AMOUNT), FILTER ENV (A D S R), AMP ENV (A D S R)
 *   SEL    FILTER (2: TYPE FREQ RES AMOUNT / KBDTRK SAT VEL SW), AMP (GAIN VEL PAN)
 *   FX     FX SWITCH (EFFECT DELAY CHORUS on / off), EFFECT (TYPE CTRL 1 CTRL 2 LEVEL: the two CTRLs and LEVEL
 *          are labelled by what they do in the type playing, vs_param_label), EQ (TONE FREQ LEVEL Q),
 *          DELAY (2: TYPE TIME FEEDBK DRY/WET / SPREAD TONE), CHORUS (2: TYPE TIME DEPTH RATE / FEEDBK LEVEL)
 *   GLO    VOICE (MODE POLY PORTA AUTO), UNISON (2: UNISON VOICES DETUNE SPREAD / PITCH PHASE),
 *          CONTROL (2: SRC 1 ASSIGN1 SENS 1 BEND / SRC 2 ASSIGN2 SENS 2), GLOBAL (MIDI CH, VOICES, TEMPO: kept in
 *          flash, not the patch's), MIDI (LOCAL, CLOCK, CLK OUT: M-vance's), DEBUG (vs_debug.h)
 *   ARP    a tap: the arp on / off (parameter 59; lit when on); held 0.5 s: ARP (TYPE RANGE BEAT GATE)
 *   SEQ    BANK: the bank browser. KNOB1 BANK (FACTORY, U1 .. U8, with its name), KNOB2 / PRESETS the slot; nine
 *          rows of "nnn name" around it, EMPTY slots dim. The patch under the cursor plays at once (Synth2's
 *          browser: arrows load); HOME or SEQ again leaves
 *   SAVE   its page: TO (the slot, in the bank shown), CURSOR and CHAR (the name, 16 characters), MODE WRITE (the
 *          patch as edited into TO: also the copy) / SWAP (the program playing and TO trade places) / CLEAR (TO
 *          EMPTY) / INIT (the INIT patch into the panel, nothing written). SAVE again does it. On FACTORY (read-
 *          only) the page says so and offers U1
 *   OCT    OCT- / OCT+ the keys' octave (kept in flash). Held together 1 s: all notes off
 *   PLAY, REC  not used (Synth1 has no sequencer). MIDI START restarts the arp
 *
 * Screens (240 x 240; the family's look: labels grey, values bright, rules dim, the knob being turned white):
 *   a page   y   0- 23  header: the title, "1/2", the button's name at the right
 *            y  30-139  4 columns of 60 px: label, value, a bar for numbers (a centre tick for signed ones), just
 *                       the value for choices and switches
 *            y 150-175  the last edit, large: "FILTER FREQ  64" (3 s)
 *            y 190-239  the program: "U1 012 name", * when edited
 *   HOME     y   0- 47  header: two rows of five wave cycles (sine saw pulse tri noise) and a plate with the name:
 *                       M-Vsynth2's own drawing (tools/gen_logo.py). Never Synth1's panel or its artwork
 *            y  48- 99  the bank ("U1 MY BANK") and the program: "012" and its name large; EDITED; the patch's
 *                       colour as a chip
 *            y 102-129  KNOB1..4 on HOME: FREQ, RES, AMOUNT (the filter env's), GAIN; the one turned white 3 s
 *            y 132-143  the voices: a cell each, bright held, dim releasing (Synth2's voice grid, vstate)
 *            y 146-239  the scope of the output (M-vance's home_scope)
 *   XFER     RECEIVING / SENDING, the bank and slot, the count; SAVING while the flash is written
 *   ASK      a firmware with a new record revision: the user banks are kept as they are (the record carries its
 *            revision); a bank it can not read is shown as "U3 (OLD FIRMWARE)" and left alone until written over
 *
 * MIDI out: every edit goes out on the parameter's CC (VS_PARAMS[].cc), so a DAW records the panel; the same CCs
 * in edit the patch. A CC that one of the patch's CTRL routes names keeps that job as well.
 * Main loop only (never the ISR). It is #included into mvsynth2.c (the unity build): it draws with gfx.c. */
#pragma once
#include <stdint.h>
#include "vs_voice.h"
#include "vs_prog.h"

typedef struct {
    uint8_t page;                               /* 0 = HOME, else an index into the page table */
    uint8_t last;                               /* the column last touched (0..3), 0xFF none */
    uint8_t redraw;                             /* the whole page on the next draw */
    uint32_t t_touch;                           /* fm1_ms of the last edit */
    uint32_t t_knob[4];                         /* fm1_ms of each knob's last detent (acceleration) */
    uint32_t drawn[4], drawn_line, drawn_prog;  /* keys of what the screen shows: redrawn only when they move */
    uint32_t arp_t;                             /* fm1_ms the ARP button went down */
    uint8_t arp_down, arp_did;
    const vs_prog_t *prog;                      /* the bank shown */
    uint8_t cur;                                /* the slot playing (0..127; the caller keeps it) */
    uint8_t edited;                             /* the patch differs from its slot */
    uint8_t sv_to, sv_pos, sv_mode;             /* SAVE: the target slot, the name's cursor, VS_SV_* */
    uint8_t act, act_to;                        /* a SAVE asked for (VS_SV_*, 0 none) and its slot: the caller does it */
    uint8_t bk_bank, bk_slot;                   /* BANK: the bank and slot under the cursor */
    uint8_t midi;                               /* the MIDI page's bits (kept in flash) */
} vs_ui_t;
enum { VS_SV_NONE, VS_SV_WRITE, VS_SV_SWAP, VS_SV_CLEAR, VS_SV_INIT };

void vs_ui_init(vs_ui_t *u, const vs_prog_t *prog);
#define VS_UI_PAGE 1u                           /* the page changed (to HOME: the caller redraws its screen) */
#define VS_UI_SETTING 2u                        /* a setting kept in flash changed */
#define VS_UI_PROG 4u                           /* SAVE pressed on its page: do u->act on slot u->act_to */
#define VS_UI_SELECT 8u                         /* BANK or PRESETS chose bk_bank / bk_slot: the caller loads it */
uint32_t vs_ui_input(vs_ui_t *u, vs_engine_t *e, uint32_t pressed, uint32_t released, uint8_t *midi_ch,
                     uint32_t out_ch);
static inline int vs_ui_home(const vs_ui_t *u) { return u->page == 0u; }
uint32_t vs_ui_button(const vs_ui_t *u);        /* the button of the page shown: its LED lit */
void vs_ui_draw(vs_ui_t *u, const vs_engine_t *e, uint32_t midi_ch);   /* a page (not HOME): what changed */
