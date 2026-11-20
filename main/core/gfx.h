/* Portable pixel helpers shared by the device firmware and the host simulator. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCR_W 170
#define SCR_H 320

typedef struct {
    uint16_t off;       /* byte offset into packed 4-bit bitmap */
    uint8_t w, h;
    int8_t xo, yo;      /* bitmap top-left relative to pen on baseline */
    uint16_t adv16;     /* advance in 1/16 px */
} glyph_t;

typedef struct {
    const uint8_t *bits;
    const glyph_t *g;   /* 95 entries, ASCII 32..126 */
    int8_t asc, desc;
} font_t;

/* A horizontal band of the screen being rendered. */
typedef struct {
    uint16_t *px;       /* SCR_W * h, RGB565 (native endian) */
    int y0, h;
} strip_t;

static inline uint16_t rgb565(int r, int g, int b)
{
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* a: 0..256 */
static inline uint16_t blend565(uint16_t d, uint16_t s, int a)
{
    if (a <= 0) return d;
    if (a >= 256) return s;
    uint32_t dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    uint32_t sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31;
    dr += ((int)(sr - dr) * a) >> 8;
    dg += ((int)(sg - dg) * a) >> 8;
    db += ((int)(sb - db) * a) >> 8;
    return (uint16_t)((dr << 11) | (dg << 5) | db);
}

/* multiply brightness, m: 0..256 (256 = unchanged) */
static inline uint16_t scale565(uint16_t c, int m)
{
    uint32_t r = ((c >> 11) * m) >> 8, g = (((c >> 5) & 63) * m) >> 8, b = ((c & 31) * m) >> 8;
    if (r > 31) r = 31; if (g > 63) g = 63; if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t add565(uint16_t c, int r, int g, int b)
{
    int cr = (c >> 11) + (r >> 3), cg = ((c >> 5) & 63) + (g >> 2), cb = (c & 31) + (b >> 3);
    if (cr > 31) cr = 31; if (cg > 63) cg = 63; if (cb > 31) cb = 31;
    if (cr < 0) cr = 0; if (cg < 0) cg = 0; if (cb < 0) cb = 0;
    return (uint16_t)((cr << 11) | (cg << 5) | cb);
}

const glyph_t *font_glyph(const font_t *f, char c);
/* width in px of a string drawn with extra tracking (px between letters) */
int text_width(const font_t *f, const char *s, float track);

/* Callback-based glyph walker: calls fn for each glyph pixel with alpha 0..15. */
typedef void (*glyph_px_fn)(void *ctx, int x, int y, int a15, int char_index);
void text_walk(const font_t *f, const char *s, float x, int baseline, float track,
               int clip_y0, int clip_y1, glyph_px_fn fn, void *ctx);

/* Alpha-blend text over an RGB strip. alpha 0..256 */
void text_draw(strip_t *st, const font_t *f, const char *s, float x, int baseline,
               float track, uint16_t color, int alpha);
void text_draw_centered(strip_t *st, const font_t *f, const char *s, int cx, int baseline,
                        float track, uint16_t color, int alpha);

/* Filled rounded rectangle with anti-aliased edges. */
void fill_rrect(strip_t *st, float x0, float y0, float x1, float y1, float r, uint16_t c, int alpha);

/* Anti-aliased thick line segment, only partially drawn: 0..1 of its length. */
void draw_seg(strip_t *st, float ax, float ay, float bx, float by, float w, uint16_t c, int alpha);

/* soft round glow, additive */
void glow_add(strip_t *st, float cx, float cy, float rad, int r, int g, int b, float amt);

float ease_out_cubic(float t);
float ease_in_out_cubic(float t);
float ease_out_back(float t, float s);
float smoothstep01(float t);
float clampf(float v, float a, float b);
float lerpf(float a, float b, float t);

uint32_t hash32(uint32_t x);
