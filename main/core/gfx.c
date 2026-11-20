#include "gfx.h"
#include <math.h>
#include <string.h>

const glyph_t *font_glyph(const font_t *f, char c)
{
    unsigned u = (unsigned char)c;
    if (u < 32 || u > 126) u = '?';
    return &f->g[u - 32];
}

int text_width(const font_t *f, const char *s, float track)
{
    float w = 0;
    int n = 0;
    for (const char *p = s; *p; p++, n++) w += font_glyph(f, *p)->adv16 / 16.0f;
    if (n > 1) w += track * (n - 1);
    return (int)(w + 0.5f);
}

static inline int glyph_alpha(const font_t *f, const glyph_t *g, int gx, int gy)
{
    int i = gy * g->w + gx;
    uint8_t b = f->bits[g->off + (i >> 1)];
    return (i & 1) ? (b & 15) : (b >> 4);
}

void text_walk(const font_t *f, const char *s, float x, int baseline, float track,
               int clip_y0, int clip_y1, glyph_px_fn fn, void *ctx)
{
    float pen = x;
    int idx = 0;
    for (const char *p = s; *p; p++, idx++) {
        const glyph_t *g = font_glyph(f, *p);
        int gx0 = (int)floorf(pen + 0.5f) + g->xo;
        int gy0 = baseline + g->yo;
        if (g->w && gy0 < clip_y1 && gy0 + g->h > clip_y0) {
            int ya = gy0 < clip_y0 ? clip_y0 - gy0 : 0;
            int yb = gy0 + g->h > clip_y1 ? clip_y1 - gy0 : g->h;
            for (int gy = ya; gy < yb; gy++)
                for (int gx = 0; gx < g->w; gx++) {
                    int a = glyph_alpha(f, g, gx, gy);
                    if (a) fn(ctx, gx0 + gx, gy0 + gy, a, idx);
                }
        }
        pen += g->adv16 / 16.0f + track;
    }
}

typedef struct { strip_t *st; uint16_t c; int alpha; } tdctx_t;

static void td_px(void *vc, int x, int y, int a15, int ci)
{
    (void)ci;
    tdctx_t *c = vc;
    if (x < 0 || x >= SCR_W) return;
    uint16_t *d = &c->st->px[(y - c->st->y0) * SCR_W + x];
    *d = blend565(*d, c->c, (a15 * c->alpha) / 15);
}

void text_draw(strip_t *st, const font_t *f, const char *s, float x, int baseline,
               float track, uint16_t color, int alpha)
{
    if (alpha <= 0) return;
    if (baseline - f->asc - 4 > st->y0 + st->h || baseline + f->desc + 4 < st->y0) return;
    tdctx_t c = { st, color, alpha };
    text_walk(f, s, x, baseline, track, st->y0, st->y0 + st->h, td_px, &c);
}

void text_draw_centered(strip_t *st, const font_t *f, const char *s, int cx, int baseline,
                        float track, uint16_t color, int alpha)
{
    text_draw(st, f, s, cx - text_width(f, s, track) / 2.0f, baseline, track, color, alpha);
}

void fill_rrect(strip_t *st, float x0, float y0, float x1, float y1, float r, uint16_t c, int alpha)
{
    /* per-row span: float math once per row, integer blend inside */
    int ya = (int)floorf(y0), yb = (int)ceilf(y1);
    if (ya < st->y0) ya = st->y0;
    if (yb > st->y0 + st->h) yb = st->y0 + st->h;
    for (int y = ya; y < yb; y++) {
        float fy = y + 0.5f;
        float vcov = clampf(fminf(fy - y0, y1 - fy) + 0.5f, 0, 1);
        if (vcov <= 0) continue;
        float inset = 0;
        if (r > 0) {
            float d = 0;
            if (fy < y0 + r) d = y0 + r - fy;
            else if (fy > y1 - r) d = fy - (y1 - r);
            if (d > 0) inset = r - sqrtf(fmaxf(0, r * r - d * d));
        }
        float xa = x0 + inset, xb = x1 - inset;
        int ia = (int)floorf(xa), ib = (int)ceilf(xb);
        if (ia < 0) ia = 0;
        if (ib > SCR_W) ib = SCR_W;
        uint16_t *o = &st->px[(y - st->y0) * SCR_W];
        int full = (int)(vcov * alpha);
        for (int x = ia; x < ib; x++) {
            float hc = fminf(x + 1 - xa, xb - x);
            int a = hc >= 1 ? full : (int)(clampf(hc, 0, 1) * full);
            o[x] = blend565(o[x], c, a);
        }
    }
}

void draw_seg(strip_t *st, float ax, float ay, float bx, float by, float w, uint16_t c, int alpha)
{
    float minx = fminf(ax, bx) - w, maxx = fmaxf(ax, bx) + w;
    float miny = fminf(ay, by) - w, maxy = fmaxf(ay, by) + w;
    int ya = (int)floorf(miny), yb = (int)ceilf(maxy);
    if (ya < st->y0) ya = st->y0;
    if (yb > st->y0 + st->h) yb = st->y0 + st->h;
    int xa = (int)floorf(minx), xb = (int)ceilf(maxx);
    if (xa < 0) xa = 0;
    if (xb > SCR_W) xb = SCR_W;
    float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            float px = x + 0.5f - ax, py = y + 0.5f - ay;
            float t = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
            t = clampf(t, 0, 1);
            float ex = px - t * dx, ey = py - t * dy;
            float d = sqrtf(ex * ex + ey * ey) - w * 0.5f;
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            uint16_t *p = &st->px[(y - st->y0) * SCR_W + x];
            *p = blend565(*p, c, (int)(cov * alpha));
        }
}

void glow_add(strip_t *st, float cx, float cy, float rad, int r, int g, int b, float amt)
{
    if (amt <= 0.004f) return;
    int ya = (int)(cy - rad), yb = (int)(cy + rad) + 1;
    if (ya < st->y0) ya = st->y0;
    if (yb > st->y0 + st->h) yb = st->y0 + st->h;
    int xa = (int)(cx - rad), xb = (int)(cx + rad) + 1;
    if (xa < 0) xa = 0;
    if (xb > SCR_W) xb = SCR_W;
    float inv = 1.0f / (rad * rad);
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float q = 1.0f - (dx * dx + dy * dy) * inv;
            if (q <= 0) continue;
            q = q * q * amt;
            uint16_t *p = &st->px[(y - st->y0) * SCR_W + x];
            *p = add565(*p, (int)(r * q), (int)(g * q), (int)(b * q));
        }
}

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float ease_out_cubic(float t) { t = clampf(t, 0, 1); t = 1 - t; return 1 - t * t * t; }
float ease_in_out_cubic(float t)
{
    t = clampf(t, 0, 1);
    return t < 0.5f ? 4 * t * t * t : 1 - powf(-2 * t + 2, 3) / 2;
}
float ease_out_back(float t, float s)
{
    t = clampf(t, 0, 1) - 1;
    return 1 + (s + 1) * t * t * t + s * t * t;
}
float smoothstep01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
