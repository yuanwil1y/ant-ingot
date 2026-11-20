#include "ingot.h"
#include "fonts.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ helpers */

static float vnoise1(float x, uint32_t seed)
{
    int i = (int)floorf(x);
    float f = x - i;
    float a = (hash32(i * 2654435761u ^ seed) & 0xFFFF) / 65535.0f;
    float b = (hash32((i + 1) * 2654435761u ^ seed) & 0xFFFF) / 65535.0f;
    f = f * f * (3 - 2 * f);
    return a + (b - a) * f;
}


/* signed distance to the bar outline (negative inside) and outward normal */
static float bar_sdf(float x, float y, float *nx, float *ny)
{
    float cx = (BAR_X0 + BAR_X1) * 0.5f, cy = (BAR_Y0 + BAR_Y1) * 0.5f;
    float hx = (BAR_X1 - BAR_X0) * 0.5f - BAR_R, hy = (BAR_Y1 - BAR_Y0) * 0.5f - BAR_R;
    float sx = x < cx ? -1.f : 1.f, sy = y < cy ? -1.f : 1.f;
    float qx = fabsf(x - cx) - hx, qy = fabsf(y - cy) - hy;
    if (qx > 0 && qy > 0) {
        float l = sqrtf(qx * qx + qy * qy);
        *nx = sx * qx / l; *ny = sy * qy / l;
        return l - BAR_R;
    }
    if (qx > qy) { *nx = sx; *ny = 0; } else { *nx = 0; *ny = sy; }
    return fmaxf(qx, qy) - BAR_R;
}

/* ------------------------------------------------------------------ base */

#define PLATEAU 92
#define GROOVE 20

/* Height map of a blank bar: rounded bevel up to a flat plateau, with a fine
 * sand texture (front) or horizontal brushing (back). Lighting happens at render time. */
void ingot_gen_base(uint8_t *buf, uint32_t seed, int back)
{
    for (int y = 0; y < SCR_H; y++) {
        float streak = back ? (vnoise1(y * 0.9f, seed) - 0.5f) * 2.4f + (vnoise1(y * 3.1f, seed ^ 77) - 0.5f) * 1.6f : 0;
        for (int x = 0; x < SCR_W; x++) {
            float nx, ny;
            float d = -bar_sdf(x + 0.5f, y + 0.5f, &nx, &ny);   /* + inside */
            if (d < -0.5f) { buf[y * SCR_W + x] = 0; continue; }
            float cov = clampf(d + 0.5f, 0, 1);
            float h;
            bool rec = false;
            if (d < BAR_BEVEL) {
                h = 4 + (PLATEAU - 4) * (d / BAR_BEVEL);             /* flat 45-degree chamfer, sharp ridge */
            } else {
                h = PLATEAU;
                /* a fine engraved frame a few px inside the ridge, like a minted bar */
                float g = 1 - fabsf(d - FRAME_IN) / 1.1f;
                if (g > 0) { h -= 14 * g; rec = g > 0.5f; }
            }
            h += streak + ((hash32(x * 7919 + y * 104729 + seed) & 255) / 255.0f - 0.5f) * (back ? 0.8f : 1.1f);
            h *= cov;
            int v = (int)(h + 0.5f);
            if (v < 1) v = 1;
            if (v > 127) v = 127;
            buf[y * SCR_W + x] = (uint8_t)(v | (rec ? IG_RECESS : 0));
        }
    }
}

/* ------------------------------------------------------------------ ink layer */

static inline void ink_add(uint8_t *ink, int x, int y, int v)
{
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
    int n = ink[y * SCR_W + x] + v;
    ink[y * SCR_W + x] = n > 255 ? 255 : n;
}

static void ink_splat(uint8_t *ink, float x, float y, float amt)
{
    x -= 0.5f; y -= 0.5f;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    float a = amt * 255;
    ink_add(ink, ix, iy, (int)(a * (1 - fx) * (1 - fy)));
    ink_add(ink, ix + 1, iy, (int)(a * fx * (1 - fy)));
    ink_add(ink, ix, iy + 1, (int)(a * (1 - fx) * fy));
    ink_add(ink, ix + 1, iy + 1, (int)(a * fx * fy));
}

static void ink_circle(uint8_t *ink, float cx, float cy, float r, float w)
{
    int n = (int)(2 * 3.14159f * r / 0.3f);
    for (int i = 0; i < n; i++) {
        float a = i * 6.2831853f / n;
        ink_splat(ink, cx + r * cosf(a), cy + r * sinf(a), 0.3f * w);
    }
}

static void ink_seg(uint8_t *ink, float ax, float ay, float bx, float by, float w)
{
    float l = hypotf(bx - ax, by - ay);
    int n = (int)(l / 0.3f) + 1;
    for (int i = 0; i <= n; i++) {
        float t = (float)i / n;
        ink_splat(ink, ax + (bx - ax) * t, ay + (by - ay) * t, 0.3f * w);
    }
}

typedef struct { uint8_t *ink; float k; } inktx_t;
static void ink_text_px(void *c, int x, int y, int a, int ci)
{
    (void)ci;
    inktx_t *t = c;
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
    int v = (int)(a * 17 * t->k);
    uint8_t *p = &t->ink[y * SCR_W + x];
    if (v > *p) *p = v > 255 ? 255 : v;
}

static void ink_text(uint8_t *ink, const font_t *f, const char *s, int cx, int baseline, float track)
{
    inktx_t c = { ink, 1.0f };
    text_walk(f, s, cx - text_width(f, s, track) / 2.0f, baseline, track, 0, SCR_H, ink_text_px, &c);
}

static void ink_diamond(uint8_t *ink, float cx, float cy, float r)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float d = fabsf(x + 0.5f - cx) + fabsf(y + 0.5f - cy);
            float c = clampf(r - d + 0.5f, 0, 1);
            if (c > 0) ink_add(ink, x, y, (int)(c * 255));
        }
}

/* ------------------------------------------------------------------ time text */

static void civil(int64_t days, int *Y, int *M, int *D)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *Y = y + (m <= 2); *M = (int)m; *D = (int)d;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* US Eastern offset in seconds at a UTC instant (DST: 2nd Sun Mar 07:00Z .. 1st Sun Nov 06:00Z) */
int us_et_offset(int64_t utc)
{
    int Y, M, D;
    civil(utc / 86400, &Y, &M, &D);
    int64_t mar1 = days_from_civil(Y, 3, 1), nov1 = days_from_civil(Y, 11, 1);
    int wd_mar1 = (int)((mar1 + 4) % 7), wd_nov1 = (int)((nov1 + 4) % 7); /* 0=Sun */
    int64_t dst_on = (mar1 + (7 - wd_mar1) % 7 + 7) * 86400 + 7 * 3600;
    int64_t dst_off = (nov1 + (7 - wd_nov1) % 7) * 86400 + 6 * 3600;
    return (utc >= dst_on && utc < dst_off) ? -4 * 3600 : -5 * 3600;
}

static const char *MON[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static void fmt_date(int64_t local, char *out, int cap)
{
    int Y, M, D;
    civil(local / 86400 - (local < 0 && local % 86400), &Y, &M, &D);
    snprintf(out, cap, "%d %s %d", D, MON[M - 1], Y);
}

static void fmt_hm(int64_t local, char *out, int cap)
{
    int64_t s = ((local % 86400) + 86400) % 86400;
    snprintf(out, cap, "%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60));
}

static const char *NUMW[] = { "ZERO", "ONE", "TWO", "THREE", "FOUR", "FIVE", "SIX", "SEVEN", "EIGHT", "NINE", "TEN" };

/* ------------------------------------------------------------------ front */

static void rosette(uint8_t *ink, float cx, float cy, uint32_t seed)
{
    int n = 7 + hash32(seed) % 6;            /* lobes */
    int K = 13 + hash32(seed + 1) % 7;      /* interlaced copies */
    int m = 2 + hash32(seed + 2) % 4;        /* slow modulation */
    float twist = 0.25f + (hash32(seed + 3) % 100) / 100.0f * 0.6f;
    float depth = 0.55f + (hash32(seed + 4) % 100) / 100.0f * 0.35f;
    const float Rm = 42.5f, A = 9.5f;

    /* outer interlaced band */
    for (int k = 0; k < K; k++) {
        float phase = k * 6.2831853f / K;
        int N = 2200;
        float px = 0, py = 0;
        for (int i = 0; i <= N; i++) {
            float th = i * 6.2831853f / N;
            float r = Rm + A * (depth + (1 - depth) * sinf(m * th)) * sinf(n * th + phase + twist * sinf(th * m));
            float x = cx + r * cosf(th), y = cy + r * sinf(th);
            if (i) {
                float l = hypotf(x - px, y - py);
                int sub = (int)(l / 0.3f) + 1;
                for (int j = 1; j <= sub; j++) {
                    float t = (float)j / sub;
                    ink_splat(ink, px + (x - px) * t, py + (y - py) * t, 0.3f * 0.42f);
                }
            }
            px = x; py = y;
        }
    }
    /* inner rope: small epitrochoid ring */
    int n2 = n * 3;
    for (int k = 0; k < 6; k++) {
        int N = 2600;
        for (int i = 0; i < N; i++) {
            float th = i * 6.2831853f / N;
            float r = 30.5f + 1.6f * sinf(n2 * th + k * 1.0472f);
            ink_splat(ink, cx + r * cosf(th), cy + r * sinf(th), 0.3f * 0.75f * (2 * 3.14159f * r / N) / 0.3f);
        }
    }
    ink_circle(ink, cx, cy, 57.2f, 1.25f);
    ink_circle(ink, cx, cy, 54.6f, 0.8f);
    ink_circle(ink, cx, cy, 33.4f, 0.8f);
    ink_circle(ink, cx, cy, 27.6f, 1.2f);
    /* tiny pearls around the outer ring */
    for (int i = 0; i < 48; i++) {
        float a = i * 6.2831853f / 48;
        ink_diamond(ink, cx + 60.3f * cosf(a), cy + 60.3f * sinf(a), 0.9f);
    }
}

void ingot_gen_front(uint8_t *buf, const uint8_t *base, const mint_rec_t *mr, ingot_layout_t *lay)
{
    const purchase_t *p = &mr->p;
    uint8_t *ink = buf;
    memset(ink, 0, SCR_W * SCR_H);
    const int cx = SCR_W / 2;
    const float rcx = 85, rcy = 134;
    uint32_t seed = hash32((uint32_t)(p->price * 100 + 0.5) * 2654435761u ^ (uint32_t)p->buy_ts ^ (uint32_t)(p->buy_ts >> 32));

    /* Layout sits inside the engraved frame (plateau starts at BAR_BEVEL, frame at FRAME_IN). */
    (void)mr;
    ink_text(ink, &font_caps13, "ANTHROPIC", cx, 40, 3.2f);
    {
        int wl = text_width(&font_caps11, "NASDAQ", 1.6f), wr = text_width(&font_caps11, "ANT", 1.6f);
        int gap = 14, tot = wl + gap + wr, x0 = cx - tot / 2;
        inktx_t c = { ink, 1.0f };
        text_walk(&font_caps11, "NASDAQ", x0, 56, 1.6f, 0, SCR_H, ink_text_px, &c);
        text_walk(&font_caps11, "ANT", x0 + wl + gap, 56, 1.6f, 0, SCR_H, ink_text_px, &c);
        ink_diamond(ink, x0 + wl + gap / 2.0f, 52, 2.0f);
    }

    rosette(ink, rcx, rcy, seed);

    /* numeral in the polished cartouche */
    char num[12];
    snprintf(num, sizeof num, "%d", p->shares > 0 ? p->shares : 1);
    if (p->shares > 99) snprintf(num, sizeof num, "ANT");
    const glyph_t *g1 = font_glyph(&font_caps64, num[0]);
    int caph = g1->h;
    ink_text(ink, &font_caps64, num, (int)rcx, (int)(rcy + caph / 2.0f + 0.5f), 0);

    /* "ONE SHARE" */
    char share[24];
    int sh = p->shares > 0 ? p->shares : 1;
    if (sh <= 10) snprintf(share, sizeof share, "%s SHARE%s", NUMW[sh], sh > 1 ? "S" : "");
    else snprintf(share, sizeof share, "%d SHARES", sh);
    ink_text(ink, &font_caps13, share, cx, 216, 2.2f);

    /* separator */
    ink_seg(ink, 28, 226.5f, 76, 226.5f, 0.9f);
    ink_seg(ink, 94, 226.5f, 142, 226.5f, 0.9f);
    ink_diamond(ink, 85, 226.5f, 2.6f);

    /* price */
    char price[24];
    snprintf(price, sizeof price, "$%.2f", p->price);
    ink_text(ink, &font_caps26, price, cx, 258, 1.0f);

    /* date and time */
    char date[24], hm[8], hm_et[8];
    int64_t bj = p->buy_ts + LOCAL_UTC_OFFSET, et = p->buy_ts + us_et_offset(p->buy_ts);
    fmt_date(bj, date, sizeof date);
    fmt_hm(bj, hm, sizeof hm);
    fmt_hm(et, hm_et, sizeof hm_et);
    ink_text(ink, &font_caps13, date, cx, 279, 2.0f);
    {
        char l[16], r[16];
        snprintf(l, sizeof l, "%s BJT", hm);
        snprintf(r, sizeof r, "%s ET", hm_et);
        int wl = text_width(&font_caps11, l, 0.8f), wr = text_width(&font_caps11, r, 0.8f);
        int gap = 14, x0 = cx - (wl + gap + wr) / 2;
        inktx_t c = { ink, 1.0f };
        text_walk(&font_caps11, l, x0, 294, 0.8f, 0, SCR_H, ink_text_px, &c);
        text_walk(&font_caps11, r, x0 + wl + gap, 294, 0.8f, 0, SCR_H, ink_text_px, &c);
        ink_diamond(ink, x0 + wl + gap / 2.0f, 290, 1.8f);
    }

    /* ---- combine: grooves are cut into the base height; the cartouche is a low dome */
    for (int i = 0, y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++, i++) {
            int b = base[i];
            if (!b) { buf[i] = 0; continue; }
            float h = (float)b;
            float dx = x + 0.5f - rcx, dy = y + 0.5f - rcy;
            float r2 = dx * dx + dy * dy;
            if (r2 < 27.0f * 27.0f) h += 9 * (1 - r2 / (27.0f * 27.0f));
            float a = ink[i] / 255.0f;
            h -= GROOVE * a;
            int v = (int)(h + 0.5f);
            if (v < 1) v = 1;
            if (v > 127) v = 127;
            buf[i] = (uint8_t)(v | (a > 0.42f ? IG_RECESS : 0));
        }

    ingot_front_layout(lay, false);
}

void ingot_front_layout(ingot_layout_t *lay, bool inscription)
{
    (void)inscription;
    lay->rcx = 85; lay->rcy = 134; lay->rmax = 64;
    stamp_band_t b[] = {
        { 24, 60, 0, 0.8f },     /* ANTHROPIC / NASDAQ ANT */
        { 200, 231, 0, 0.8f },   /* ONE SHARE + separator */
        { 232, 263, 0, 1.4f },   /* price: the heavy blow */
        { 264, 299, 0, 0.9f },   /* date + time */
    };
    lay->nbands = 4;
    memcpy(lay->bands, b, sizeof b);
}

/* ------------------------------------------------------------------ engrave on a finished face */

typedef struct { uint8_t *buf; const font_t *f; } egctx_t;

static void eg_dark(void *c, int x, int y, int a, int ci)
{
    (void)ci;
    uint8_t *buf = ((egctx_t *)c)->buf;
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
    uint8_t *p = &buf[y * SCR_W + x];
    if (!*p) return;
    int v = IG_SHADE(*p) - GROOVE * a / 15;
    if (v < 1) v = 1;
    *p = (uint8_t)(v | ((*p & IG_RECESS) || a > 6 ? IG_RECESS : 0));
}

static void eg_light(void *c, int x, int y, int a, int ci)
{
    (void)ci;
    uint8_t *buf = ((egctx_t *)c)->buf;
    x += 1; y += 1;
    if ((unsigned)x >= SCR_W || (unsigned)y >= SCR_H) return;
    uint8_t *p = &buf[y * SCR_W + x];
    if (!*p) return;
    int v = IG_SHADE(*p) + a * 2;
    if (v > 127) v = 127;
    *p = (uint8_t)(v | (*p & IG_RECESS));
}

typedef struct { glyph_px_fn fn; void *inner; int only; } onlyctx_t;
static void only_px(void *c, int x, int y, int a, int ci)
{
    onlyctx_t *o = c;
    if (o->only < 0 || ci == o->only) o->fn(o->inner, x, y, a, ci);
}

void ingot_engrave_text(uint8_t *buf, const font_t *f, const char *s, float x, int baseline,
                        float track, int only_char)
{
    egctx_t c = { buf, f };
    onlyctx_t o1 = { eg_light, &c, only_char }, o2 = { eg_dark, &c, only_char };
    (void)o1;
    text_walk(f, s, x, baseline, track, 0, SCR_H, only_px, &o2);
}

/* ------------------------------------------------------------------ palettes */

void blackbody(float t, float *r, float *g, float *b)
{
    static const float K[][4] = {
        { 0.00f, 0.00f, 0.00f, 0.00f },
        { 0.15f, 0.35f, 0.02f, 0.00f },
        { 0.35f, 0.85f, 0.16f, 0.02f },
        { 0.55f, 1.00f, 0.45f, 0.07f },
        { 0.75f, 1.00f, 0.76f, 0.36f },
        { 1.00f, 1.00f, 0.97f, 0.90f },
    };
    t = clampf(t, 0, 1);
    int i = 0;
    while (i < 4 && t > K[i + 1][0]) i++;
    float u = (t - K[i][0]) / (K[i + 1][0] - K[i][0]);
    *r = lerpf(K[i][1], K[i + 1][1], u);
    *g = lerpf(K[i][2], K[i + 1][2], u);
    *b = lerpf(K[i][3], K[i + 1][3], u);
}

static void gold_ramp(float s, float *r, float *g, float *b)
{
    static const float K[][4] = {
        { 0.00f, 18, 11, 3 },
        { 0.22f, 84, 55, 17 },
        { 0.45f, 162, 114, 40 },
        { 0.65f, 212, 164, 74 },
        { 0.82f, 241, 206, 126 },
        { 1.00f, 255, 247, 218 },
    };
    s = clampf(s, 0, 1);
    int i = 0;
    while (i < 4 && s > K[i + 1][0]) i++;
    float u = (s - K[i][0]) / (K[i + 1][0] - K[i][0]);
    *r = lerpf(K[i][1], K[i + 1][1], u);
    *g = lerpf(K[i][2], K[i + 1][2], u);
    *b = lerpf(K[i][3], K[i + 1][3], u);
}

float patina_for_days(int days)
{
    if (days <= 1) return 0;
    return 1 - expf(-(days - 1) / 700.0f);
}

void lut_metal(uint16_t lut[256], const metal_params_t *p)
{
    float er, eg, eb;
    blackbody(p->heat, &er, &eg, &eb);
    float glow = clampf(p->heat * 1.4f, 0, 1);
    for (int i = 0; i < 256; i++) {
        int sh = i & 127;
        if (!sh) { lut[i] = 0; continue; }
        float s = sh / 127.0f;
        int rec = i & 128;
        /* raw casting: flatter, greyer */
        float sc = lerpf(0.22f + 0.5f * s, s, p->finish);
        float r, g, b;
        gold_ramp(sc, &r, &g, &b);
        float grey = (r + g + b) / 3;
        float desat = (1 - p->finish) * 0.55f;
        r = lerpf(r, grey * 1.05f, desat); g = lerpf(g, grey, desat); b = lerpf(b, grey * 0.9f, desat);
        if (rec) {
            r *= 0.93f; g *= 0.91f; b *= 0.88f;
            float pv = p->patina * 0.85f;
            float vr = 38 * (0.5f + 0.9f * s), vg = 92 * (0.5f + 0.9f * s), vb = 76 * (0.5f + 0.9f * s);
            r = lerpf(r, vr, pv); g = lerpf(g, vg, pv); b = lerpf(b, vb, pv);
        } else {
            float pv = p->patina * 0.5f * (1 - s * 0.6f);      /* high points stay rubbed bright */
            r = lerpf(r, r * 0.80f, pv); g = lerpf(g, g * 0.72f, pv); b = lerpf(b, b * 0.62f, pv);
        }
        if (p->cool > 0) {
            float c = p->cool;
            r = lerpf(r, 200 + 55 * s, c * 0.5f); g = lerpf(g, 210 + 45 * s, c * 0.5f); b = lerpf(b, 225 + 30 * s, c * 0.5f);
        }
        if (glow > 0) {
            float e = 255 * (0.55f + 0.45f * s) * glow;
            float keep = 1 - glow * 0.88f;
            r = r * keep + er * e; g = g * keep + eg * e; b = b * keep + eb * e;
        }
        lut[i] = rgb565((int)(r * p->gain), (int)(g * p->gain), (int)(b * p->gain));
    }
}

void lut_ledger(uint16_t lut[256])
{
    for (int i = 0; i < 128; i++) {         /* graphite plate */
        float s = i / 127.0f;
        lut[i] = rgb565((int)(8 + 70 * s), (int)(8 + 66 * s), (int)(10 + 62 * s));
    }
    for (int i = 0; i < 128; i++) {         /* gold inlay over mid graphite */
        float a = i / 127.0f, r, g, b;
        gold_ramp(0.62f + 0.3f * a, &r, &g, &b);
        lut[128 + i] = rgb565((int)lerpf(22, r, a), (int)lerpf(21, g, a), (int)lerpf(22, b, a));
    }
}

const char *roman(int n, char *out, int cap)
{
    static const int v[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    static const char *s[] = { "M", "CM", "D", "CD", "C", "XC", "L", "XL", "X", "IX", "V", "IV", "I" };
    int o = 0;
    out[0] = 0;
    if (n <= 0 || n > 3999) { snprintf(out, cap, "%d", n); return out; }
    for (int i = 0; i < 13; i++)
        while (n >= v[i] && o + 3 < cap) { int l = (int)strlen(s[i]); memcpy(out + o, s[i], l); o += l; n -= v[i]; }
    out[o] = 0;
    return out;
}

void ingot_engrave_rule(uint8_t *buf, int x0, int x1, int y)
{
    int cx = (x0 + x1) / 2;
    for (int x = x0; x <= x1; x++) {
        if (abs(x - cx) < 6) continue;
        uint8_t *p = &buf[y * SCR_W + x], *q = &buf[(y + 1) * SCR_W + x];
        (void)q;
        if (*p) *p = (uint8_t)((IG_SHADE(*p) - GROOVE * 3 / 4) | IG_RECESS);
    }
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            if (abs(dx) + abs(dy) > 3) continue;
            uint8_t *p = &buf[(y + dy) * SCR_W + cx + dx];
            if (*p) *p = (uint8_t)((IG_SHADE(*p) - GROOVE * 3 / 4) | IG_RECESS);
        }
}

mkt_t market_state(int64_t utc, int64_t *next_open, int64_t *close_at)
{
    int off = us_et_offset(utc);
    int64_t et = utc + off;
    int64_t day = et / 86400;
    int sod = (int)(et - day * 86400);
    int wd = (int)((day + 4) % 7);            /* 0 = Sunday */
    int mins = sod / 60;
    bool weekday = wd >= 1 && wd <= 5;
    mkt_t s = MKT_CLOSED;
    if (weekday) {
        if (mins >= 240 && mins < 570) s = MKT_PRE;
        else if (mins >= 570 && mins < 960) s = MKT_OPEN;
        else if (mins >= 960 && mins < 1200) s = MKT_AFTER;
    }
    if (close_at) *close_at = day * 86400 + 960 * 60 - off;
    if (next_open) {
        int64_t d = day;
        if (!weekday || mins >= 570) d++;
        while (((d + 4) % 7) == 0 || ((d + 4) % 7) == 6) d++;
        *next_open = d * 86400 + 570 * 60 - off;
    }
    return s;
}
