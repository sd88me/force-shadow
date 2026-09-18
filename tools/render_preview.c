/* render_preview.c -- host-side preview tool for Maze Voice's shadow-mode
 * pages. Shares the same drawing primitives/widget logic that gets
 * ported into force_shadow.c's real renderer, but outputs a plain PPM
 * image instead of writing into a DRM dumb buffer -- lets UI layout be
 * iterated and checked visually on a dev machine, without a live device
 * round-trip for every tweak. Builds and runs natively (no cross-compile,
 * no QEMU): `gcc -O2 -o render_preview render_preview.c -lm && ./render_preview`.
 *
 * Deliberately skips the portrait buffer transform live load test #6
 * established (see DESIGN.md) -- this renders straight into a landscape
 * RGB canvas, since that transform is already proven separately and
 * isn't what's being checked here (page layout, widget legibility,
 * knob/label positioning). The real force_shadow.c build still goes
 * through put_px_land()'s transform as always.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../src/font8x8.h"

#define LAND_W 1280
#define LAND_H 800

static uint8_t canvas[LAND_H][LAND_W][3];

typedef struct { uint8_t r, g, b; } rgb_t;
static rgb_t rgb(uint32_t hex) {
    rgb_t c = { (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff };
    return c;
}

/* ---- palette (Maze Voice web GUI) ---- */
static const uint32_t PLATE      = 0x131211;
static const uint32_t PLATE_HI   = 0x1c1a17;
static const uint32_t PLATE_LINE = 0x2a2823;
static const uint32_t INK        = 0xefe9d8;
static const uint32_t INK_DIM    = 0x8f8878;
static const uint32_t INK_FAINT  = 0x5c584c;
static const uint32_t ACCENT     = 0xc1552f;
static const uint32_t ACCENT_HI  = 0xe2793f;
static const uint32_t KNOB_FACE  = 0xefe9d8;
static const uint32_t KNOB_RING  = 0x2a2823;
static const uint32_t BAR_BG     = 0x0d0c0a;

/* ---- primitives ---- */
static void put_px(int x, int y, uint32_t color) {
    if (x < 0 || x >= LAND_W || y < 0 || y >= LAND_H) return;
    rgb_t c = rgb(color);
    canvas[y][x][0] = c.r; canvas[y][x][1] = c.g; canvas[y][x][2] = c.b;
}
static void fill_rect(int x0, int y0, int w, int h, uint32_t color) {
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            put_px(x, y, color);
}
static void fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x*x + y*y <= r*r) put_px(cx + x, cy + y, color);
}
static void draw_ring(int cx, int cy, int r, int thick, uint32_t color) {
    int r_in = r - thick;
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++) {
            int d2 = x*x + y*y;
            if (d2 <= r*r && d2 >= r_in*r_in) put_px(cx + x, cy + y, color);
        }
}
static void draw_hline(int x0, int y, int w, uint32_t color) { fill_rect(x0, y, w, 1, color); }
static void draw_vline(int x, int y0, int h, uint32_t color) { fill_rect(x, y0, 1, h, color); }

/* ---- text (font8x8.h) ---- */
static int font_glyph_index(char ch) {
    for (size_t i = 0; i < strlen(font_chars); i++)
        if (font_chars[i] == ch) return (int)i;
    return 0; /* space */
}
static void draw_char(int x, int y, char ch, int scale, uint32_t color) {
    const uint8_t *g = font8x8[font_glyph_index(ch)];
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (1 << (7 - col)))
                fill_rect(x + col*scale, y + row*scale, scale, scale, color);
}
static int text_width(const char *s, int scale) { return (int)strlen(s) * 9 * scale - scale; }
static void draw_text(int x, int y, const char *s, int scale, uint32_t color) {
    int cx = x;
    for (const char *p = s; *p; p++) { draw_char(cx, y, *p, scale, color); cx += 9*scale; }
}
static void draw_text_c(int cx, int y, const char *s, int scale, uint32_t color) {
    draw_text(cx - text_width(s, scale)/2, y, s, scale, color);
}

/* ---- knob pointer angle (libm ok here -- host tool only) ---- */
static void knob_dot(int cx, int cy, int r, int pct, int *dx, int *dy) {
    double angle = (-135.0 + 270.0 * pct / 100.0) * M_PI / 180.0;
    int dist = (int)(r * 0.72);
    *dx = cx + (int)(dist * sin(angle));
    *dy = cy - (int)(dist * cos(angle));
}

/* ---- widgets ---- */
static void widget_knob(int cx, int cy, int r, int pct, const char *label, const char *value) {
    draw_ring(cx, cy, r + 3, 3, KNOB_RING);
    fill_circle(cx, cy, r, KNOB_FACE);
    int dx, dy; knob_dot(cx, cy, r, pct, &dx, &dy);
    fill_circle(dx, dy, r/7 + 2, ACCENT);
    draw_text_c(cx, cy + r + 10, label, 1, INK);
    draw_text_c(cx, cy + r + 22, value, 1, INK_FAINT);
}
static void widget_toggle(int cx, int cy, const char *label, int on) {
    int pw = 34, ph = 18;
    fill_rect(cx - pw/2, cy - ph/2, pw, ph, 0x050403);
    draw_ring(cx - pw/2 + ph/2, cy, ph/2 - 2, 1, PLATE_LINE);
    int lx = on ? (cx + pw/2 - ph/2) : (cx - pw/2 + ph/2);
    fill_circle(lx, cy, ph/2 - 3, on ? ACCENT_HI : 0x4c473d);
    draw_text_c(cx, cy + ph/2 + 8, label, 1, INK);
}
static void widget_button(int cx, int cy, const char *label) {
    int w = text_width(label, 1) + 24, h = 26;
    fill_rect(cx - w/2, cy - h/2, w, h, ACCENT);
    draw_text_c(cx, cy - 3, label, 1, 0xfdf3ea);
}
static void widget_enum_h(int cx, int cy, const char *label, const char **opts, int n, int active) {
    draw_text_c(cx, cy - 30, label, 1, INK);
    int seg_w = 78, seg_h = 22, gap = 2;
    int total = n * seg_w + (n-1)*gap;
    int x0 = cx - total/2;
    for (int i = 0; i < n; i++) {
        int x = x0 + i*(seg_w+gap);
        fill_rect(x, cy - seg_h/2, seg_w, seg_h, i == active ? 0xf2f1ee : 0x050403);
        draw_text_c(x + seg_w/2, cy - 3, opts[i], 1, i == active ? 0x1c1a17 : INK_DIM);
    }
}
static void widget_enum_v(int cx, int cy, const char *label, const char **opts, int n, int active) {
    draw_text_c(cx, cy - (n*24)/2 - 20, label, 1, ACCENT_HI);
    int seg_w = 90, seg_h = 20, gap = 2;
    int y0 = cy - (n*(seg_h+gap))/2;
    for (int i = 0; i < n; i++) {
        int y = y0 + i*(seg_h+gap);
        fill_rect(cx - seg_w/2, y, seg_w, seg_h, i == active ? 0xf2f1ee : 0x050403);
        draw_text_c(cx, y + 5, opts[i], 1, i == active ? 0x1c1a17 : INK_DIM);
    }
}
static void frame_box(int x, int y, int w, int h, const char *title) {
    draw_ring(0,0,0,0,0); /* no-op, keeps signature symmetry */
    fill_rect(x, y, w, 1, PLATE_LINE);
    fill_rect(x, y, 1, h, PLATE_LINE);
    fill_rect(x+w-1, y, 1, h, PLATE_LINE);
    fill_rect(x, y+h-1, w, 1, PLATE_LINE);
    draw_text(x + 18, y + 16, title, 1, ACCENT_HI);
    fill_rect(x + 18, y + 30, w - 36, 1, PLATE_LINE);
}

/* ---- chrome: top bar + tab bar ---- */
#define TOPBAR_H 72
#define TABBAR_H 72
/* Confirmed live (2026-09-18): the touch digitizer's native height is
 * only 720px, not LAND_H's 800 -- a tab bar flush with LAND_H's bottom
 * edge is completely untouchable, not just imprecise. Kept in sync with
 * force_shadow.c's own TOUCHABLE_H so this preview stays a faithful
 * reference for future pages. */
#define TOUCHABLE_H 720
static const char *TABS[] = { "VOICE", "WAVEFOLDER / FILTER", "MOD / RANDOM / MIX" };
static void draw_chrome(int active_tab) {
    fill_rect(0, 0, LAND_W, LAND_H, PLATE);
    fill_rect(0, 0, LAND_W, TOPBAR_H, PLATE_HI);
    draw_hline(0, TOPBAR_H, LAND_W, PLATE_LINE);
    draw_text(40, 28, "FORCE SHADOW - MAZE VOICE", 2, INK);
    fill_circle(LAND_W - 150, 36, 5, ACCENT_HI);
    draw_text(LAND_W - 130, 28, "LIVE", 2, INK_DIM);

    int tabbar_y = TOUCHABLE_H - TABBAR_H;
    fill_rect(0, tabbar_y, LAND_W, TABBAR_H, BAR_BG);
    draw_hline(0, tabbar_y, LAND_W, PLATE_LINE);
    int n = 3, tw = LAND_W / n;
    for (int i = 0; i < n; i++) {
        if (i == active_tab) {
            fill_rect(i*tw, tabbar_y, tw, 3, ACCENT);
            fill_rect(i*tw, tabbar_y, tw, TABBAR_H, 0x1a120d);
        }
        draw_text_c(i*tw + tw/2, tabbar_y + TABBAR_H/2 - 6,
                    TABS[i], 2, i == active_tab ? INK : INK_FAINT);
    }
}

#define CONTENT_Y (TOPBAR_H + 16)
#define CONTENT_H (TOUCHABLE_H - TOPBAR_H - TABBAR_H - 32)

/* ---- Page 1: Voice ---- */
static void page_voice(void) {
    int margin = 36, gap = 20;
    int fw = (LAND_W - 2*margin - 2*gap) / 3;
    int x0 = margin, x1 = x0 + fw + gap, x2 = x1 + fw + gap;
    int y = CONTENT_Y, h = CONTENT_H;

    frame_box(x0, y, fw, h, "OSCILLATOR");
    {
        const char *rl[3] = {"VCO","MOD","FM"};
        struct { const char *l1,*v1,*l2,*v2; int p1,p2; } rows[3] = {
            {"VCO TUNE","0 ST","VCO EG1","0", 50,50},
            {"MOD FREQ","55.0 HZ","MOD EG1","0", 42,50},
            {"FM DEPTH","0","FM EG1","0", 0,50},
        };
        int ry0 = y + 50, rh = (h - 60) / 3;
        for (int i = 0; i < 3; i++) {
            int ry = ry0 + i*rh + rh/2;
            draw_text(x0 + 18, ry - 4, rl[i], 1, ACCENT_HI);
            int kx0 = x0 + 85, kx1 = x0 + fw - 70;
            widget_knob(kx0, ry, 26, rows[i].p1, rows[i].l1, rows[i].v1);
            widget_knob(kx1, ry, 26, rows[i].p2, rows[i].l2, rows[i].v2);
        }
    }

    frame_box(x1, y, fw, h, "ENVELOPES");
    {
        int ry = y + h/2 + 10;
        widget_knob(x1 + fw/3, ry, 30, 60, "ENV1 DEC", "60");
        widget_knob(x1 + 2*fw/3, ry, 30, 70, "VCA DECAY", "70");
    }

    frame_box(x2, y, fw, h, "MIXER / TONE");
    {
        struct { const char *l,*v; int p; } items[7] = {
            {"VCO LVL","100",50}, {"MOD LVL","50",25}, {"NOISE LVL","0",0},
            {"NOISE TONE","0",50}, {"RING LVL","0",0}, {"TONE/SAT","0",0},
            {"OUT LEVEL","80",80},
        };
        int cols = 3, rowsN = 3;
        int cw = (fw - 36) / cols, rh = (h - 50) / rowsN;
        for (int i = 0; i < 7; i++) {
            int col = i % cols, row = i / cols;
            int cx = x2 + 18 + cw*col + cw/2;
            int cyk = y + 50 + rh*row + rh/2;
            widget_knob(cx, cyk, 24, items[i].p, items[i].l, items[i].v);
        }
    }
}

/* ---- Page 2: WaveFolder / Filter ---- */
static void page_wavefolder_filter(void) {
    int margin = 36, gap = 20, divw = 140;
    int fw = (LAND_W - 2*margin - 2*gap - divw) / 2;
    int x0 = margin, xdiv = x0 + fw + gap, x1 = xdiv + divw + gap;
    int y = CONTENT_Y, h = CONTENT_H;

    frame_box(x0, y, fw, h, "WAVEFOLDER");
    {
        struct { const char *l,*v; int p; } items[4] = {
            {"FOLD DRIVE","0",0}, {"FOLD BIAS","0",50},
            {"FOLD EG1","0",50}, {"FOLD KEY","0",0},
        };
        int cw = fw/2, rh = (h-50)/2;
        for (int i = 0; i < 4; i++) {
            int col = i%2, row = i/2;
            widget_knob(x0 + cw*col + cw/2, y+50+rh*row+rh/2, 28, items[i].p, items[i].l, items[i].v);
        }
    }

    {
        int cx = xdiv + divw/2, cy = y + h/2;
        const char *ropts[3] = {"VCW>VCF","PARALLEL","VCF>VCW"};
        widget_enum_v(cx, cy - 90, "ROUTE", ropts, 3, 1);
        widget_knob(cx, cy + 70, 28, 50, "BLEND", "0");
    }

    frame_box(x1, y, fw, h, "FILTER");
    {
        struct { const char *l,*v; int p; } items[6] = {
            {"CUTOFF","100",100}, {"RESONANCE","0",0}, {"LP-BP","0",0},
            {"CUT EG1","30",65}, {"CUT KEY","0",0}, {"FILT DRIVE","0",0},
        };
        int cw = fw/2, rh = (h-50)/3;
        for (int i = 0; i < 6; i++) {
            int col = i%2, row = i/2;
            widget_knob(x1 + cw*col + cw/2, y+50+rh*row+rh/2, 26, items[i].p, items[i].l, items[i].v);
        }
    }
}

/* ---- Page 3: Mod / Random / Mix ---- */
static void page_mod_random_mix(void) {
    int margin = 36, gap = 20;
    int fw = (LAND_W - 2*margin - 2*gap) / 3;
    int x0 = margin, x1 = x0 + fw + gap, x2 = x1 + fw + gap;
    int y = CONTENT_Y, h = CONTENT_H;

    frame_box(x0, y, fw, h, "KEY TRACK");
    widget_knob(x0 + fw/2, y + h/3, 30, 100, "VCO KEY", "100");
    widget_knob(x0 + fw/2, y + 2*h/3, 30, 100, "MOD KEY", "100");

    frame_box(x1, y, fw, h, "RANDOMISE");
    {
        const char *labels[4] = {"RND VOICE","RND FOLD","RND FILT","RND TONE"};
        int ry0 = y + 55, rh = (h - 110) / 4;
        for (int i = 0; i < 4; i++)
            widget_toggle(x1 + fw/2, ry0 + rh*i + rh/2, labels[i], 1);
        widget_button(x1 + fw/2, y + h - 30, "GENERATE");
    }

    frame_box(x2, y, fw, h, "OUTPUT MIX");
    {
        int ry0 = y + 55, rh = (h - 55) / 3;
        widget_toggle(x2 + fw/2, ry0 + rh*0 + rh/2, "VOICE ON/OFF", 1);
        widget_knob(x2 + fw/2, ry0 + rh*1 + rh/2, 28, 66, "GAIN", "100 %");
        const char *chopts[3] = {"L","R","L+R"};
        widget_enum_h(x2 + fw/2, ry0 + rh*2 + rh/2, "CHANNEL", chopts, 3, 2);
    }
}

/* ---- PPM output ---- */
static void write_ppm(const char *path) {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", LAND_W, LAND_H);
    fwrite(canvas, 1, sizeof(canvas), f);
    fclose(f);
}

int main(int argc, char **argv) {
    int page = argc > 1 ? atoi(argv[1]) : 0;
    const char *out = argc > 2 ? argv[2] : "preview.ppm";
    draw_chrome(page);
    if (page == 0) page_voice();
    else if (page == 1) page_wavefolder_filter();
    else page_mod_random_mix();
    write_ppm(out);
    fprintf(stderr, "wrote %s (page %d)\n", out, page);
    return 0;
}
