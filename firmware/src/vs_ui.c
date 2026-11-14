/* SPDX-License-Identifier: GPL-3.0-only */
/* M-Vsynth2's screens: knobs, graphs and themes (docs/design/mvsynth2-ui-2026-11-14.md). Included by mvsynth2.c after
 * its state (the engine, the programs, the settings, the screen and page in view); nothing here edits the patch.
 *
 * Every screen is four bands, the same on every page, so a hand on KNOB 2 always reads the second cell:
 *   header   0..21    the section as a chip, the title, a pip a page of the section, a cell a voice (held, releasing)
 *   panel   24..127   what the page does, drawn from the patch's values (ui_panel_render); the readout of the knob in
 *                     hand (1.5 s after it turns) at its top left
 *   cells  130..219   KNOB 1..4, left to right: a knob (ui_cell), its label, its value
 *   footer 222..239   the bank and slot, the name (WARN: edited), the tempo or the bank's state
 * A band is drawn into gfx.c's canvas (CV_MAX, 240 x 64: the panel goes in two strips of 52 rows) and sent with one
 * DMA, and only when what it shows changed (a hash a band). The LFO's dot and HOME's scope redraw the panel at most
 * 12 times a second; a knob's turn, 25.
 *
 * The knob: a 270-degree ring from 7 to 5 o'clock (KNOB_ANG: a pixel's angle, 0..240), anti-aliased from KNOB_RING's
 * coverage. VALUE lights the ring from its start to the value; CENTRE from 12 o'clock (signed values) with a tick;
 * CHOICE cuts it into a segment a choice, the one chosen lit; SWITCH is a switch, not a knob; ROUTE (the CTRL routes:
 * a source or a parameter) has no ring. The knob in hand: its arc, its rim and its value in ACCENT. FREQ carries a
 * thin outer arc: the span the filter envelope's AMOUNT adds.
 *
 * Themes: eight colours each (UI_THEMES); FILL and GRID are mixed from them when the theme is set. GLOBAL 2/2 picks
 * the theme and COLOUR: THEME, or PATCH (the patch's .sy1 colour takes the THEME colour's place; "default" keeps it).
 *
 * Main loop only. Integers only: no float, no 64-bit division. */
#include "vs_ui_gen.h"

/* ---- themes */
typedef struct {
    const char *name;
    uint16_t bg, surf, raised, line, dim, text, theme, accent, warn;
} ui_theme_def_t;
static const ui_theme_def_t UI_THEMES[] = {
    {"SEA", RGB(11, 15, 16), RGB(20, 26, 28), RGB(35, 44, 47), RGB(52, 66, 74), RGB(143, 160, 166), RGB(232, 240, 240),
     RGB(0, 200, 160), RGB(255, 180, 84), RGB(255, 140, 40)},
    {"EMBER", RGB(16, 13, 11), RGB(27, 22, 19), RGB(45, 37, 32), RGB(70, 58, 50), RGB(168, 152, 140), RGB(243, 234, 226),
     RGB(255, 122, 61), RGB(92, 214, 255), RGB(255, 209, 102)},
    {"ICE", RGB(10, 13, 18), RGB(19, 24, 34), RGB(33, 42, 56), RGB(51, 64, 90), RGB(142, 155, 179), RGB(230, 236, 247),
     RGB(77, 184, 255), RGB(255, 209, 102), RGB(255, 140, 40)},
    {"ORCHID", RGB(14, 11, 18), RGB(24, 20, 31), RGB(40, 34, 51), RGB(63, 54, 82), RGB(162, 151, 181), RGB(238, 232, 247),
     RGB(208, 123, 255), RGB(125, 255, 176), RGB(255, 140, 40)},
    {"CRT", RGB(6, 10, 6), RGB(13, 20, 13), RGB(24, 36, 24), RGB(39, 64, 42), RGB(127, 168, 132), RGB(216, 245, 216),
     RGB(124, 255, 107), RGB(255, 255, 255), RGB(255, 170, 40)},
    {"PAPER", RGB(242, 239, 232), RGB(231, 227, 217), RGB(251, 250, 246), RGB(189, 183, 169), RGB(92, 88, 79), RGB(26, 25, 23),
     RGB(6, 97, 79), RGB(168, 51, 10), RGB(168, 51, 10)},
};
#define UI_NTHEME (sizeof UI_THEMES / sizeof UI_THEMES[0])
#define UI_LIGHT 5u                             /* the light theme (the patch colours darken on it) */
/* the patch colours (vs_patch.h: 1 red, 2 blue, 3 green, 4 yellow; 0 default: the theme's own) */
static const uint16_t UI_PCOL[2][4] = {
    {RGB(255, 90, 90), RGB(77, 163, 255), RGB(72, 209, 122), RGB(255, 210, 77)},
    {RGB(192, 42, 42), RGB(26, 95, 180), RGB(30, 122, 60), RGB(138, 106, 0)},
};

static struct {
    uint16_t bg, surf, raised, line, dim, text, theme, accent, warn, fill, grid, rel;
} T;
static uint32_t ui_theme_key = ~0u;

/* a + (b - a) x t / 15, per channel (t 0..15) */
static uint16_t mix565(uint32_t a, uint32_t b, uint32_t t)
{
    const uint32_t u = 15u - t;
    const uint32_t r = (((a >> 11) * u + (b >> 11) * t) * 273u + 2048u) >> 12;
    const uint32_t g = ((((a >> 5) & 63u) * u + ((b >> 5) & 63u) * t) * 273u + 2048u) >> 12;
    const uint32_t bl = (((a & 31u) * u + (b & 31u) * t) * 273u + 2048u) >> 12;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static uint16_t ui_patch_colour(uint32_t c)     /* a .sy1 colour, 0 the theme's own */
{
    return c >= 1u && c <= 4u ? UI_PCOL[set.theme == UI_LIGHT][c - 1u] : T.theme;
}

static void ui_theme_service(void)              /* the theme in use; a change redraws everything */
{
    const ui_theme_def_t *d = &UI_THEMES[set.theme < UI_NTHEME ? set.theme : 0u];
    const uint32_t pc = set.colour && eng.patch.color >= 1u && eng.patch.color <= 4u ? eng.patch.color : 0u;
    const uint32_t key = (uint32_t)set.theme << 8 | pc;
    if (key == ui_theme_key) return;
    ui_theme_key = key;
    T.bg = d->bg; T.surf = d->surf; T.raised = d->raised; T.line = d->line; T.dim = d->dim; T.text = d->text;
    T.accent = d->accent; T.warn = d->warn;
    T.theme = pc ? UI_PCOL[set.theme == UI_LIGHT][pc - 1u] : d->theme;
    T.fill = mix565(T.surf, T.theme, 3);
    T.grid = mix565(T.surf, T.line, 8);
    T.rel = mix565(T.surf, T.theme, 7);
    need_full = 1;
}

/* ---- drawing on the canvas (gfx.c: cv_px, cv_w, cv_h, cv_oy; screen y = canvas y - cv_oy) */
static uint32_t isqrt32(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

static void pxa(int32_t x, int32_t y, uint16_t c, uint32_t a)   /* a pixel at coverage a (0..15) */
{
    uint32_t i;
    y += cv_oy;
    if (!a || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h) return;
    i = (uint32_t)y * cv_w + (uint32_t)x;
    cv_px[i] = a >= 15u ? swap16(c) : swap16(mix565(swap16(cv_px[i]), c, a));
}

static void fill(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { if (w > 0 && h > 0) cv_rect(x, y, w, h, c); }

/* a rounded rectangle, its corners anti-aliased (r up to h / 2: a pill) */
static void rrect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t c)
{
    int32_t i, j;
    if (w <= 0 || h <= 0) return;
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    fill(x + r, y, w - 2 * r, h, c);
    fill(x, y + r, r, h - 2 * r, c);
    fill(x + w - r, y + r, r, h - 2 * r, c);
    for (j = 0; j < r; j++)
        for (i = 0; i < r; i++) {               /* a corner pixel: its distance (Q4) from the corner circle's centre */
            const int32_t dx = (r - i) * 16 - 8, dy = (r - j) * 16 - 8;
            const int32_t d = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy)), cv = r * 16 + 8 - d;
            const uint32_t a = cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u;
            pxa(x + i, y + j, c, a); pxa(x + w - 1 - i, y + j, c, a);
            pxa(x + i, y + h - 1 - j, c, a); pxa(x + w - 1 - i, y + h - 1 - j, c, a);
        }
}
static void rframe(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint16_t border, uint16_t inside)
{
    rrect(x, y, w, h, r, border);
    rrect(x + 1, y + 1, w - 2, h - 2, r > 1 ? r - 1 : 0, inside);
}

/* a disc (Q4 centre and radius) */
static void disc(int32_t cx, int32_t cy, int32_t rq, uint16_t c)
{
    int32_t x, y;
    for (y = (cy - rq) >> 4; y <= (cy + rq) >> 4; y++)
        for (x = (cx - rq) >> 4; x <= (cx + rq) >> 4; x++) {
            const int32_t dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
            const int32_t cv = rq + 8 - (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
            pxa(x, y, c, cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u);
        }
}

/* a line a..b (Q4), w wide (Q4), round ends */
static void seg(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t w, uint16_t c)
{
    const int32_t dx = bx - ax, dy = by - ay, hw = w / 2;
    const int32_t L = (int32_t)isqrt32((uint32_t)(dx * dx + dy * dy));
    int32_t x, y, ux, uy, x0, x1, y0, y1;
    if (L < 2) { disc(ax, ay, hw, c); return; }
    ux = dx * 4096 / L; uy = dy * 4096 / L;
    x0 = ((ax < bx ? ax : bx) - hw - 16) >> 4; x1 = ((ax > bx ? ax : bx) + hw + 16) >> 4;
    y0 = ((ay < by ? ay : by) - hw - 16) >> 4; y1 = ((ay > by ? ay : by) + hw + 16) >> 4;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            const int32_t rx = x * 16 + 8 - ax, ry = y * 16 + 8 - ay;
            const int32_t along = (rx * ux + ry * uy) >> 12;
            int32_t d, cv;
            if (along < 0) d = (int32_t)isqrt32((uint32_t)(rx * rx + ry * ry));
            else if (along > L) { const int32_t ex = x * 16 + 8 - bx, ey = y * 16 + 8 - by; d = (int32_t)isqrt32((uint32_t)(ex * ex + ey * ey)); }
            else { d = (rx * uy - ry * ux) >> 12; if (d < 0) d = -d; }
            cv = hw + 8 - d;
            pxa(x, y, c, cv <= 0 ? 0u : cv >= 16 ? 15u : (uint32_t)cv * 15u / 16u);
        }
}

/* a column's span lo..hi (Q4 rows), its ends anti-aliased */
static void vspan(int32_t x, int32_t lo, int32_t hi, uint16_t c)
{
    int32_t r;
    if (lo < 0) lo = 0;
    for (r = lo >> 4; r * 16 < hi; r++) {
        const int32_t t = r * 16 > lo ? r * 16 : lo, b = r * 16 + 16 < hi ? r * 16 + 16 : hi;
        if (b > t) pxa(x, r, c, (uint32_t)(b - t) * 15u / 16u);
    }
}

/* a curve: yq[i] (Q4 screen rows) at column x0 + i, i < n; stroke w (Q4). dash: draw 4 columns of every 8 */
static void curve(const int16_t *yq, uint32_t n, int32_t x0, int32_t w, uint16_t c, int dash)
{
    uint32_t i;
    for (i = 0; i + 1u < n; i++) {
        const int32_t a = yq[i], b = yq[i + 1u];
        if (dash && (i & 4u)) continue;
        vspan(x0 + (int32_t)i, (a < b ? a : b) - w / 2, (a > b ? a : b) + w / 2 + 8, c);
    }
}
/* the area between a curve and a row (Q4), in c */
static void under(const int16_t *yq, uint32_t n, int32_t x0, int32_t base, uint16_t c)
{
    uint32_t i;
    for (i = 0; i + 1u < n; i++) {
        const int32_t y = (yq[i] + yq[i + 1u]) / 2 + 8;
        if (y < base) vspan(x0 + (int32_t)i, y, base, c);
        else vspan(x0 + (int32_t)i, base, y, c);
    }
}

/* text: f's coverage blended onto what is there; returns the end x. txt_track: px added to each advance (a long
 * value's -1, ui_cell) */
static int32_t txt_track;
static int32_t txt(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    for (; *s; s++) {
        const uint32_t gi = glyph(f, (uint8_t)*s), w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        uint32_t gx, gy;
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                const uint32_t v = gd[gy * bpr + gx / 2u];
                pxa(x - f->pad + (int32_t)gx, y + (int32_t)gy, c, (gx & 1u) ? (v & 15u) : (v >> 4));
            }
        x += f->adv[gi] + txt_track;
    }
    return x;
}
/* text cut to w px (an ellipsis if it is cut), aligned 0 left, 1 centre, 2 right on x */
static void txt_fit(int32_t x, int32_t y, int32_t w, const felucca_font_t *f, const char *s, uint16_t c, int align)
{
    char b[32];
    uint32_t n = 0;
    int32_t tw = text_w(f, s);
    if (tw > w) {
        const int32_t dots = text_w(f, "..");
        tw = dots;
        while (s[n] && n < sizeof b - 3u && tw + f->adv[glyph(f, (uint8_t)s[n])] <= w) { tw += f->adv[glyph(f, (uint8_t)s[n])]; b[n] = s[n]; n++; }
        b[n++] = '.'; b[n++] = '.'; b[n] = 0;
        s = b;
    }
    txt(align == 1 ? x - tw / 2 : align == 2 ? x - tw : x, y, f, s, c);
}
/* a chip: text on RAISED, right-aligned on x (align 2) or from x (0) */
static int32_t chip(int32_t x, int32_t y, const char *s, uint16_t c, int align)
{
    const int32_t w = text_w(&FONT_LAB, s) + 8;
    if (align == 2) x -= w;
    rrect(x, y, w, 14, 3, T.raised);
    txt(x + 4, y + 1, &FONT_LAB, s, c);
    return w;
}

/* ---- the bands */
enum { Y_HEAD = 0, H_HEAD = 22, Y_PANEL = 24, H_PANEL = 104, Y_CELL = 130, H_CELL = 90, Y_FOOT = 222, H_FOOT = 18 };
enum { PX0 = 10, PW = 220, PY0 = 32, PY1 = 120 };   /* the plot inside the panel */
static uint32_t k_head, k_panel, k_cell[4], k_foot, t_panel;

static uint32_t strlen_u(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static uint32_t hsh(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t hstr(uint32_t h, const char *s) { while (*s) h = hsh(h, (uint8_t)*s++); return h; }

static uint32_t hot_t;                          /* when big_id (the knob in hand) last moved */
static int ui_hot(uint32_t id) { return id == big_id && id != 0xFFu && fm1_ms - hot_t < 1500u; }
static uint32_t gl_t;                           /* when a GLOBAL knob last moved (ui_gtouch: mvsynth2.c global_edit) */
static void ui_gtouch(void) { gl_t = fm1_ms ? fm1_ms : 1u; }
static int ui_ghot(uint32_t c) { return c == col && gl_t && fm1_ms - gl_t < 1500u; }
static void ui_touch(uint32_t id) { big_id = id; hot_t = fm1_ms ? fm1_ms : 1u; }

/* the header: chip, title, pips (n pages, i shown), the voices */
static void ui_head(const char *chp, uint16_t chc, const char *title, uint32_t n, uint32_t i)
{
    uint32_t k, key = hsh(hstr(hstr(2166136261u, chp), title), n << 8 | i), vs = 0;
    int32_t x;
    for (k = 0; k < VS_NV; k++) vs = vs << 2 | (eng.v[k].gate ? 2u : eng.v[k].active ? 1u : 0u);
    key = hsh(hsh(key, vs), chc);
    if (key == k_head) return;
    k_head = key;
    cv_begin(240, H_HEAD, T.surf);
    x = 5 + text_w(&FONT_LAB, chp) + 8;
    rframe(5, 4, x - 5, 14, 3, chc, T.surf);
    txt(9, 5, &FONT_LAB, chp, chc);
    x = txt(x + 6, 2, &FONT_TTL, title, T.text) + 8;
    if (n > 1u)
        for (k = 0; k < n && x + 9 < 186; k++, x += 9) rrect(x, 10, 6, 3, 1, k == i ? T.theme : T.line);
    for (k = 0; k < VS_NV; k++) {
        const uint32_t s = vs >> (2u * (VS_NV - 1u - k)) & 3u;
        rrect(190 + (int32_t)k * 6, 6, 4, 10, 1, s == 2u ? T.theme : s ? T.rel : T.line);
    }
    cv_blit(0, Y_HEAD);
}

/* the footer: left text (lc), a name (nc), right text (rc) */
static void ui_foot(const char *left, const char *name, uint16_t nc, const char *right, uint16_t rc)
{
    const uint32_t key = hsh(hsh(hstr(hstr(hstr(2166136261u, left), name), right), nc), rc);
    int32_t x;
    if (key == k_foot) return;
    k_foot = key;
    cv_begin(240, H_FOOT, T.bg);
    fill(0, 0, 240, 1, T.line);
    x = txt(6, 4, &FONT_LAB, left, T.dim);
    if (*name) txt_fit(*left ? x + 6 : 6, 4, 228 - (*left ? x + 6 : 6) - text_w(&FONT_LAB, right) - 6, &FONT_LAB, name, nc, 0);   /* (small: Kris 14 Nov) */
    txt(234 - text_w(&FONT_LAB, right), 4, &FONT_LAB, right, rc);
    cv_blit(0, Y_FOOT);
}

/* ---- the knob cell */
enum { W_NONE, W_VALUE, W_CENTRE, W_CHOICE, W_SWITCH, W_ROUTE, W_READ };
typedef struct {
    uint8_t w, n, idx, hot, mod;                /* widget; CHOICE: n choices, idx the one; the knob in hand; mod arc */
    int16_t v, c, m0, m1;                       /* angles, Q4 of KNOB_ANG's units (0..3840): value, centre, mod span */
    const char *lab;
    char val[16];
} ui_knob_t;

static int16_t ang_of(int32_t v, int32_t lo, int32_t hi)
{
    if (hi <= lo) return 0;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int16_t)((v - lo) * 3840 / (hi - lo));
}

static void knob_param(uint32_t id, ui_knob_t *k)  /* a parameter's knob */
{
    const vs_param_info_t *pi = &VS_PARAMS[id];
    const int32_t v = pval(id), mx = pmax(id);
    k->hot = (uint8_t)ui_hot(id);
    k->mod = 0; k->n = 0; k->idx = 0; k->c = 0;
    k->lab = vs_param_label(id, pval(VS_P_FX_TYPE));
    ptext(id, k->val, sizeof k->val);
    k->v = ang_of(v, pi->min, mx);
    if (pi->kind == VS_K_SWITCH) k->w = W_SWITCH;
    else if (pi->kind == VS_K_ENUM) { k->w = W_CHOICE; k->n = (uint8_t)(mx - pi->min + 1); k->idx = (uint8_t)(v - pi->min); }
    else if (pi->kind == VS_K_SRC || pi->kind == VS_K_DST) k->w = W_ROUTE;
    else if (pi->kind == VS_K_SIGNED || id == VS_P_OSC2_PITCH || id == VS_P_OSC2_FINE || id == VS_P_OSC_TUNE) {
        k->w = W_CENTRE;
        k->c = ang_of(pi->kind == VS_K_SIGNED ? vs_param_centre(id) : 64, pi->min, mx);
    } else k->w = W_VALUE;
    if (id == VS_P_FLT_FREQ) {                  /* the span the filter envelope's AMOUNT adds */
        const int32_t a = pval(VS_P_FLT_AMOUNT) - 64;
        if (a) { k->mod = 1; k->m0 = k->v; k->m1 = ang_of(v + a * 2, 0, 127); }
    }
}

/* the cell: 60 x 90, the knob's 48 x 48 at (6, 2), the label at y 54, the value at 68 */
static void ui_cell(uint32_t c, const ui_knob_t *k)
{
    const uint16_t ink = k->hot ? T.accent : T.theme;
    const int32_t ox = 6, oy = 2;
    uint32_t key = hsh(hsh(hsh(hstr(hstr(2166136261u, k->lab), k->val), (uint32_t)k->w << 24 | (uint32_t)k->n << 16 | (uint32_t)k->idx << 8 | k->hot),
                           (uint32_t)(uint16_t)k->v << 16 | (uint16_t)k->c), (uint32_t)k->mod << 31 ^ (uint32_t)(uint16_t)k->m0 << 16 ^ (uint16_t)k->m1);
    int32_t x, y;
    if (key == k_cell[c]) return;
    k_cell[c] = key;
    cv_begin(60, H_CELL, T.bg);
    if (c) fill(0, 4, 1, 78, T.grid);
    if (k->w == W_NONE) { disc(30 * 16, 26 * 16, 32, T.line); cv_blit(60u * c, Y_CELL); return; }
    if (k->w == W_SWITCH) {
        const int on = k->v > 1920;
        rframe(11, 16, 38, 20, 10, on ? ink : T.line, on ? T.fill : T.bg);
        disc((on ? 39 : 21) * 16, 26 * 16, 6 * 16 + 8, on ? ink : T.dim);
    } else if (k->w == W_ROUTE || k->w == W_READ) {
        disc(18 * 16, 26 * 16, 4 * 16, k->w == W_ROUTE ? T.dim : ink);
        disc(42 * 16, 26 * 16, 4 * 16, ink);
        seg(22 * 16, 26 * 16, 38 * 16, 26 * 16, 24, T.dim);
    } else {
        const int32_t seg_q = k->w == W_CHOICE && k->n ? 3840 / k->n : 3840;
        const int32_t lo = k->w == W_CENTRE ? (k->c < k->v ? k->c : k->v) : 0, hi = k->w == W_CENTRE ? (k->c > k->v ? k->c : k->v) : k->v;
        int32_t a16, deg, sx, sy;
        for (y = 0; y < KNOB_N; y++)
            for (x = 0; x < KNOB_N; x++) {
                const uint32_t i = (uint32_t)(y * KNOB_N + x), a = KNOB_ANG[i];
                const uint32_t rc = KNOB_RING[i] >> 4, mc = KNOB_RING[i] & 15u, cc = KNOB_CAP[i] >> 4, rim = KNOB_CAP[i] & 15u;
                if (a > 240u) {
                    if (cc) { pxa(ox + x, oy + y, T.raised, cc); if (k->hot && rim) pxa(ox + x, oy + y, T.accent, rim); }
                    continue;
                }
                a16 = (int32_t)a * 16 + 8;
                if (rc) {
                    uint16_t col = T.line;
                    if (k->w == W_CHOICE) {
                        const int32_t s = a16 / seg_q, in = a16 - s * seg_q;
                        if (in >= 40 && in <= seg_q - 40) col = s == k->idx ? ink : T.line;
                        else col = T.bg;
                    } else {
                        /* lit lo..hi, its ends a pixel soft (the ring at r 18.5: a unit is 0.36 px) */
                        int32_t f = a16 < lo ? 16 - (lo - a16) / 3 : a16 > hi ? 16 - (a16 - hi) / 3 : 16;
                        if (k->w == W_VALUE && a16 < lo) f = 16;
                        if (hi == lo) f = 0;
                        if (f > 0) col = mix565(T.line, ink, (uint32_t)(f > 16 ? 16 : f) * 15u / 16u);
                    }
                    if (col != T.bg) pxa(ox + x, oy + y, col, rc);
                }
                if (mc && k->mod) {
                    const int32_t m0 = k->m0 < k->m1 ? k->m0 : k->m1, m1 = k->m0 < k->m1 ? k->m1 : k->m0;
                    if (a16 >= m0 && a16 <= m1) pxa(ox + x, oy + y, T.accent, mc);
                }
                if (cc) { pxa(ox + x, oy + y, T.raised, cc); if (k->hot && rim) pxa(ox + x, oy + y, T.accent, rim); }
            }
        if (k->w == W_CENTRE) seg((ox + 24) * 16, (oy + 1) * 16 + 4, (ox + 24) * 16, (oy + 3) * 16, 20, T.dim);
        /* the pointer: from r 4.5 to r 11 at the value's angle (-135 .. +135 degrees; UI_SIN: 256 a turn) */
        if (k->w == W_CHOICE && k->n) deg = -135 + (int32_t)(k->idx * 2u + 1u) * 135 / k->n;
        else deg = -135 + k->v * 270 / 3840;
        {
            const uint32_t ix = (uint32_t)((deg + 360) * 256 / 360) & 255u;
            sx = UI_SIN[ix]; sy = -UI_SIN[(ix + 64u) & 255u];   /* x = sin, y = -cos */
            seg((ox + 24) * 16 + sx * 72 / 16384, (oy + 24) * 16 + sy * 72 / 16384,
                (ox + 24) * 16 + sx * 176 / 16384, (oy + 24) * 16 + sy * 176 / 16384, 34, T.text);
        }
    }
    txt_fit(30, 54, 58, &FONT_LAB, k->lab, T.dim, 1);
    if (text_w(&FONT_VAL, k->val) <= 58) txt(30 - text_w(&FONT_VAL, k->val) / 2, 68, &FONT_VAL, k->val, k->hot ? T.accent : T.text);
    else {                                      /* a long value: the small face, tighter */
        const int32_t w = text_w(&FONT_LAB, k->val) - (int32_t)strlen_u(k->val);
        txt_track = -1;
        if (w <= 60) txt(30 - w / 2, 70, &FONT_LAB, k->val, k->hot ? T.accent : T.text);
        else txt_fit(30, 70, 60, &FONT_LAB, k->val, k->hot ? T.accent : T.text, 1);
        txt_track = 0;
    }
    cv_blit(60u * c, Y_CELL);
}
static void ui_cells_param(const uint8_t ids[4])
{
    uint32_t c;
    ui_knob_t k;
    for (c = 0; c < 4u; c++) {
        if (ids[c] == 0xFFu) { k.w = W_NONE; k.lab = ""; k.val[0] = 0; k.n = k.idx = k.hot = k.mod = 0; k.v = k.c = k.m0 = k.m1 = 0; }
        else knob_param(ids[c], &k);
        ui_cell(c, &k);
    }
}

/* ---- the panel's graphs */
enum { G_NONE, G_LFO1, G_LFO2, G_OSC1, G_SUB, G_OSC2, G_MIX, G_MENV, G_FENV, G_AENV, G_FILT, G_AMP, G_CHAIN, G_EFFECT,
       G_EQ, G_DELAY, G_CHORUS, G_VOICE, G_UNISON, G_CTRL, G_ARP, G_HOME, G_SAVE, G_GLOB1, G_GLOB2, G_GLOB3 };

static int32_t P(uint32_t id) { return pval(id); }
static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* waves: phase Q16 (0..65535) -> Q14 */
static int32_t w_sin(uint32_t ph)
{
    const uint32_t i = ph >> 8 & 255u, f = ph & 255u;
    return (UI_SIN[i] * (int32_t)(256u - f) + UI_SIN[(i + 1u) & 255u] * (int32_t)f) >> 8;
}
static int32_t w_saw(uint32_t ph) { return 16384 - (int32_t)((ph & 65535u) >> 1); }
static int32_t w_tri(uint32_t ph) { ph &= 65535u; return ph < 32768u ? -16384 + (int32_t)ph : 16384 - (int32_t)(ph - 32768u); }
static int32_t w_pulse(uint32_t ph, uint32_t pw) { return (ph & 65535u) < pw ? 16384 : -16384; }
static int32_t w_rnd(uint32_t k) { k = k * 2654435761u; k ^= k >> 15; return (int32_t)(k & 32767u) - 16384; }
/* OSC1: 0 SINE 1 SAW 2 PULSE 3 TRI; OSC2: 1 SAW 2 PULSE 3 TRI 4 NOISE; the SUB: 0 SINE 1 TRI 2 SAW 3 SQUARE */
static int32_t w_osc(uint32_t shape, uint32_t ph, uint32_t pw)
{
    switch (shape) {
    case 0: return w_sin(ph);
    case 1: return w_saw(ph);
    case 2: return w_pulse(ph, pw);
    case 3: return w_tri(ph);
    default: return w_rnd(ph >> 9);
    }
}
static uint32_t pw_of(void) { return 32768u + (uint32_t)P(VS_P_OSC_PW) * 30000u / 127u; }
/* OSC2's cycles a cycle of OSC1, Q8, from PITCH (64 the same; a semitone a step, drawn between 1/2 and 8) */
static uint32_t osc2_ratio(void)
{
    static const uint16_t SEMI[12] = {256, 271, 287, 304, 323, 342, 362, 384, 406, 431, 456, 483};
    int32_t s = P(VS_P_OSC2_PITCH) - 64, oct = 0;
    uint32_t r;
    while (s < 0) { s += 12; oct--; }
    while (s >= 12) { s -= 12; oct++; }
    r = SEMI[s];
    r = oct >= 0 ? r << (oct > 4 ? 4 : oct) : r >> (-oct > 4 ? 4 : -oct);
    return r < 128u ? 128u : r > 2048u ? 2048u : r;
}

static int16_t yv[PW + 1];                      /* a curve's rows, Q4 */
static void plot(uint32_t n, uint16_t c, int fillb, int32_t base, int dash)
{
    if (fillb) under(yv, n, PX0, base, T.fill);
    curve(yv, n, PX0, 24, c, dash);
}
static void grid(uint32_t cols, uint32_t rows)
{
    uint32_t i;
    for (i = 1; i < cols; i++) fill(PX0 + (int32_t)(PW * i / cols), PY0, 1, PY1 - PY0, T.grid);
    for (i = 0; i <= rows; i++) fill(PX0, PY0 + (int32_t)((PY1 - PY0) * i / rows), PW, 1, T.grid);
}
static void readout(void)                       /* the knob in hand, large, at the top left */
{
    char s[16];
    int32_t w;
    const char *lab;
    if (!ui_hot(big_id) || big_id >= VS_NPARAM) return;
    lab = vs_param_label(big_id, P(VS_P_FX_TYPE));
    ptext(big_id, s, sizeof s);
    w = text_w(&FONT_BIG, s);
    if (text_w(&FONT_LAB, lab) > w) w = text_w(&FONT_LAB, lab);
    rrect(PX0 - 2, PY0 - 2, w + 10, 44, 4, T.surf);
    txt(PX0 + 2, PY0, &FONT_LAB, lab, T.dim);
    txt(PX0 + 2, PY0 + 12, &FONT_BIG, s, T.accent);
}

static void g_env(uint32_t kind)
{
    /* MOD ENV: attack, decay to 0, AMOUNT signed (drawn from the middle); FILTER / AMP: A D S R */
    const int menv = kind == G_MENV;
    const uint32_t a0 = menv ? VS_P_MENV_A : kind == G_FENV ? VS_P_FENV_A : VS_P_AENV_A;
    const int32_t A = 2 + P(a0) * 48 / 127, D = 4 + P(a0 + 1u) * 50 / 127;
    const int32_t S = menv ? 0 : P(a0 + 2u), R = menv ? 0 : 4 + P(a0 + 3u) * 50 / 127;
    const int32_t HOLD = menv ? 0 : 44;
    const int32_t base = menv ? (PY0 + PY1) / 2 : PY1 - 1;
    const int32_t amp = menv ? (P(VS_P_MENV_AMOUNT) - 64) * 40 / 64 : PY1 - PY0 - 6;
    const int32_t ys = base * 16 - amp * S * 16 / 127;
    int32_t x, x1 = A, x2 = A + D, x3 = x2 + HOLD, x4 = x3 + R, i, hx0 = -1, hx1 = -1;
    const uint32_t hid = big_id;
    const int hot = ui_hot(hid) && hid >= a0 && hid <= a0 + (menv ? 1u : 3u);
    grid(8, 4);
    if (menv) fill(PX0, base, PW, 1, T.line);
    for (x = 0; x <= PW; x++) {
        int32_t y;
        if (x <= x1) y = base * 16 - amp * 16 * x / (x1 ? x1 : 1);
        else if (x <= x2) { const int32_t t = (x - x1) * 63 / (x2 - x1); y = ys + (base * 16 - amp * 16 - ys) * UI_EXP[t] / 16384; }
        else if (x <= x3) y = ys;
        else if (x <= x4) { const int32_t t = (x - x3) * 63 / (x4 - x3 ? x4 - x3 : 1); y = base * 16 + (ys - base * 16) * UI_EXP[t] / 16384; }
        else y = base * 16;
        yv[x] = (int16_t)y;
    }
    plot(PW + 1, T.theme, 1, base * 16 + 8, 0);
    if (hot) {                                  /* the segment in hand, in ACCENT */
        const uint32_t s = hid - a0;
        hx0 = s == 0u ? 0 : s == 1u ? x1 : s == 2u ? x2 : x3;
        hx1 = s == 0u ? x1 : s == 1u ? x2 : s == 2u ? x3 : x4;
        curve(yv + hx0, (uint32_t)(hx1 - hx0 + 1), PX0 + hx0, 32, T.accent, 0);
    }
    {   /* the corners, and the segments' letters */
        static const char *const L4[4] = {"A", "D", "S", "R"};
        const int32_t xs[5] = {0, x1, x2, x3, x4};
        for (i = 1; i < (menv ? 3 : 4); i++) disc((PX0 + xs[i]) * 16 + 8, yv[xs[i]], 40, T.text);
        for (i = 0; i < (menv ? 2 : 4); i++)
            if (xs[i + 1] - xs[i] >= 8)
                txt_fit(PX0 + (xs[i] + xs[i + 1]) / 2, PY1 - 12, 20, &FONT_LAB, L4[i], hot && (int32_t)(hid - a0) == i ? T.accent : T.dim, 1);
    }
}

static void g_filter(void)
{
    static const uint8_t SHAPE[5] = {0, 1, 2, 3, 1};    /* LP12 LP24 HP12 BP12 LPDL (drawn as LP24) */
    const uint32_t ty = (uint32_t)clampi(P(VS_P_FLT_TYPE), 0, 4), sh = SHAPE[ty];
    const int32_t rq = P(VS_P_FLT_RES) * 7 * 16 / 127, r0 = rq >> 4, fr = rq & 15, r1 = r0 < 7 ? r0 + 1 : 7;
    const int32_t y0 = PY0 + 22;                /* 0 dB; a pixel a dB */
    int32_t pass, x;
    char s[12];
    grid(8, 4);
    for (pass = 0; pass < 2; pass++) {          /* 0: where AMOUNT takes the cutoff (dashed), 1: the cutoff */
        const int32_t amt = P(VS_P_FLT_AMOUNT) - 64;
        const int32_t cx = P(VS_P_FLT_FREQ) * PW / 127 + (pass ? 0 : amt * 2 * PW / 127);
        if (!pass && !amt) continue;
        for (x = 0; x <= PW; x++) {
            const int32_t d = x - cx + 220, i = d >> 1;
            int32_t db;
            if (i < 0) db = UI_FLT[sh][r0][0];
            else if (i >= 220) db = UI_FLT[sh][r0][220];
            else {
                const int32_t a = UI_FLT[sh][r0][i] * (16 - fr) + UI_FLT[sh][r1][i] * fr;
                const int32_t b = UI_FLT[sh][r0][i + 1] * (16 - fr) + UI_FLT[sh][r1][i + 1] * fr;
                db = (d & 1) ? (a + b) / 2 : a;
                db /= 16;
            }
            yv[x] = (int16_t)clampi(y0 * 16 - db * 16, PY0 * 16, PY1 * 16);
        }
        if (pass) {
            plot(PW + 1, ui_hot(VS_P_FLT_FREQ) || ui_hot(VS_P_FLT_RES) ? T.accent : T.theme, 1, PY1 * 16, 0);
            fill(PX0 + clampi(cx, 0, PW - 1), PY0, 1, PY1 - PY0, T.dim);
        } else curve(yv, PW + 1, PX0, 16, T.accent, 1);
    }
    ptext(VS_P_FLT_TYPE, s, sizeof s);
    chip(PX0 + PW, PY0 - 4, s, T.theme, 2);
    if (P(VS_P_FLT_SAT)) { char t[12], *p = put_s(t, "SAT "); p = put_u(p, (uint32_t)P(VS_P_FLT_SAT)); *p = 0; chip(PX0 + PW - text_w(&FONT_LAB, s) - 14, PY0 - 4, t, T.dim, 2); }
}

static void g_lfo(uint32_t n)
{
    const uint32_t wave = (uint32_t)P(n ? VS_P_LFO2_TYPE : VS_P_LFO1_TYPE), depth = (uint32_t)P(n ? VS_P_LFO2_DEPTH : VS_P_LFO1_DEPTH);
    const int on = P(n ? VS_P_LFO2_ON : VS_P_LFO1_ON) && depth;
    const int32_t mid = (PY0 + PY1) / 2, amp = 10 + (int32_t)depth * 30 / 127;
    const uint16_t c = on ? T.theme : T.dim;
    char s[16];
    int32_t x;
    grid(8, 4);
    fill(PX0, mid, PW, 1, T.line);
    for (x = 0; x <= PW; x++) {
        const uint32_t ph = (uint32_t)x * 65535u / PW;
        int32_t v;
        switch (wave) {
        case 0: v = w_saw(ph); break;
        case 1: v = w_tri(ph); break;
        case 2: v = w_pulse(ph, 32768u); break;
        case 3: v = w_rnd(ph >> 13); break;             /* S&H: 8 steps a cycle */
        case 4: { const uint32_t k = ph >> 13, f = (ph >> 5) & 255u;   /* smooth random: the steps, eased */
                  const int32_t e = (16384 - w_sin(f * 128u + 16384u)) / 2;
                  v = w_rnd(k) + (w_rnd(k + 1u) - w_rnd(k)) * e / 16384; break; }
        default: v = w_sin(ph); break;
        }
        yv[x] = (int16_t)(mid * 16 - v * amp * 16 / 16384);
    }
    plot(PW + 1, c, on, mid * 16 + 8, 0);
    if (on) {                                   /* the LFO's phase now (the engine's; it runs free) */
        const uint32_t ph = eng.lfo[n].ph >> 16;
        const int32_t px = (int32_t)(ph * PW / 65535u);
        fill(PX0 + px, PY0, 1, PY1 - PY0, T.dim);
        disc((PX0 + px) * 16 + 8, yv[px], 56, T.accent);
    }
    ptext(n ? VS_P_LFO2_DEST : VS_P_LFO1_DEST, s, sizeof s);
    x = chip(PX0 + PW, PY0 - 4, s, c, 2);
    ptext(n ? VS_P_LFO2_SPEED : VS_P_LFO1_SPEED, s, sizeof s);
    chip(PX0 + PW - x - 4, PY0 - 4, s, T.dim, 2);
}

static void g_osc(uint32_t kind)
{
    const int32_t mid = (PY0 + PY1) / 2 + 3, amp = 30;
    const uint32_t pw = pw_of(), s1 = (uint32_t)P(VS_P_OSC1_SHAPE), s2 = (uint32_t)P(VS_P_OSC2_SHAPE);
    const uint32_t r2 = osc2_ratio(), cyc = 2u;    /* two cycles of OSC1 */
    const int32_t det = P(VS_P_OSC1_DETUNE), fm = P(VS_P_OSC1_FM), mix = P(VS_P_OSC_MIX);
    int32_t x, j;
    char s[12];
    grid(8, 4);
    fill(PX0, mid, PW, 1, T.line);
    if (kind == G_OSC1 && det) {                /* DETUNE's copies, drifting apart along the screen */
        static const int8_t CP[6] = {-5, -3, -1, 1, 3, 5};
        for (j = 0; j < 6; j++) {
            for (x = 0; x <= PW; x++) {
                const uint32_t ph = (uint32_t)(x * (int32_t)cyc * 65536 / PW + CP[j] * det * x / 4);
                yv[x] = (int16_t)(mid * 16 - w_osc(s1, ph, pw) * amp * 16 / 16384 * 4 / 5);
            }
            curve(yv, PW + 1, PX0, 16, mix565(T.surf, T.theme, 6), 0);
        }
    }
    if (kind == G_SUB) {                        /* OSC1, faint, under the SUB */
        for (x = 0; x <= PW; x++) yv[x] = (int16_t)(mid * 16 - w_osc(s1, (uint32_t)x * cyc * 65536u / PW, pw) * amp * 16 / 16384);
        curve(yv, PW + 1, PX0, 16, mix565(T.surf, T.theme, 6), 0);
    }
    for (x = 0; x <= PW; x++) {
        uint32_t ph = (uint32_t)x * cyc * 65536u / PW;
        int32_t v;
        if (fm && kind != G_OSC2) ph += (uint32_t)(w_sin(ph * r2 >> 8) * fm / 127 / 2);   /* FM from OSC2 */
        if (kind == G_SUB) {                    /* the SUB at OSC1's note or an octave down, over OSC1 faint */
            const uint32_t sp = P(VS_P_SUB_OCT) ? ph >> 1 : ph, ss = (uint32_t)P(VS_P_SUB_SHAPE);
            const int32_t g = 8 + P(VS_P_SUB_GAIN) * 120 / 127;
            v = (ss == 0u ? w_sin(sp) : ss == 1u ? w_tri(sp) : ss == 2u ? w_saw(sp) : w_pulse(sp, 32768u)) * g / 128;
        } else if (kind == G_OSC2) {
            v = w_osc(s2, ph * r2 >> 8, pw);
        } else if (kind == G_MIX) {             /* the mix: OSC1 and OSC2 (synced, or ringed) by MIX */
            const uint32_t p2 = P(VS_P_OSC2_SYNC) ? (ph & 65535u) * r2 >> 8 : ph * r2 >> 8;
            int32_t o2 = w_osc(s2, p2, pw);
            const int32_t o1 = w_osc(s1, ph, pw);
            if (P(VS_P_OSC2_RING)) o2 = o1 * o2 / 16384;
            v = (o1 * (127 - mix) + o2 * mix) / 127;
        } else v = w_osc(s1, ph, pw);
        yv[x] = (int16_t)(mid * 16 - v * amp * 16 / 16384);
    }
    plot(PW + 1, T.theme, 0, 0, 0);
    if (kind == G_OSC1) {
        ptext(VS_P_OSC1_SHAPE, s, sizeof s);
        x = chip(PX0 + PW, PY0 - 4, s, T.theme, 2);
        if (det) chip(PX0 + PW - x - 4, PY0 - 4, "DETUNE x8", T.dim, 2);
    } else if (kind == G_OSC2) {
        ptext(VS_P_OSC2_SHAPE, s, sizeof s);
        x = chip(PX0 + PW, PY0 - 4, s, T.theme, 2);
        ptext(VS_P_OSC2_PITCH, s, sizeof s);
        chip(PX0 + PW - x - 4, PY0 - 4, s, T.dim, 2);
    } else if (kind == G_SUB) {
        ptext(VS_P_SUB_SHAPE, s, sizeof s);
        chip(PX0 + PW, PY0 - 4, s, T.theme, 2);
    } else {
        x = 0;
        if (P(VS_P_OSC2_RING)) x += chip(PX0 + PW - x, PY0 - 4, "RING", T.theme, 2) + 4;
        if (P(VS_P_OSC2_SYNC)) x += chip(PX0 + PW - x, PY0 - 4, "SYNC", T.theme, 2) + 4;
        if (P(VS_P_MENV_ON)) chip(PX0 + PW - x, PY0 - 4, "M.ENV", T.theme, 2);
    }
}

static void g_amp(void)
{
    /* the level against velocity (GAIN, VEL), and PAN below */
    const int32_t gain = P(VS_P_AMP_GAIN), vel = P(VS_P_AMP_VEL), pan = P(VS_P_PAN);
    const int32_t top = PY0 + 14, bot = PY1 - 22, px = PX0 + 20 + (pan * (PW - 40)) / 127;
    int32_t x;
    grid(8, 4);
    for (x = 0; x <= PW; x++) {
        const int32_t lv = gain * (127 * 127 - vel * (127 - x * 127 / PW)) / (127 * 127);
        yv[x] = (int16_t)(bot * 16 - lv * (bot - top) * 16 / 127);
    }
    plot(PW + 1, T.theme, 1, bot * 16 + 8, 0);
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "LEVEL BY VELOCITY", T.dim);
    fill(PX0 + 20, PY1 - 8, PW - 40, 2, T.line);
    fill(PX0 + 20 + (PW - 40) / 2, PY1 - 12, 1, 10, T.dim);
    txt(PX0 + 6, PY1 - 13, &FONT_LAB, "L", T.dim);
    txt(PX0 + PW - 12, PY1 - 13, &FONT_LAB, "R", T.dim);
    disc(px * 16 + 8, (PY1 - 7) * 16, 64, ui_hot(VS_P_PAN) ? T.accent : T.theme);
}

static void g_chain(void)
{
    static const char *const NM[4] = {"EFFECT", "EQ", "DELAY", "CHORUS"};
    const int on[4] = {P(VS_P_FX_ON) != 0, 1, P(VS_P_DLY_ON) != 0, P(VS_P_CHO_ON) != 0};
    const uint32_t sub[4] = {VS_P_FX_TYPE, 0xFFu, VS_P_DLY_TIME, VS_P_CHO_TYPE};
    const int32_t y = PY0 + 20, w = 51, gap = 5;
    int32_t i;
    char s[12];
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "SIGNAL PATH", T.dim);
    for (i = 0; i < 4; i++) {
        const int32_t bx = PX0 + 4 + i * (w + gap);
        if (i) seg((bx - gap + 1) * 16, (y + 17) * 16, (bx - 1) * 16, (y + 17) * 16, 20, T.dim);
        rframe(bx, y, w, 34, 4, on[i] ? T.theme : T.line, on[i] ? T.fill : T.surf);
        txt_fit(bx + w / 2, y + 4, w - 2, &FONT_LAB, NM[i], on[i] ? T.text : T.dim, 1);
        ptext(sub[i] != 0xFFu ? sub[i] : VS_P_EQ_TONE, s, sizeof s);
        txt_fit(bx + w / 2, y + 18, w - 2, &FONT_LAB, s, on[i] ? T.theme : T.dim, 1);
    }
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "EQ IS ALWAYS IN THE PATH", T.dim);
}

static void g_effect(void)
{
    /* the type's shape: AD1 AD2 DD a transfer curve; DECI a stepped sine; RM a ring-modulated sine; COMP the level
     * in to out; PH1..4 a comb of notches */
    const int32_t ty = P(VS_P_FX_TYPE), c1 = P(VS_P_FX_CTRL1), c2 = P(VS_P_FX_CTRL2);
    const int on = P(VS_P_FX_ON) != 0;
    const uint16_t col = on ? T.theme : T.dim;
    const int32_t mid = (PY0 + PY1) / 2, amp = 38;
    int32_t x;
    char s[12];
    grid(8, 4);
    fill(PX0, mid, PW, 1, T.line);
    for (x = 0; x <= PW; x++) {
        const int32_t in = x * 32768 / PW - 16384;  /* -1..1, Q14 */
        int32_t v;
        if (ty <= 2) {
            const int32_t a = 1 + c1 / 4;              /* drive */
            const int32_t ax = in < 0 ? -in : in;
            v = ty == 2 ? clampi(in * a, -16384, 16384) : (1 + a) * in * 1024 / ((16384 + a * ax) >> 4);   /* (1 + a) x / (1 + a |x|) */
            if (v > 16384) v = 16384;
            if (v < -16384) v = -16384;
        } else if (ty == 3) {
            const int32_t hold = 2 + c1 / 6, lev = 2 + (127 - c2) / 16;
            const int32_t xs = x / hold * hold;
            v = w_sin((uint32_t)(xs * 2 * 65536 / PW)) * lev / 16384 * 16384 / lev;
        } else if (ty == 4) {
            v = w_sin((uint32_t)(x * 2 * 65536 / PW)) * w_sin((uint32_t)(x * (2 + c1 / 8) * 65536 / PW)) / 16384;
        } else if (ty == 5) {
            const int32_t th = 4000 + (127 - c1) * 90, ratio = 1 + c2 / 16;
            const int32_t l = x * 16384 / PW;
            v = (l < th ? l : th + (l - th) / ratio) * 2 - 16384;
        } else {
            const int32_t st = ty - 5;                 /* PH1..4: 1..4 stages, two notches each */
            const int32_t c = w_sin((uint32_t)(x * st * 2 * 65536 / PW + c1 * 256));
            v = 16384 - (c < 0 ? -c : c) * (16 + c2 / 8) / 32 * 2;
        }
        yv[x] = (int16_t)(mid * 16 - v * amp * 16 / 16384);
    }
    if (ty <= 2 || ty == 5) seg(PX0 * 16, (mid + amp) * 16, (PX0 + PW) * 16, (mid - amp) * 16, 12, T.line);
    plot(PW + 1, col, 0, 0, 0);
    ptext(VS_P_FX_TYPE, s, sizeof s);
    x = chip(PX0 + PW, PY0 - 4, s, col, 2);
    if (!on) chip(PX0 + PW - x - 4, PY0 - 4, "OFF", T.dim, 2);
}

static void g_eq(void)
{
    const int32_t tone = P(VS_P_EQ_TONE) - 64, xf = P(VS_P_EQ_FREQ) * PW / 127, g = (P(VS_P_EQ_LEVEL) - 64) * 30 / 64;
    const int32_t q = 2 + P(VS_P_EQ_Q) / 6, mid = (PY0 + PY1) / 2;
    int32_t x;
    grid(8, 4);
    fill(PX0, mid, PW, 1, T.line);
    for (x = 0; x <= PW; x++) {
        const int32_t dx = (x - xf) * q / 16;
        const int32_t bell = g * 16 * 256 / (256 + dx * dx);          /* Q4 rows */
        const int32_t tilt = -tone * 12 * 16 / 64 * (x > 110 ? x - 110 : 0) / 110;   /* TONE: a low-pass, less of the top */
        yv[x] = (int16_t)clampi(mid * 16 - bell - tilt, PY0 * 16, PY1 * 16);
    }
    plot(PW + 1, T.theme, 1, mid * 16 + 8, 0);
}

static void g_delay(void)
{
    const int32_t ty = P(VS_P_DLY_TYPE), tm = P(VS_P_DLY_TIME), fb = P(VS_P_DLY_FBK), wet = P(VS_P_DLY_DRYWET);
    const int32_t spread = (P(VS_P_DLY_SPREAD) - 64) / 8;
    const int on = P(VS_P_DLY_ON) != 0;
    const int32_t mid = (PY0 + PY1) / 2, H = mid - PY0 - 6, dx = 6 + tm * 4;
    int32_t i, g = 127;
    char s[12];
    grid(8, 2);
    txt(PX0 + 2, PY0 + 1, &FONT_LAB, "L", T.dim);
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "R", T.dim);
    fill(PX0 + 12, mid - 2 - H, 4, H, T.text);
    fill(PX0 + 12, mid + 2, 4, H, T.text);
    for (i = 1; i < 12; i++) {
        const int32_t x = PX0 + 12 + i * dx, h = H * g / 127 * (16 + wet) / 143;
        const uint16_t c = on ? (i < 5 ? T.theme : T.rel) : T.dim;
        const int left = ty == 0 || (ty == 1 ? (i & 1) == 0 : (i & 1) == 1);
        if (x + 4 > PX0 + PW) break;
        if (ty == 0 || left) fill(x, mid - 2 - h, 4, h, c);
        if (ty == 0 || !left) fill(x + spread, mid + 2, 4, h, c);
        g = g * (fb + 8) / 135;
        if (h < 1) break;
    }
    ptext(VS_P_DLY_TYPE, s, sizeof s);
    i = chip(PX0 + PW, PY0 - 4, s, on ? T.theme : T.dim, 2);
    ptext(VS_P_DLY_TIME, s, sizeof s);
    chip(PX0 + PW - i - 4, PY0 - 4, on ? s : "OFF", T.dim, 2);
}

static void g_chorus(void)
{
    const int32_t tm = P(VS_P_CHO_TIME), dp = P(VS_P_CHO_DEPTH), rt = P(VS_P_CHO_RATE);
    const int on = P(VS_P_CHO_ON) != 0;
    const int32_t base = PY1 - 8 - tm * (PY1 - PY0 - 30) / 127, amp = 2 + dp * 20 / 127;
    const uint32_t cyc = 1u + (uint32_t)rt * 5u / 127u;
    int32_t x, k;
    char s[12];
    grid(8, 4);
    for (k = 1; k >= 0; k--) {                  /* left and right, half a cycle apart */
        for (x = 0; x <= PW; x++) yv[x] = (int16_t)(base * 16 - w_sin((uint32_t)x * cyc * 65536u / PW + (uint32_t)k * 32768u) * amp * 16 / 16384);
        curve(yv, PW + 1, PX0, 24, !on ? T.dim : k ? T.rel : T.theme, 0);
    }
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "DELAY TIME", T.dim);
    ptext(VS_P_CHO_TYPE, s, sizeof s);
    x = chip(PX0 + PW, PY0 - 4, s, on ? T.theme : T.dim, 2);
    if (!on) chip(PX0 + PW - x - 4, PY0 - 4, "OFF", T.dim, 2);
}

static void g_voice(void)
{
    /* the voices a note takes (POLY, MONO, LEGATO), and the glide PORTAMENTO draws between two notes */
    const int32_t mode = P(VS_P_PLAY_MODE), poly = mode ? 1 : P(VS_P_POLY), pt = P(VS_P_PORTA_TIME);
    const int32_t nv = (int32_t)(eng.nv < VS_NV ? eng.nv : VS_NV);
    int32_t k, x;
    char s[12];
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "VOICES", T.dim);
    for (k = 0; k < (int32_t)VS_NV; k++) {
        const int lit = k < poly && k < nv;
        rframe(PX0 + 2 + k * 13, PY0 + 14, 10, 22, 2, lit ? T.theme : T.line, lit ? T.fill : T.surf);
    }
    txt(PX0 + 112, PY0 - 2, &FONT_LAB, "GLIDE", T.dim);
    {
        const int32_t x0 = PX0 + 112, w = PW - 112, lo = PY1 - 10, hi = PY0 + 16, gw = 2 + pt * (w - 24) / 127;
        for (x = 0; x <= w; x++) {
            int32_t y;
            if (x < 12) y = lo * 16;
            else if (x < 12 + gw) { const int32_t t = (x - 12) * 63 / gw; y = hi * 16 + (lo - hi) * 16 * UI_EXP[t] / 16384; }
            else y = hi * 16;
            yv[x] = (int16_t)y;
        }
        curve(yv, (uint32_t)w + 1u, x0, 24, pt ? T.theme : T.dim, 0);
    }
    ptext(VS_P_PLAY_MODE, s, sizeof s);
    x = chip(PX0 + PW, PY1 - 14, s, T.theme, 2);
    if (P(VS_P_PORTA_AUTO)) chip(PX0 + PW - x - 4, PY1 - 14, "AUTO", T.dim, 2);
}

static void g_unison(void)
{
    /* the stack: VOICES lines, spaced by DETUNE, shaded by their place in SPREAD */
    const int on = P(VS_P_UNI_ON) != 0;
    const int32_t n = P(VS_P_UNI_VOICES), det = P(VS_P_UNI_DETUNE), spr = P(VS_P_UNI_PAN) - 64;
    const int32_t cx = PX0 + PW / 2, sp = 3 + det / 5;
    int32_t k;
    grid(8, 4);
    fill(cx, PY0, 1, PY1 - PY0, T.line);
    for (k = 0; k < n; k++) {
        const int32_t off = (2 * k - (n - 1)) * sp / 2, x = cx + off;
        const int32_t h = 30 + (spr > 0 ? spr : -spr) * (k & 1 ? 1 : -1) / 6;
        seg(x * 16, (PY0 + 46 - h) * 16, x * 16, (PY0 + 46 + h) * 16, 40, on ? (k & 1 ? T.rel : T.theme) : T.dim);
    }
    txt(PX0 + 2, PY1 - 12, &FONT_LAB, "PITCH", T.dim);
    if (!on) chip(PX0 + PW, PY0 - 4, "UNISON OFF", T.dim, 2);
}

static void g_ctrl(void)
{
    /* the two routes: SRC -> ASSIGN, SENS; the bend's range */
    char a[16], b[16], c[16];
    uint32_t r;
    for (r = 0; r < 2u; r++) {
        const int32_t y = PY0 + 2 + (int32_t)r * 30;
        ptext(r ? VS_P_CTRL_SRC2 : VS_P_CTRL_SRC1, a, sizeof a);
        ptext(r ? VS_P_CTRL_DST2 : VS_P_CTRL_DST1, b, sizeof b);
        ptext(r ? VS_P_CTRL_SENS2 : VS_P_CTRL_SENS1, c, sizeof c);
        rrect(PX0, y, PW, 24, 4, T.raised);
        txt_fit(PX0 + 8, y + 4, 64, &FONT_VAL, a, T.text, 0);
        seg((PX0 + 78) * 16, (y + 12) * 16, (PX0 + 96) * 16, (y + 12) * 16, 20, T.theme);
        seg((PX0 + 91) * 16, (y + 8) * 16, (PX0 + 96) * 16, (y + 12) * 16, 20, T.theme);
        seg((PX0 + 91) * 16, (y + 16) * 16, (PX0 + 96) * 16, (y + 12) * 16, 20, T.theme);
        txt_fit(PX0 + 104, y + 4, 70, &FONT_VAL, b, T.theme, 0);
        txt_fit(PX0 + PW - 8, y + 4, 40, &FONT_VAL, c, T.dim, 2);
    }
    ptext(VS_P_BEND_RANGE, a, sizeof a);
    r = (uint32_t)txt(PX0 + 2, PY1 - 14, &FONT_LAB, "BEND RANGE", T.dim);
    txt((int32_t)r + 8, PY1 - 16, &FONT_VAL, a, T.text);
}

static void g_arp(void)
{
    /* sixteen steps of the pattern: TYPE (UP&DOWN, UP, DOWN, RANDOM) over RANGE octaves of a three-note chord;
     * a step's length: GATE */
    const int32_t ty = P(VS_P_ARP_TYPE), oct = P(VS_P_ARP_RANGE) + 1, gate = P(VS_P_ARP_GATE);
    const int on = P(VS_P_ARP_ON) != 0;
    const int32_t n = 3 * oct, sw = PW / 16, hh = (PY1 - PY0 - 8) / (n > 1 ? n : 1);
    int32_t i;
    grid(16, 4);
    for (i = 0; i < 16; i++) {
        int32_t st;
        if (ty == 2) st = i % n;
        else if (ty == 3) st = n - 1 - i % n;
        else if (ty == 4) st = (int32_t)((uint32_t)w_rnd((uint32_t)i + 7u) & 1023u) % n;
        else { const int32_t m = n > 1 ? 2 * n - 2 : 1, j = i % m; st = j < n ? j : m - j; }
        fill(PX0 + i * sw + 1, PY1 - 4 - (st + 1) * hh, 1 + (sw - 2) * (gate + 8) / 135, hh - 2 > 2 ? hh - 2 : 2, on ? T.theme : T.dim);
    }
    if (!on) chip(PX0 + PW, PY0 - 4, "ARP OFF (TAP ARP)", T.dim, 2);
}

/* HOME: the program, the play mode, the scope */


static void g_home(void)
{
    char s[24], *p;
    int32_t x, pk = 1;
    const uint16_t pc = ui_patch_colour(eng.patch.color);
    rrect(PX0 - 4, PY0 - 6, 8, 8, 2, pc);
    txt_fit(PX0 + 8, PY0 - 10, 150, &FONT_BIG, prog_name[0] ? prog_name : "-", edited ? T.warn : T.text, 0);
    p = put_s(s, vs_prog_bank_name(prog.bank)); p = put_s(p, "  /  "); p = put_3(p, slot + 1u); *p = 0;
    txt(PX0 + 8, PY0 + 16, &FONT_LAB, s, T.dim);
    if (edited) txt(PX0 + 8 + text_w(&FONT_LAB, s) + 8, PY0 + 16, &FONT_LAB, "EDITED", T.warn);
    ptext(VS_P_PLAY_MODE, s, sizeof s);
    if (P(VS_P_PLAY_MODE) == 0) { p = put_s(s, "POLY "); p = put_u(p, (uint32_t)(P(VS_P_POLY) < (int32_t)eng.nv ? P(VS_P_POLY) : (int32_t)eng.nv)); *p = 0; }
    x = chip(PX0 + PW, PY0 - 6, s, T.theme, 2);
    if (P(VS_P_UNI_ON)) chip(PX0 + PW - x - 4, PY0 - 6, "UNISON", T.theme, 2);
    x = chip(PX0 + PW, PY0 + 12, P(VS_P_ARP_ON) ? "ARP ON" : "ARP OFF", P(VS_P_ARP_ON) ? T.theme : T.dim, 2);
    if (set.oct) { p = put_s(s, "OCT "); *p++ = set.oct < 0 ? '-' : '+'; p = put_u(p, (uint32_t)(set.oct < 0 ? -set.oct : set.oct)); *p = 0; chip(PX0 + PW - x - 4, PY0 + 12, s, T.accent, 2); }
    rrect(4, PY0 + 34, 232, PY1 + 6 - (PY0 + 34), 4, T.raised);
    {
        const int32_t mid = PY0 + 34 + (PY1 + 6 - (PY0 + 34)) / 2, amp = 22;
        fill(PX0, mid, PW, 1, T.line);
        for (x = 0; x < PW; x++) { const int32_t v = scope[x] < 0 ? -scope[x] : scope[x]; if (v > pk) pk = v; }
        if (pk < 2048) pk = 2048;                 /* a gain up to x16, so a quiet patch still shows its shape */
        for (x = 0; x < PW; x++) yv[x] = (int16_t)(mid * 16 - scope[x] * amp * 16 / pk);
        curve(yv, PW, PX0, 24, T.theme, 0);
    }
}

static void g_save(void)
{
    char s[40], *p;
    uint32_t k;
    txt_fit(120, PY0 - 4, 220, &FONT_BIG, prog_name[0] ? prog_name : "-", T.text, 1);
    p = put_s(s, "TO  "); p = put_s(p, vs_prog_bank_name(save_bank)); *p++ = ' '; p = put_3(p, save_slot + 1u); *p = 0;
    txt_fit(120, PY0 + 28, 220, &FONT_TTL, s, T.theme, 1);
    if (save_bank == prog.bank) {
        p = s;
        if (vs_prog_empty(&prog, save_slot)) p = put_s(p, "the slot is empty");
        else { p = put_s(p, "replaces  "); for (k = 0; k < VS_NAME; k++) *p++ = (char)prog.r[save_slot].b[k]; }
        *p = 0;
    } else { p = put_s(s, "the bank loads on save"); *p = 0; }
    txt_fit(120, PY0 + 52, 220, &FONT_VAL, s, T.dim, 1);
}

static void g_glob1(void)
{
    char s[24], *p;
    const int ext = clk_t && fm1_ms - clk_t < 1000u;
    int32_t k;
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "VOICES IN USE", T.dim);
    for (k = 0; k < (int32_t)VS_NV; k++) {
        const int lit = k < (int32_t)set.voices;
        rframe(PX0 + 2 + k * 13, PY0 + 14, 10, 22, 2, lit ? T.theme : T.line, lit ? T.fill : T.surf);
    }
    p = put_u(s, eng.bpm_q8 >> 8); p = put_s(p, " BPM"); *p = 0;
    txt(PX0 + 120, PY0 - 2, &FONT_LAB, ext ? "TEMPO: MIDI CLOCK" : "TEMPO", T.dim);
    txt(PX0 + 120, PY0 + 12, &FONT_BIG, s, ext ? T.accent : T.text);
    p = put_s(s, "MIDI IN: "); p = set.ch ? put_s(put_u(put_s(p, "CHANNEL "), set.ch), "") : put_s(p, "ANY CHANNEL"); *p = 0;
    txt(PX0 + 2, PY1 - 14, &FONT_LAB, s, T.dim);
}

static void g_glob2(void)
{
    /* a swatch a theme: its ground, its surface, its two colours; the one in use framed */
    uint32_t k;
    for (k = 0; k < UI_NTHEME; k++) {
        const ui_theme_def_t *d = &UI_THEMES[k];
        const int32_t x = PX0 + (int32_t)(k % 3u) * 74, y = PY0 - 4 + (int32_t)(k / 3u) * 46;
        if (k == set.theme) rrect(x - 2, y - 2, 70, 42, 5, T.accent);
        rrect(x, y, 66, 38, 4, d->bg);
        rrect(x + 4, y + 4, 58, 16, 3, d->surf);
        disc((x + 14) * 16, (y + 12) * 16, 72, d->theme);
        disc((x + 30) * 16, (y + 12) * 16, 72, d->accent);
        txt_fit(x + 33, y + 23, 62, &FONT_LAB, d->name, d->text, 1);
    }
}

/* GLOBAL 3/3, DIAG: what the sound costs (the ISR's buffer, its voices and its bus), the cores, the voices the
 * overload guard dropped, the tempo's source */
static uint32_t dg_avg, dg_peak;                /* (ui_draw: the CPU average and the 3 s peak, % of the buffer) */
static void g_glob3(void)
{
    char s[32], *p;
    const int ext = clk_t && fm1_ms - clk_t < 1000u;
    const uint16_t pc = dg_peak >= 90u ? T.warn : T.accent;
    txt(PX0 + 2, PY0 - 2, &FONT_LAB, "CPU", T.dim);
    p = put_u(s, dg_avg); *p++ = '%'; *p = 0;
    txt(PX0 + 2, PY0 + 12, &FONT_BIG, s, T.text);
    txt(PX0 + 80, PY0 - 2, &FONT_LAB, "PEAK (3 S)", T.dim);
    p = put_u(s, dg_peak); *p++ = '%'; *p = 0;
    txt(PX0 + 80, PY0 + 12, &FONT_BIG, s, pc);
    p = put_s(s, "VOICES "); p = put_u(p, pc_voice); p = put_s(p, "%   BUS "); p = put_u(p, pc_bus); *p++ = '%'; *p = 0;
    txt(PX0 + 2, PY0 + 44, &FONT_LAB, s, T.text);
    p = put_s(s, vs_core1_online() ? "2 CORES" : "1 CORE"); p = put_s(p, "   DROPPED "); p = put_u(p, eng.shed); *p = 0;
    txt(PX0 + 2, PY0 + 58, &FONT_LAB, s, eng.shed ? T.warn : T.dim);
    p = put_u(s, eng.bpm_q8 >> 8); p = put_s(p, ext ? " BPM, MIDI CLOCK" : " BPM, INTERNAL"); *p = 0;
    txt(PX0 + 2, PY1 - 14, &FONT_LAB, s, T.dim);
}

static void ui_panel_render(uint32_t kind)
{
    rrect(4, Y_PANEL, 232, H_PANEL, 4, T.surf);
    switch (kind) {
    case G_LFO1: g_lfo(0); break;
    case G_LFO2: g_lfo(1); break;
    case G_OSC1: case G_SUB: case G_OSC2: case G_MIX: g_osc(kind); break;
    case G_MENV: case G_FENV: case G_AENV: g_env(kind); break;
    case G_FILT: g_filter(); break;
    case G_AMP: g_amp(); break;
    case G_CHAIN: g_chain(); break;
    case G_EFFECT: g_effect(); break;
    case G_EQ: g_eq(); break;
    case G_DELAY: g_delay(); break;
    case G_CHORUS: g_chorus(); break;
    case G_VOICE: g_voice(); break;
    case G_UNISON: g_unison(); break;
    case G_CTRL: g_ctrl(); break;
    case G_ARP: g_arp(); break;
    case G_HOME: g_home(); break;
    case G_SAVE: g_save(); break;
    case G_GLOB1: g_glob1(); break;
    case G_GLOB2: g_glob2(); break;
    case G_GLOB3: g_glob3(); break;
    default: break;
    }
    if (kind != G_HOME && kind != G_SAVE && kind != G_CHAIN && kind != G_CTRL && kind < G_GLOB1) readout();   /* (the path and the routes say it) */
}

/* the panel, in two strips of 52 rows, when its key changes (min_ms apart, unless the screen is new) */
static void ui_panel(uint32_t kind, uint32_t key, uint32_t min_ms)
{
    uint32_t s;
    key = hsh(key, kind);
    if (key == k_panel || (k_panel != 0u && fm1_ms - t_panel < min_ms)) return;
    k_panel = key;
    t_panel = fm1_ms;
    for (s = 0; s < 2u; s++) {
        cv_begin(240, H_PANEL / 2, T.bg);
        cv_oy = -(int32_t)(Y_PANEL + s * (H_PANEL / 2u));
        ui_panel_render(kind);
        cv_oy = 0;
        cv_blit(0, Y_PANEL + s * (H_PANEL / 2u));
    }
}

static uint32_t ui_patch_key(void)
{
    uint32_t i, h = 2166136261u;
    for (i = 0; i < VS_NPARAM; i++) h = hsh(h, (uint32_t)eng.patch.v[i]);
    return hsh(h, ui_hot(big_id) ? big_id : 0xFFu);
}

static uint32_t page_kind(uint32_t p)
{
    const uint32_t id = VS_PAGES[p].id[0];
    switch (VS_PARAMS[id].sec) {
    case VS_SEC_LFO1: return G_LFO1;
    case VS_SEC_LFO2: return G_LFO2;
    case VS_SEC_OSC1: return G_OSC1;
    case VS_SEC_SUB: return G_SUB;
    case VS_SEC_OSC2: return G_OSC2;
    case VS_SEC_OSCC: return G_MIX;
    case VS_SEC_MENV: return G_MENV;
    case VS_SEC_FENV: return G_FENV;
    case VS_SEC_AENV: return G_AENV;
    case VS_SEC_FILT: return G_FILT;
    case VS_SEC_AMP: return G_AMP;
    case VS_SEC_FX: return id == VS_P_FX_ON ? G_CHAIN : G_EFFECT;
    case VS_SEC_EQ: return G_EQ;
    case VS_SEC_DELAY: return G_DELAY;
    case VS_SEC_CHORUS: return G_CHORUS;
    case VS_SEC_VOICE: return G_VOICE;
    case VS_SEC_UNISON: return G_UNISON;
    case VS_SEC_CTRL: return G_CTRL;
    case VS_SEC_ARP: return G_ARP;
    default: return G_NONE;
    }
}

static const char *const BTN_NAME[7] = {"LFO", "EDIT", "ENV", "SEL", "FX", "GLO", "ARP"};
static uint32_t gpage;                          /* GLOBAL: 0 MIDI CH, VOICES, TEMPO, CLOCK; 1 THEME, COLOUR; 2 DIAG */

/* the footer of a patch page: bank and slot, the name, the tempo (or the transfer, or the bank unsaved) */
/* the footer, every screen: the patch's name only (edited: in WARN); on the right only what needs seeing now, a
 * transfer or a bank not yet written. The meters are on GLOBAL 3/3 (Kris 14 Nov) */
static void ui_foot_prog(void)
{
    char r[24], *p = r;
    uint16_t rc = T.dim;
    if (xfer_t && fm1_ms - xfer_t < 2000u) { p = put_s(r, "XFER "); p = put_u(p, xfer.rx_n); *p++ = '/'; p = put_u(p, xfer.tx_n); rc = T.accent; }
    else if (prog.dirty) { p = put_s(r, "UNSAVED BANK"); rc = T.warn; }
    *p = 0;
    ui_foot("", prog_name[0] ? prog_name : "-", edited ? T.warn : T.text, r, rc);
}

static void ui_draw_page(void)
{
    const vs_page_t *pg = &VS_PAGES[page];
    uint32_t k, n = 0, i = 0, key = ui_patch_key(), ms = 40u;
    const uint32_t kind = page_kind(page);
    for (k = 0; k < VS_NPAGE; k++) if (VS_PAGES[k].btn == pg->btn) { if (k == page) i = n; n++; }
    if (pg->btn == VS_BTN_GLO) n += 2u;         /* GLOBAL's two pages close the GLO walk */
    ui_head(BTN_NAME[pg->btn], T.theme, pg->title, n, i);
    if (kind == G_LFO1 || kind == G_LFO2) {    /* the dot moves: 12 frames a second while the LFO runs */
        const uint32_t l = kind == G_LFO2;
        if (P(l ? VS_P_LFO2_ON : VS_P_LFO1_ON) && P(l ? VS_P_LFO2_DEPTH : VS_P_LFO1_DEPTH)) { key = hsh(key, fm1_ms / 80u); ms = 80u; }
    }
    ui_panel(kind, hsh(key, page), need_full ? 0u : ms);
    ui_cells_param(pg->id);
    ui_foot_prog();
}

static void ui_draw_home(void)
{
    static const uint8_t KN[4] = {VS_P_FLT_FREQ, VS_P_FLT_RES, VS_P_FLT_AMOUNT, VS_P_AMP_GAIN};
    static uint32_t quiet, flat;
    uint32_t key = hsh(hsh(ui_patch_key(), prog.bank << 8 | slot), (uint32_t)edited << 16 ^ (uint32_t)(uint8_t)prog_name[0] << 8 ^ (uint8_t)prog_name[1]);
    key = hsh(key, (uint32_t)eng.nv << 8 ^ (uint32_t)(set.oct + 4));
    if (eng.act || scope_pk >= 64u) quiet = fm1_ms;   /* a voice, or a tail still heard (the delay's echoes) */
    if (fm1_ms - quiet < 300u) {                /* sounding: the scope, 12 frames a second */
        if (!scope_req) scope_req = 1;
        key = hsh(key, scope_seq);
        flat = 0;
    } else if (!flat) {                         /* silent: the line goes flat (not the last wave held) */
        uint32_t i;
        for (i = 0; i < UI_SCOPE_N; i++) scope[i] = 0;
        scope_pk = 0;
        flat = 1;
        key = hsh(key, 0xF1A7u);
    }
    ui_head("HOME", T.theme, APP_NAME, 0, 0);
    ui_panel(G_HOME, key, need_full ? 0u : 80u);
    ui_cells_param(KN);
    ui_foot_prog();
}

static void ui_draw_save(void)
{
    ui_knob_t k;
    char *p;
    ui_head("SAVE", T.warn, "SAVE", 0, 0);
    ui_panel(G_SAVE, hsh(hsh(save_bank << 8 | save_slot, prog.bank), (uint32_t)(uint8_t)prog_name[0] << 8 ^ (uint8_t)prog_name[2]), 0u);
    k.hot = 0; k.mod = 0; k.c = 0;
    k.w = W_CHOICE; k.n = (uint8_t)VS_NBANK; k.idx = (uint8_t)(save_bank - 1u); k.v = 0; k.lab = "BANK";
    p = put_s(k.val, vs_prog_bank_name(save_bank)); *p = 0;
    ui_cell(0, &k);
    k.w = W_VALUE; k.n = 0; k.idx = 0; k.v = ang_of((int32_t)save_slot, 0, 127); k.lab = "SLOT";
    p = put_3(k.val, save_slot + 1u); *p = 0;
    ui_cell(1, &k);
    k.w = W_NONE; k.lab = ""; k.val[0] = 0; k.v = 0;
    ui_cell(2, &k);
    ui_cell(3, &k);
    ui_foot("SAVE: WRITE", "", T.text, "ANOTHER: LEAVE", T.dim);
}

static void ui_draw_global(void)
{
    ui_knob_t k;
    char *p;
    const int ext = clk_t && fm1_ms - clk_t < 1000u;
    k.mod = 0; k.c = 0; k.n = 0; k.idx = 0;
    ui_head("GLO", T.theme, "GLOBAL", 0, 0);
    {   /* the pips: GLO's patch pages, then GLOBAL's two */
        uint32_t n = 0, j;
        for (j = 0; j < VS_NPAGE; j++) if (VS_PAGES[j].btn == VS_BTN_GLO) n++;
        k_head = 0;
        ui_head("GLO", T.theme, "GLOBAL", n + 3u, n + gpage);
    }
    if (!gpage) {
        ui_panel(G_GLOB1, hsh(hsh(set.voices, set.ch), (uint32_t)ext << 16 ^ eng.bpm_q8 >> 8), 0u);
        k.hot = (uint8_t)ui_ghot(0); k.w = W_VALUE; k.v = ang_of(set.ch, 0, 16); k.lab = "MIDI CH";
        p = set.ch ? put_u(k.val, set.ch) : put_s(k.val, "ANY"); *p = 0;
        ui_cell(0, &k);
        k.hot = (uint8_t)ui_ghot(1); k.v = ang_of(set.voices, 1, (int32_t)VS_NV); k.lab = "VOICES";
        p = put_u(k.val, set.voices); *p = 0;
        ui_cell(1, &k);
        k.hot = (uint8_t)ui_ghot(2); k.v = ang_of(set.tempo, 40, 240); k.lab = "TEMPO";
        p = put_u(k.val, set.tempo); *p = 0;
        ui_cell(2, &k);
        k.hot = 0; k.w = W_READ; k.v = 0; k.lab = "CLOCK";
        p = put_s(k.val, ext ? "EXT" : "INT"); *p = 0;
        ui_cell(3, &k);
    } else if (gpage == 2u) {                   /* DIAG: the meters, read only; redrawn twice a second */
        const uint32_t key = hsh(hsh(hsh(dg_avg, dg_peak << 8 ^ pc_voice << 16 ^ pc_bus << 24), eng.shed),
                                 (uint32_t)vs_core1_online() ^ (uint32_t)ext << 1 ^ eng.bpm_q8 >> 6);
        ui_panel(G_GLOB3, key, need_full ? 0u : 500u);
        k.hot = 0; k.w = W_READ; k.v = 0;
        k.lab = "CPU"; p = put_u(k.val, dg_avg); *p++ = '%'; *p = 0; ui_cell(0, &k);
        k.lab = "PEAK"; p = put_u(k.val, dg_peak); *p++ = '%'; *p = 0; ui_cell(1, &k);
        k.lab = "CORES"; p = put_u(k.val, vs_core1_online() ? 2u : 1u); *p = 0; ui_cell(2, &k);
        k.lab = "DROPPED"; p = put_u(k.val, eng.shed); *p = 0; ui_cell(3, &k);
    } else {
        ui_panel(G_GLOB2, set.theme, 0u);
        k.hot = (uint8_t)ui_ghot(0); k.w = W_CHOICE; k.n = (uint8_t)UI_NTHEME; k.idx = set.theme; k.lab = "THEME";
        p = put_s(k.val, UI_THEMES[set.theme].name); *p = 0;
        ui_cell(0, &k);
        k.hot = (uint8_t)ui_ghot(1); k.w = W_CHOICE; k.n = 2; k.idx = set.colour ? 1u : 0u; k.lab = "COLOUR";
        p = put_s(k.val, set.colour ? "PATCH" : "THEME"); *p = 0;
        ui_cell(1, &k);
        k.hot = 0; k.w = W_NONE; k.lab = ""; k.val[0] = 0;
        ui_cell(2, &k);
        ui_cell(3, &k);
    }
    ui_foot_prog();
}

/* BANK: nine rows around the cursor; a row a canvas, redrawn when it changes */
static uint32_t k_row[9];
static void ui_draw_bank(void)
{
    int32_t r;
    char s[24], *p;
    p = put_s(s, "BANK  "); p = put_s(p, vs_prog_bank_name(prog.bank)); *p = 0;
    ui_head("SEQ", T.theme, s, 0, 0);
    for (r = -4; r <= 4; r++) {
        const uint32_t sl = (cursor + 128u + (uint32_t)r) & 127u, i = (uint32_t)(r + 4);
        const int cur = r == 0, empty = vs_prog_empty(&prog, sl);
        uint32_t key = hsh(hsh(sl << 8 | prog.bank, (uint32_t)cur << 1 | (sl == slot)), empty ? 0u : prog.r[sl].b[VS_REC - 1u]);
        uint32_t k;
        if (!empty) for (k = 0; k < VS_NAME; k++) key = hsh(key, prog.r[sl].b[k]);
        if (key == k_row[i]) continue;
        k_row[i] = key;
        cv_begin(240, 22, T.bg);
        if (cur) rframe(4, 1, 226, 20, 4, T.theme, T.fill);
        if (sl == slot && prog.bank == set.bank) fill(1, 5, 2, 12, T.accent);
        p = put_3(s, sl + 1u); *p = 0;
        txt(12, 3, &FONT_VAL, s, cur ? T.theme : T.dim);
        if (empty) txt(54, 4, &FONT_LAB, "empty", T.line);
        else {
            for (k = 0; k < VS_NAME; k++) s[k] = (char)prog.r[sl].b[k];
            s[VS_NAME] = 0;
            for (k = VS_NAME; k > 0u && s[k - 1u] == ' '; k--) s[k - 1u] = 0;
            disc(44 * 16, 11 * 16, 48, prog.r[sl].b[VS_REC - 1u] ? ui_patch_colour(prog.r[sl].b[VS_REC - 1u]) : T.line);
            txt_fit(54, 3, 170, &FONT_VAL, s, T.text, 0);
        }
        cv_blit(0, 25u + i * 22u);
    }
    {   /* the scroll bar */
        static uint32_t kb;
        if (kb != (cursor | 0x100u) || need_full) {
            kb = cursor | 0x100u;
            cv_begin(4, 198, T.bg);
            rrect(0, 0, 3, 198, 1, T.surf);
            rrect(0, (int32_t)(cursor * 172u / 127u), 3, 26, 1, T.dim);
            cv_blit(234, 25);
        }
    }
    ui_foot("K1 BANK", "", T.text, "SELECT / PRESETS SLOT", T.dim);
}

/* everything: the theme first (a change redraws all), then the screen in view */
static void ui_draw(uint32_t avg_us, uint32_t peak_pc)
{
    uint32_t k;
    ui_theme_service();
    dg_avg = avg_us * 100u / BUF_US; dg_peak = peak_pc;
    if (need_full) {
        lcd_fill(0, 0, 240, 240, T.bg);
        k_head = k_panel = k_foot = 0;
        for (k = 0; k < 4u; k++) k_cell[k] = 0;
        for (k = 0; k < 9u; k++) k_row[k] = 0;
    }
    if (scr == SCR_PAGE) ui_draw_page();
    else if (scr == SCR_BANK) ui_draw_bank();
    else if (scr == SCR_SAVE) ui_draw_save();
    else if (scr == SCR_GLOBAL) ui_draw_global();
    else ui_draw_home();
}
