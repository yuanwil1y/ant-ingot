#include "scene.h"
#include "ingot.h"
#include "fonts.h"
#include "qrcodegen.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define STRIP_MAX 40
#define GL_N 420

enum { ST_SETUP, ST_READY, ST_MINT, ST_FACES, ST_ZOOM };
enum { F_FRONT, F_LINE, F_LEDGER, F_COUNT };

/* palette helpers */
#define C_INK      rgb565(46, 36, 22)
#define C_CARD     rgb565(238, 231, 214)
#define C_CARD_DIM rgb565(150, 136, 112)
#define C_GOLD_HI  rgb565(244, 210, 132)
#define C_GOLD     rgb565(214, 172, 96)
#define C_GOLD_DIM rgb565(150, 120, 72)
#define C_UP       rgb565(240, 198, 108)
#define C_DOWN     rgb565(150, 162, 178)
#define C_RED      rgb565(170, 52, 36)

static const char *FALLBACK_LINES[] = {
    "Gold does not check the price. It only keeps the date.",
    "Markets move. I stay exactly this heavy.",
    "Every day I am held, I become a little more yours.",
    "I was cast in a single shake. I intend to last longer.",
    "Patience is a kind of polish.",
    "Somewhere a chart is panicking. I am not a chart.",
    "One share, one day older, still entirely here.",
    "Let the ticker hurry. Metal keeps its own time.",
    "I remember the price you paid. That is enough.",
    "Today the light moved across me. Nothing else did.",
};
#define N_FALLBACK (int)(sizeof FALLBACK_LINES / sizeof FALLBACK_LINES[0])

typedef struct { char s[48]; float x; int y; int first; int n; } qline_t;

static struct {
    model_t *m;
    int st;
    float t;                    /* time in current state */
    float clock;                /* monotonic seconds */
    const uint8_t *base, *front;
    uint8_t *back;
    ingot_layout_t lay;
    uint16_t lut_front[256], lut_back[256];
    metal_params_t mp_front, mp_back;
    bool lut_front_ok, lut_back_ok;
    uint16_t bg_row[SCR_H];
    int8_t band_of_row[SCR_H];
    bool front_ready;           /* front engraved for current pending/minted data */
    purchase_t front_for;

    /* setup */
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    bool qr_ok;
    char qr_for[64];
    float chk_anim[3];          /* time since a check settled */
    chk_t chk_seen[3];
    setup_stage_t stage_seen;
    float stage_t;

    /* ready / heat */
    float heat;
    bool rehearsal;

    /* mint */
    float mt;                   /* mint timeline (designed seconds) */
    float dy, dyv;
    int stamps_done;
    float band_flash[6];
    float led_pulse;

    /* glint */
    int16_t glint[GL_N];
    bool glint_on;
    float g_t, g_dur, g_amp, g_w;
    int g_gx, g_gy;
    float g_cool;

    /* faces */
    int face, face_to;
    bool flipping;
    float flip_t, fth, fvel;
    float cooldown;
    bool need_calm;
    float calm_t;

    /* line face */
    char line[160];
    int line_for_day;
    qline_t ql[8];
    int nql, nchars;
    bool burin;
    float burin_pos;            /* characters engraved (float) */
    int burin_done;
    char header[40], footer[40];
    bool back_built;

    /* ledger / market face */
    int mpage, mpage_to;
    bool mfading;
    float mfade_t;
    int last_minute;
    double led_from[5], led_to[5];  /* price, chg%, value, gain%, x ipo */
    float ledger_t;
    bool ledger_anim, ledger_ready;
    double shown_price;
    bool have_shown;

    /* zoom */
    float zt, zlog, zlog_from;
    int zphase;                 /* 0 out, 1 hold, 2 back in */
    double ztotal;

    /* touch */
    bool pressed;
    float press_t;
    int px, py;
    bool long_fired;
    float ring_fade;

    /* new-day sweep */
    float sweep_t;
    bool sweeping;
    int sweep_from_day;

    float fade;                 /* boot fade-in */
    uint32_t events;
    bool dirty;
} S;

/* ================================================================== utilities */

static void build_bg(void)
{
    for (int y = 0; y < SCR_H; y++) {
        float t = (float)y / SCR_H;
        S.bg_row[y] = rgb565((int)(16 - 10 * t), (int)(13 - 9 * t), (int)(10 - 6 * t));
    }
}

static void glint_start(float dur, float amp, float w, int gx, int gy, float cool)
{
    S.glint_on = true;
    S.g_t = 0; S.g_dur = dur; S.g_amp = amp; S.g_w = w; S.g_gx = gx; S.g_gy = gy; S.g_cool = cool;
}

static void glint_update(void)
{
    if (!S.glint_on) { memset(S.glint, 0, sizeof S.glint); return; }
    int maxc = (SCR_W * S.g_gx + SCR_H * S.g_gy) / 10;
    float c = -S.g_w * 3 + (maxc + S.g_w * 6) * ease_in_out_cubic(S.g_t / S.g_dur);
    for (int i = 0; i < GL_N; i++) {
        float d = (i - c) / S.g_w;
        float v = d * d < 9 ? S.g_amp * expf(-d * d) : 0;
        S.glint[i] = (int16_t)v;
    }
}

static int display_days(void)
{
    if (S.m->tune.preview_days >= 0) return S.m->tune.preview_days;
    int d = days_held(S.m);
    if (S.sweeping) {
        float u = clampf(S.sweep_t / 1.6f, 0, 1);
        return S.sweep_from_day + (int)((d - S.sweep_from_day) * u);
    }
    if (d == 0) return S.m->patina_day;
    return d;
}

static void set_front_params(metal_params_t mp)
{
    if (!S.lut_front_ok || memcmp(&mp, &S.mp_front, sizeof mp)) {
        S.mp_front = mp;
        lut_metal(S.lut_front, &mp);
        S.lut_front_ok = true;
    }
}

static void set_back_params(metal_params_t mp)
{
    if (!S.lut_back_ok || memcmp(&mp, &S.mp_back, sizeof mp)) {
        S.mp_back = mp;
        lut_metal(S.lut_back, &mp);
        S.lut_back_ok = true;
    }
}

static void fmt_commas(double v, char *out, int cap)
{
    char tmp[32];
    snprintf(tmp, sizeof tmp, "%.0f", v);
    int n = (int)strlen(tmp), o = 0;
    for (int i = 0; i < n && o < cap - 2; i++) {
        out[o++] = tmp[i];
        int rem = n - 1 - i;
        if (rem && rem % 3 == 0) out[o++] = ',';
    }
    out[o] = 0;
}

static void fmt_pct_tiny(double pct, char *out, int cap)
{
    if (pct <= 0) { snprintf(out, cap, "0%%"); return; }
    int dec = (int)(-floor(log10(pct))) + 1;
    if (dec < 2) dec = 2;
    if (dec > 14) dec = 14;
    snprintf(out, cap, "%.*f%%", dec, pct);
}

static const char *MONS[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static void date_of(int64_t utc, char *out, int cap)
{
    int64_t days = (utc + LOCAL_UTC_OFFSET) / 86400 + 719468;
    int64_t era = days / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned mo = mp < 10 ? mp + 3 : mp - 9;
    int y = (int)(yoe + era * 400) + (mo <= 2);
    snprintf(out, cap, "%u %s %d", d, MONS[mo - 1], y);
}

/* ================================================================== surfaces */

static uint32_t seed_of(const purchase_t *p)
{
    return hash32((uint32_t)(p->price * 100 + 0.5) ^ (uint32_t)p->buy_ts);
}

static void build_band_rows(void)
{
    memset(S.band_of_row, -1, sizeof S.band_of_row);
    for (int b = 0; b < S.lay.nbands; b++)
        for (int y = S.lay.bands[b].y0; y <= S.lay.bands[b].y1 && y < SCR_H; y++) S.band_of_row[y] = (int8_t)b;
}

#define SURF_VERSION 4u
#define BASE_SEED 0x5eedu

static void ensure_base(void)
{
    if (S.base) return;
    uint32_t key = hash32(SURF_VERSION * 7919u ^ BASE_SEED);
    S.base = surf_lookup(0, key);
    if (S.base) return;
    uint8_t *buf = malloc(SCR_W * SCR_H);
    if (!buf) return;
    ingot_gen_base(buf, BASE_SEED, 0);
    S.base = surf_commit(0, key, buf);
}

static bool ensure_front(const purchase_t *p, uint32_t serial)
{
    if (S.front_ready && !memcmp(p, &S.front_for, sizeof *p)) return true;
    ingot_front_layout(&S.lay, p->inscription[0] != 0);
    build_band_rows();
    uint32_t key = hash32(SURF_VERSION ^ hash32((uint32_t)lrint(p->price * 100)) ^ hash32((uint32_t)p->buy_ts) ^
                          hash32((uint32_t)p->shares * 31u) ^ hash32(serial));
    for (const char *c = p->inscription; *c; c++) key = hash32(key ^ (uint8_t)*c);
    S.front = surf_lookup(1, key);
    if (!S.front) {
        ensure_base();
        uint8_t *buf = malloc(SCR_W * SCR_H);
        if (!buf || !S.base) { free(buf); return false; }
        mint_rec_t r = { .minted = true, .p = *p, .serial = serial };
        ingot_gen_front(buf, S.base, &r, &S.lay);
        S.front = surf_commit(1, key, buf);
        if (!S.front) return false;
    }
    S.front_for = *p;
    S.front_ready = true;
    return true;
}

/* ---------------------------------------------------------------- daily line */

static const char *current_line(int day)
{
    if (S.m->line[0] && S.m->line_day == day) return S.m->line;
    return FALLBACK_LINES[(day > 0 ? day : 0) % N_FALLBACK];
}

static void layout_line(const char *txt)
{
    const font_t *f = &font_ital17;
    const int maxw = 136;
    S.nql = 0;
    S.nchars = 0;
    char word[48];
    char cur[48] = "";
    const char *p = txt;
    while (*p && S.nql < 8) {
        while (*p == ' ') p++;
        int n = 0;
        while (p[n] && p[n] != ' ' && n < 46) n++;
        if (!n) break;
        memcpy(word, p, n);
        word[n] = 0;
        p += n;
        char trial[96];
        snprintf(trial, sizeof trial, "%s%s%s", cur, cur[0] ? " " : "", word);
        if (cur[0] && text_width(f, trial, 0.2f) > maxw) {
            snprintf(S.ql[S.nql].s, sizeof S.ql[0].s, "%s", cur);
            S.nql++;
            snprintf(cur, sizeof cur, "%s", word);
        } else {
            snprintf(cur, sizeof cur, "%s", trial);
        }
    }
    if (cur[0] && S.nql < 8) { snprintf(S.ql[S.nql].s, sizeof S.ql[0].s, "%s", cur); S.nql++; }
    const int lh = 23;
    int top = 168 - (S.nql * lh) / 2 + 12;
    for (int i = 0; i < S.nql; i++) {
        S.ql[i].x = SCR_W / 2.0f - text_width(f, S.ql[i].s, 0.2f) / 2.0f;
        S.ql[i].y = top + i * lh;
        S.ql[i].first = S.nchars;
        S.ql[i].n = (int)strlen(S.ql[i].s);
        S.nchars += S.ql[i].n;
    }
}

static void engrave_char(int k)
{
    for (int i = 0; i < S.nql; i++)
        if (k >= S.ql[i].first && k < S.ql[i].first + S.ql[i].n) {
            ingot_engrave_text(S.back, &font_ital17, S.ql[i].s, S.ql[i].x, S.ql[i].y, 0.2f, k - S.ql[i].first);
            return;
        }
}

static void char_pos(float k, float *x, float *y)
{
    int ki = (int)k;
    for (int i = 0; i < S.nql; i++)
        if (ki >= S.ql[i].first && ki < S.ql[i].first + S.ql[i].n) {
            char pre[48];
            int j = ki - S.ql[i].first;
            memcpy(pre, S.ql[i].s, j);
            pre[j] = 0;
            float x0 = S.ql[i].x + text_width(&font_ital17, pre, 0.2f) + (j ? 0.2f : 0);
            const glyph_t *g = font_glyph(&font_ital17, S.ql[i].s[j]);
            *x = x0 + g->adv16 / 16.0f * (k - ki);
            *y = S.ql[i].y - 5;
            return;
        }
    *x = SCR_W / 2.0f; *y = 170;
}

static void build_back(bool engraved)
{
    if (!S.back) S.back = malloc(SCR_W * SCR_H);
    if (!S.back) return;
    int day = days_held(S.m);
    ingot_gen_base(S.back, seed_of(&S.m->mint.p) ^ 0xBAC4, 1);
    char r[24];
    if (day > 0) snprintf(S.header, sizeof S.header, "DAY %s", roman(day, r, sizeof r));
    else snprintf(S.header, sizeof S.header, "HELD");
    int w = text_width(&font_caps13, S.header, 2.4f);
    ingot_engrave_text(S.back, &font_caps13, S.header, SCR_W / 2.0f - w / 2.0f, 40, 2.4f, -1);
    /* hairline with a diamond, engraved as text-ish strokes */
    ingot_engrave_rule(S.back, 50, 120, 56);
    if (S.m->now > 0) date_of(S.m->now, S.footer, sizeof S.footer);
    else snprintf(S.footer, sizeof S.footer, "ANT");
    w = text_width(&font_caps11, S.footer, 1.6f);
    ingot_engrave_text(S.back, &font_caps11, S.footer, SCR_W / 2.0f - w / 2.0f, 300, 1.6f, -1);
    snprintf(S.line, sizeof S.line, "%s", current_line(day));
    S.line_for_day = day;
    layout_line(S.line);
    if (engraved)
        for (int k = 0; k < S.nchars; k++) engrave_char(k);
    S.burin_done = engraved ? S.nchars : 0;
    S.burin_pos = (float)S.burin_done;
    S.back_built = true;
}

/* ================================================================== renderers */

/* Metal face: index image through palette, plus glint, stamp offset, mint reveal. */
static void render_metal(strip_t *st, const uint8_t *buf, const uint16_t *lut, bool minting)
{
    int dy = (int)lrintf(S.dy);
    int gx = S.g_gx, gy = S.g_gy;
    bool gl = S.glint_on;
    int rcx = (int)S.lay.rcx, rcy = (int)S.lay.rcy;
    float rr = 0, rin = 0;
    int bandt[6] = { 0 }, flash[6] = { 0 };
    if (minting) {
        float u = (S.mt - 0.2f) / 1.5f;
        rr = S.lay.rmax * ease_out_cubic(u) + (u > 0 ? 1 : 0);
        rin = rr - 9;
        for (int b = 0; b < S.lay.nbands; b++) {
            bandt[b] = b < S.stamps_done;
            flash[b] = (int)(S.band_flash[b] * 40);
        }
    }
    int r2 = (int)(rr * rr), rin2 = rin > 0 ? (int)(rin * rin) : 0;
    int hotk = minting ? (int)(255 * clampf(S.mp_front.heat * 1.6f, 0.25f, 1)) : 0;
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        int sy = y - dy;
        uint16_t *o = &st->px[yy * SCR_W];
        if (sy < 0 || sy >= SCR_H) { for (int x = 0; x < SCR_W; x++) o[x] = S.bg_row[y]; continue; }
        const uint8_t *row = &buf[sy * SCR_W];
        const uint8_t *brow = (minting && S.base) ? &S.base[sy * SCR_W] : row;
        int band = S.band_of_row[sy];
        int dyc = sy - rcy;
        bool rosrow = minting && dyc * dyc < (int)(S.lay.rmax * S.lay.rmax) + 64;
        int add_row = (minting && band >= 0) ? flash[band] : 0;
        bool row_revealed = !minting || (band >= 0 ? bandt[band] : !rosrow);
        int gbase = sy * gy;
        for (int x = 0; x < SCR_W; x++) {
            int idx;
            int add = add_row;
            int hot = 0;
            if (rosrow) {
                int dx = x - rcx;
                int d2 = dx * dx + dyc * dyc;
                if (d2 < r2) {
                    idx = row[x];
                    if (d2 >= rin2 && r2 > rin2) hot = (d2 - rin2) * 255 / (r2 - rin2);
                } else idx = brow[x];
            } else {
                idx = row_revealed ? row[x] : brow[x];
            }
            if (!idx) { o[x] = S.bg_row[y]; continue; }
            int s = idx & 127;
            if (gl) {
                int gi = ((x * gx + gbase) * 6554) >> 16;   /* /10 */
                if (gi >= 0 && gi < GL_N) s += (idx & 128) ? S.glint[gi] >> 2 : S.glint[gi];
            }
            s += add;
            if (s > 127) s = 127;
            if (s < 1) s = 1;
            uint16_t c = lut[(idx & 128) | s];
            if (hot) {
                int h = hot * hotk >> 8;
                c = add565(c, h, h * 6 / 10, h / 5);
            }
            o[x] = c;
        }
    }
}

/* ---------------------------------------------------------------- setup tag */

static void tag_card(strip_t *st, float y0, float y1, int alpha)
{
    /* shadow, card, punched hole */
    fill_rrect(st, 15, y0 + 3, 157, y1 + 4, 10, rgb565(0, 0, 0), alpha * 90 / 256);
    fill_rrect(st, 12, y0, 158, y1, 10, C_CARD, alpha);
    fill_rrect(st, 79, y0 + 8, 91, y0 + 20, 6, rgb565(90, 70, 40), alpha * 200 / 256);
}

static void draw_qr(strip_t *st, int cx, int top, int mod, int alpha)
{
    if (!S.qr_ok) return;
    int n = qrcodegen_getSize(S.qr);
    int x0 = cx - n * mod / 2;
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        int qy = y - top;
        if (qy < 0 || qy >= n * mod) continue;
        for (int qx = 0; qx < n * mod; qx++) {
            if (qrcodegen_getModule(S.qr, qx / mod, qy / mod)) {
                uint16_t *p = &st->px[yy * SCR_W + x0 + qx];
                *p = blend565(*p, C_INK, alpha);
            }
        }
    }
}

static void ring_arc(strip_t *st, float cx, float cy, float R, float w, float a0, float span, uint16_t c, int alpha)
{
    int ya = (int)(cy - R - w), yb = (int)(cy + R + w) + 1;
    if (ya < st->y0) ya = st->y0;
    if (yb > st->y0 + st->h) yb = st->y0 + st->h;
    for (int y = ya; y < yb; y++)
        for (int x = (int)(cx - R - w); x <= (int)(cx + R + w); x++) {
            if (x < 0 || x >= SCR_W) continue;
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float cov = clampf(w * 0.5f - fabsf(d - R) + 0.5f, 0, 1);
            if (cov <= 0) continue;
            if (span < 2 * PI) {
                float a = atan2f(dx, -dy) - a0;   /* 0 at 12 o'clock, clockwise */
                while (a < 0) a += 2 * PI;
                while (a >= 2 * PI) a -= 2 * PI;
                if (a > span) continue;
            }
            uint16_t *p = &st->px[(y - st->y0) * SCR_W + x];
            *p = blend565(*p, c, (int)(cov * alpha));
        }
}

static void check_icon(strip_t *st, float x, float y, chk_t c, float t, int alpha)
{
    if (c == CHK_PENDING) {
        fill_rrect(st, x - 3, y - 3, x + 3, y + 3, 3, C_CARD_DIM, alpha);
    } else if (c == CHK_RUNNING) {
        ring_arc(st, x, y, 6, 2, fmodf(S.clock * 6.0f, 2 * PI), 4.2f, C_INK, alpha);
    } else if (c == CHK_OK) {
        float p = ease_out_cubic(t / 0.35f);
        float p1 = clampf(p / 0.4f, 0, 1), p2 = clampf((p - 0.4f) / 0.6f, 0, 1);
        draw_seg(st, x - 6, y, x - 6 + 4 * p1, y + 4 * p1, 2.2f, rgb565(60, 120, 60), alpha);
        if (p2 > 0) draw_seg(st, x - 2, y + 4, x - 2 + 9 * p2, y + 4 - 10 * p2, 2.2f, rgb565(60, 120, 60), alpha);
    } else {
        float sh = t < 0.4f ? sinf(t * 40) * 3 * (1 - t / 0.4f) : 0;
        draw_seg(st, x - 5 + sh, y - 5, x + 5 + sh, y + 5, 2.2f, C_RED, alpha);
        draw_seg(st, x + 5 + sh, y - 5, x - 5 + sh, y + 5, 2.2f, C_RED, alpha);
    }
}

static void render_setup_overlay(strip_t *st)
{
    setup_t *su = &S.m->setup;
    int cx = SCR_W / 2;
    text_draw_centered(st, &font_caps13, "UNMINTED", cx, 40, 3.0f, rgb565(40, 32, 22), 200);
    if (su->stage == SETUP_CHECKING) {
        tag_card(st, 80, 236, 256);
        text_draw_centered(st, &font_caps11, "CHECKING", cx, 118, 2.0f, C_INK, 256);
        static const char *names[3] = { "Wi-Fi", "Market data", "DeepSeek" };
        for (int i = 0; i < 3; i++) {
            int y = 150 + i * 28;
            text_draw(st, &font_serif13, names[i], 50, y + 4, 0, C_INK, 256);
            check_icon(st, 36, y, su->chk[i], S.chk_anim[i], 256);
        }
        return;
    }
    int qn = S.qr_ok ? qrcodegen_getSize(S.qr) : 29;
    int mod = qn * 4 <= 112 ? 4 : 3;
    tag_card(st, 62, 108 + qn * mod + 60, 256);
    text_draw_centered(st, &font_caps11, "SCAN TO BEGIN", cx, 100, 1.6f, C_INK, 256);
    draw_qr(st, cx, 108, mod, 256);
    int ty = 108 + qn * mod + 18;
    text_draw_centered(st, &font_serif13, "Join Wi-Fi", cx, ty, 0, C_CARD_DIM, 256);
    text_draw_centered(st, &font_serif13, su->ap_ssid, cx, ty + 15, 0, C_INK, 256);
    if (su->stage == SETUP_FAILED && su->error[0]) {
        fill_rrect(st, 10, 272, 160, 300, 6, rgb565(60, 18, 12), 220);
        text_draw_centered(st, &font_serif13, su->error, cx, 290, 0, rgb565(250, 214, 200), 256);
    } else {
        text_draw_centered(st, &font_serif13, "then follow the page", cx, ty + 33, 0, C_CARD_DIM, 256);
    }
}

static void render_ready_overlay(strip_t *st)
{
    const purchase_t *p = &S.m->setup.pending;
    int a = (int)(256 * (1 - smoothstep01((S.heat - 0.04f) / 0.3f)));
    if (a <= 0) return;
    int cx = SCR_W / 2;
    tag_card(st, 70, 256, a);
    text_draw_centered(st, &font_caps11, S.rehearsal ? "REHEARSAL" : "READY TO MINT", cx, 108, 1.6f, C_INK, a);
    char b[40];
    snprintf(b, sizeof b, "$%.2f", p->price);
    text_draw_centered(st, &font_caps26, b, cx, 146, 1, C_INK, a);
    int sh = p->shares > 0 ? p->shares : 1;
    snprintf(b, sizeof b, "%d SHARE%s", sh, sh > 1 ? "S" : "");
    text_draw_centered(st, &font_caps11, b, cx, 164, 1.6f, C_CARD_DIM, a);
    date_of(p->buy_ts, b, sizeof b);
    text_draw_centered(st, &font_caps13, b, cx, 186, 1.6f, C_INK, a);
    float pulse = 0.55f + 0.45f * sinf(S.clock * 3.2f);
    text_draw_centered(st, &font_caps13, "SHAKE TO MINT", cx, 220, 1.2f, rgb565(150, 72, 20), (int)(a * pulse));
    text_draw_centered(st, &font_serif13, S.rehearsal ? "nothing will be saved" : "engraving is permanent", cx, 238, 0, C_CARD_DIM, a);
}

/* ---------------------------------------------------------------- line face */

static void render_line_face(strip_t *st)
{
    if (!S.back) { for (int i = 0; i < SCR_W * st->h; i++) st->px[i] = 0; return; }
    render_metal(st, S.back, S.lut_back, false);
    if (S.burin && S.burin_done < S.nchars) {
        float x, y;
        char_pos(S.burin_pos, &x, &y);
        float fl = 0.75f + 0.25f * sinf(S.clock * 53) * sinf(S.clock * 31);
        glow_add(st, x, y, 9, 255, 220, 160, 0.55f * fl);
        glow_add(st, x, y, 3.2f, 255, 255, 240, 1.0f);
    }
}

/* ---------------------------------------------------------------- ledger */

static void draw_text_clip(strip_t *st, const font_t *f, const char *s, float x, int base, uint16_t c, int alpha, int cy0, int cy1)
{
    strip_t sub = *st;
    int y0 = cy0 > st->y0 ? cy0 : st->y0;
    int y1 = cy1 < st->y0 + st->h ? cy1 : st->y0 + st->h;
    if (y1 <= y0) return;
    sub.px = st->px + (y0 - st->y0) * SCR_W;
    sub.y0 = y0;
    sub.h = y1 - y0;
    text_draw(&sub, f, s, x, base, 0, c, alpha);
}

static void odometer(strip_t *st, const font_t *f, const char *from, const char *to, int cx, int base, float t, bool up, uint16_t c)
{
    int n = (int)strlen(to);
    if ((int)strlen(from) != n || t >= 1) { text_draw_centered(st, f, to, cx, base, 0, c, 256); return; }
    float x = cx - text_width(f, to, 0) / 2.0f;
    int lh = f->asc + 2;
    int cy0 = base - f->asc + 6, cy1 = base + 4;
    int k = 0;
    for (int i = 0; i < n; i++) {
        char ch[2] = { to[i], 0 };
        float adv = font_glyph(f, to[i])->adv16 / 16.0f;
        if (to[i] >= '0' && to[i] <= '9' && from[i] >= '0' && from[i] <= '9') {
            int a = from[i] - '0', b = to[i] - '0';
            int steps = up ? (b - a + 10) % 10 : -((a - b + 10) % 10);
            float ti = clampf((t - 0.06f * k) / 0.7f, 0, 1);
            k++;
            float pos = a + steps * ease_out_back(ti, 0.9f);
            float fl = floorf(pos);
            float fr = pos - fl;
            int d0 = (((int)fl % 10) + 10) % 10, d1 = (d0 + 1) % 10;
            char c0[2] = { (char)('0' + d0), 0 }, c1[2] = { (char)('0' + d1), 0 };
            draw_text_clip(st, f, c0, x, base - (int)lrintf(fr * lh), c, 256, cy0, cy1);
            draw_text_clip(st, f, c1, x, base + (int)lrintf((1 - fr) * lh), c, 256, cy0, cy1);
        } else {
            text_draw(st, f, ch, x, base, 0, c, 256);
        }
        x += adv;
    }
}

/* ---------------------------------------------------------------- market face
 * Black enamel window in the reverse side. Tap cycles three pages:
 * 0 today (intraday), 1 since you bought, 2 since IPO. */

#define CH_X0 22
#define CH_Y0 110
#define CH_W 126
#define CH_H 90
#define PANEL_X0 13
#define PANEL_X1 157
#define PANEL_Y0 13
#define PANEL_Y1 307

typedef struct { float x, v; } cpt_t;

static int64_t et_day_of(int64_t utc) { return (utc + us_et_offset(utc)) / 86400; }

static int ymd_to_days(int ymd)
{
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

/* Collect the points of a page's chart (x in 0..1). Returns count. */
static int chart_points(int page, cpt_t *pt, int cap, float *ref1, float *ref2, float *mark_x, float *mark_v)
{
    const model_t *m = S.m;
    const intraday_t *in = &m->intra;
    const series_t *se = &m->daily;
    const purchase_t *p = &m->mint.p;
    const quote_t *q = &m->quote;
    int n = 0;
    *ref1 = *ref2 = NAN;
    *mark_x = -1;
    *mark_v = 0;
    if (page == 0) {
        for (int i = 0; i < in->n && n < cap; i++) pt[n++] = (cpt_t){ in->min[i] / 390.0f, in->px[i] };
        *ref1 = (float)p->price;
        if (q->valid && q->prev_close > 0) *ref2 = (float)q->prev_close;
        int64_t bd = et_day_of(p->buy_ts);
        if (in->n && ymd_to_days(in->date) == bd) {
            int64_t bet = p->buy_ts + us_et_offset(p->buy_ts);
            int bm = (int)((bet % 86400) / 60) - 570;
            if (bm >= 0 && bm <= 390) { *mark_x = bm / 390.0f; *mark_v = (float)p->price; }
        }
        return n;
    }
    if (page == 1) {
        int64_t bd = et_day_of(p->buy_ts);
        *ref1 = (float)p->price;
        int cnt = 0;
        for (int i = 0; i < se->n; i++) if (se->day[i] > bd) cnt++;
        if (cnt < 2 && in->n && ymd_to_days(in->date) >= bd) {
            /* young holding: use today's minutes after the purchase */
            int bm = -1;
            if (ymd_to_days(in->date) == bd) {
                int64_t bet = p->buy_ts + us_et_offset(p->buy_ts);
                bm = (int)((bet % 86400) / 60) - 570;
            }
            pt[n++] = (cpt_t){ 0, (float)p->price };
            int first = -1, last = in->n - 1;
            for (int i = 0; i < in->n; i++) if (in->min[i] >= bm) { first = i; break; }
            if (first >= 0) {
                float span = (float)(in->min[last] - in->min[first] + 1);
                for (int i = first; i <= last && n < cap; i++) pt[n++] = (cpt_t){ (in->min[i] - in->min[first] + 1) / span, in->px[i] };
            }
        } else {
            pt[n++] = (cpt_t){ 0, (float)p->price };
            for (int i = 0; i < se->n && n < cap - 1; i++) if (se->day[i] > bd) pt[n++] = (cpt_t){ 0, se->close[i] };
            if (q->valid) pt[n++] = (cpt_t){ 0, (float)q->price };
            for (int i = 0; i < n; i++) pt[i].x = n > 1 ? (float)i / (n - 1) : 0;
        }
        *mark_x = 0;
        *mark_v = (float)p->price;
        return n;
    }
    /* since IPO: offer price, then every close, then now */
    *ref1 = (float)IPO_PRICE;
    pt[n++] = (cpt_t){ 0, (float)IPO_PRICE };
    int64_t bd = et_day_of(p->buy_ts);
    int buy_idx = -1;
    for (int i = 0; i < se->n && n < cap - 1; i++) {
        if (buy_idx < 0 && se->day[i] >= bd) buy_idx = n;
        pt[n++] = (cpt_t){ 0, se->close[i] };
    }
    if (q->valid) {
        if (!(se->n && se->day[se->n - 1] >= et_day_of(m->now > 0 ? m->now : p->buy_ts) && fabsf(se->close[se->n - 1] - (float)q->price) < 0.005f))
            pt[n++] = (cpt_t){ 0, (float)q->price };
    }
    if (buy_idx < 0) buy_idx = n - 1;
    for (int i = 0; i < n; i++) pt[i].x = n > 1 ? (float)i / (n - 1) : 0;
    *mark_x = pt[buy_idx].x;
    *mark_v = (float)p->price;
    return n;
}

static struct {
    uint8_t *mask;          /* CH_W*CH_H: hi nibble = line, lo nibble = fill */
    int page;
    uint32_t rev;
    bool ok;
    int n;
    float lo, hi;
    float ref1, ref2, mark_x, mark_v, end_x, end_v;
} CHT = { .page = -1 };

static float chart_y(float v) { return CH_H - 6 - (v - CHT.lo) / (CHT.hi - CHT.lo) * (CH_H - 12); }

static void build_chart(int page)
{
    static cpt_t pt[INTRA_MAX + 2];
    if (!CHT.mask) CHT.mask = malloc(CH_W * CH_H);
    if (!CHT.mask) return;
    memset(CHT.mask, 0, CH_W * CH_H);
    CHT.page = page;
    CHT.rev = S.m->intra.rev * 131u + S.m->daily.rev * 7u + (uint32_t)(S.m->quote.price * 100);
    int n = chart_points(page, pt, INTRA_MAX + 2, &CHT.ref1, &CHT.ref2, &CHT.mark_x, &CHT.mark_v);
    CHT.n = n;
    CHT.ok = n >= 2;
    if (!CHT.ok) return;
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < n; i++) { lo = fminf(lo, pt[i].v); hi = fmaxf(hi, pt[i].v); }
    if (!isnan(CHT.ref1)) { lo = fminf(lo, CHT.ref1); hi = fmaxf(hi, CHT.ref1); }
    if (!isnan(CHT.ref2)) { lo = fminf(lo, CHT.ref2); hi = fmaxf(hi, CHT.ref2); }
    float pad = fmaxf((hi - lo) * 0.08f, 0.05f);
    CHT.lo = lo - pad;
    CHT.hi = hi + pad;
    /* line: AA distance to segments */
    static float colmin[CH_W];
    for (int x = 0; x < CH_W; x++) colmin[x] = 1e9f;
    for (int i = 1; i < n; i++) {
        float ax = pt[i - 1].x * (CH_W - 1), ay = chart_y(pt[i - 1].v);
        float bx = pt[i].x * (CH_W - 1), by = chart_y(pt[i].v);
        int x0 = (int)floorf(fminf(ax, bx)) - 2, x1 = (int)ceilf(fmaxf(ax, bx)) + 2;
        int y0 = (int)floorf(fminf(ay, by)) - 2, y1 = (int)ceilf(fmaxf(ay, by)) + 2;
        float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
        for (int y = y0; y <= y1; y++) {
            if (y < 0 || y >= CH_H) continue;
            for (int x = x0; x <= x1; x++) {
                if (x < 0 || x >= CH_W) continue;
                float px = x + 0.5f - ax, py = y + 0.5f - ay;
                float t = l2 > 0 ? clampf((px * dx + py * dy) / l2, 0, 1) : 0;
                float ex = px - t * dx, ey = py - t * dy;
                float cov = clampf(1.3f - sqrtf(ex * ex + ey * ey), 0, 1);
                int a = (int)(cov * 15 + 0.5f);
                uint8_t *mp = &CHT.mask[y * CH_W + x];
                if (a > (*mp >> 4)) *mp = (uint8_t)((a << 4) | (*mp & 15));
            }
        }
        /* track the line's y per column for the fill */
        for (int x = (int)fminf(ax, bx); x <= (int)fmaxf(ax, bx) && x < CH_W; x++) {
            if (x < 0) continue;
            float t = dx != 0 ? clampf((x + 0.5f - ax) / dx, 0, 1) : 0;
            float yy = ay + dy * t;
            if (yy < colmin[x]) colmin[x] = yy;
        }
    }
    for (int x = 0; x < CH_W; x++) {
        if (colmin[x] > 1e8f) continue;
        for (int y = (int)colmin[x] + 1; y < CH_H; y++) {
            float f = 1 - (y - colmin[x]) / (CH_H - colmin[x]);
            int a = (int)(f * f * 15);
            uint8_t *mp = &CHT.mask[y * CH_W + x];
            *mp = (uint8_t)((*mp & 0xF0) | a);
        }
    }
    CHT.end_x = pt[n - 1].x * (CH_W - 1);
    CHT.end_v = pt[n - 1].v;
}

static uint32_t chart_rev_now(void)
{
    return S.m->intra.rev * 131u + S.m->daily.rev * 7u + (uint32_t)(S.m->quote.price * 100);
}

static void draw_chart(strip_t *st, int alpha, int yoff)
{
    if (!CHT.ok || !CHT.mask || alpha <= 0) return;
    uint16_t line = C_GOLD_HI, fill = rgb565(150, 110, 40);
    int y0 = CH_Y0 + yoff;
    int r1 = isnan(CHT.ref1) ? -999 : (int)lrintf(chart_y(CHT.ref1));
    int r2 = isnan(CHT.ref2) ? -999 : (int)lrintf(chart_y(CHT.ref2));
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy, cy = y - y0;
        if (cy < 0 || cy >= CH_H) continue;
        uint16_t *o = &st->px[yy * SCR_W + CH_X0];
        const uint8_t *mr = &CHT.mask[cy * CH_W];
        bool d1 = cy == r1, d2 = cy == r2;
        for (int x = 0; x < CH_W; x++) {
            uint16_t c = o[x];
            if ((d1 || d2) && ((x >> 2) & 1) == 0) c = blend565(c, d1 ? C_GOLD : C_GOLD_DIM, (d1 ? 150 : 110) * alpha >> 8);
            int f = mr[x] & 15, l = mr[x] >> 4;
            if (f) c = blend565(c, fill, (f * 7 * alpha) >> 8);
            if (l) c = blend565(c, line, (l * 17 * alpha) >> 8);
            o[x] = c;
        }
    }
    /* now: a warm point at the end of the line */
    float ex = CH_X0 + CHT.end_x + 0.5f, ey = y0 + chart_y(CHT.end_v);
    glow_add(st, ex, ey, 7, 255, 210, 140, 0.35f * alpha / 256);
    fill_rrect(st, ex - 2, ey - 2, ex + 2, ey + 2, 2, rgb565(255, 244, 214), alpha);
    /* your purchase: a tiny ingot */
    if (CHT.mark_x >= 0) {
        float mx = CH_X0 + CHT.mark_x * (CH_W - 1) + 0.5f, my = y0 + chart_y(CHT.mark_v);
        fill_rrect(st, mx - 4.5f, my - 3, mx + 4.5f, my + 3, 1.5f, rgb565(0, 0, 0), alpha * 160 >> 8);
        fill_rrect(st, mx - 3.5f, my - 2, mx + 3.5f, my + 2, 1, rgb565(236, 196, 104), alpha);
    }
}

static void fmt_big(double v, char *out, int cap)
{
    if (v >= 1e12) snprintf(out, cap, "%.2fT", v / 1e12);
    else if (v >= 1e9) snprintf(out, cap, "%.2fB", v / 1e9);
    else if (v >= 1e6) snprintf(out, cap, "%.1fM", v / 1e6);
    else snprintf(out, cap, "%.0f", v);
}

static void fmt_dur(int64_t s, char *out, int cap)
{
    if (s < 0) s = 0;
    int h = (int)(s / 3600), mi = (int)(s / 60 % 60);
    if (h >= 24) snprintf(out, cap, "%dD %dH", h / 24, h % 24);
    else snprintf(out, cap, "%dH %02dM", h, mi);
}

static void row_lr(strip_t *st, int y, const char *l, const char *r, uint16_t rc, int a)
{
    text_draw(st, &font_caps11, l, 26, y, 1.4f, C_GOLD_DIM, a);
    text_draw(st, &font_mono11, r, 145 - text_width(&font_mono11, r, 0), y, 0, rc, a);
}

static void render_enamel(strip_t *st)
{
    if (S.back) render_metal(st, S.back, S.lut_back, false);
    const float X0 = PANEL_X0, X1 = PANEL_X1, Y0 = PANEL_Y0, Y1 = PANEL_Y1, R = 9;
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        float fy = y + 0.5f;
        if (fy < Y0 - 1 || fy > Y1 + 1) continue;
        float inset = 0;
        if (fy < Y0 + R) { float d = Y0 + R - fy; inset = R - sqrtf(fmaxf(0, R * R - d * d)); }
        else if (fy > Y1 - R) { float d = fy - (Y1 - R); inset = R - sqrtf(fmaxf(0, R * R - d * d)); }
        float xa = X0 + inset, xb = X1 - inset;
        float vcov = clampf(fminf(fy - Y0, Y1 - fy) + 0.5f, 0, 1);
        float t = (y - Y0) / (Y1 - Y0);
        int base = (int)(14 + 8 * (1 - t));
        if (y < Y0 + 5) base -= (int)((Y0 + 5 - y) * 2);
        uint16_t ec = rgb565(base, base - 1, base + 1);
        uint16_t *o = &st->px[yy * SCR_W];
        for (int x = (int)xa; x <= (int)xb && x < SCR_W; x++) {
            float cov = vcov * clampf(fminf(x + 1 - xa, xb - x), 0, 1);
            o[x] = blend565(o[x], ec, (int)(cov * 256));
        }
    }
}

static void page_today(strip_t *st, int a, int yo)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    int cx = SCR_W / 2;
    char s1[40], s2[40];
    int64_t now = S.m->now, nopen = 0, close_at = 0;
    mkt_t ms = now > 0 ? market_state(now, &nopen, &close_at) : MKT_CLOSED;
    static const char *MS[] = { "CLOSED", "PRE", "OPEN", "AFTER" };
    text_draw(st, &font_caps11, "TODAY", 26, 36 + yo, 2.0f, C_GOLD_DIM, a);
    {
        const char *t = MS[ms];
        int w = text_width(&font_caps11, t, 1.6f);
        uint16_t c = ms == MKT_OPEN ? C_GOLD_HI : (ms == MKT_CLOSED ? rgb565(120, 112, 100) : C_GOLD);
        text_draw(st, &font_caps11, t, 145 - w, 36 + yo, 1.6f, c, a);
        fill_rrect(st, 145 - w - 9, 30 + yo, 145 - w - 4, 35 + yo, 2.5f, c, a);
    }
    float t = S.ledger_anim ? clampf(S.ledger_t / 0.9f, 0, 1) : (S.ledger_ready ? 0 : 1);
    float e = ease_out_cubic(t);
    if (q->valid) {
        if (a >= 250) {
            snprintf(s1, sizeof s1, "$%.2f", S.led_from[0]);
            snprintf(s2, sizeof s2, "$%.2f", S.led_to[0]);
            odometer(st, &font_mono32, s1, s2, cx, 80 + yo, t, S.led_to[0] >= S.led_from[0], C_GOLD_HI);
        } else {
            snprintf(s2, sizeof s2, "$%.2f", q->price);
            text_draw_centered(st, &font_mono32, s2, cx, 80 + yo, 0, C_GOLD_HI, a);
        }
        double chg = S.led_from[1] + (S.led_to[1] - S.led_from[1]) * e;
        snprintf(s1, sizeof s1, "%+.2f%%   %+.2f", chg, q->price - q->prev_close);
        text_draw_centered(st, &font_mono11, s1, cx, 99 + yo, 0, chg >= 0 ? C_UP : C_DOWN, a);
    } else {
        text_draw_centered(st, &font_mono32, "$--.--", cx, 80 + yo, 0, C_GOLD_DIM, a);
    }
    if (CHT.ok) draw_chart(st, a, yo);
    else text_draw_centered(st, &font_mono11, "no session yet", cx, CH_Y0 + CH_H / 2 + yo, 0, C_GOLD_DIM, a);
    if (q->valid && q->high > q->low) {
        /* day range */
        int y = 214 + yo;
        float u = clampf((float)((q->price - q->low) / (q->high - q->low)), 0, 1);
        fill_rrect(st, 26, y - 1, 145, y + 1, 1, rgb565(70, 60, 44), a);
        float cpos = 26 + u * 119;
        fill_rrect(st, 26, y - 1, cpos, y + 1, 1, C_GOLD_DIM, a);
        fill_rrect(st, cpos - 2.5f, y - 4, cpos + 2.5f, y + 4, 1.5f, C_GOLD_HI, a);
        float cu = (float)((p->price - q->low) / (q->high - q->low));
        if (cu >= 0 && cu <= 1) fill_rrect(st, 26 + cu * 119 - 0.6f, y - 5, 26 + cu * 119 + 0.6f, y + 5, 0, C_GOLD, a);
        snprintf(s1, sizeof s1, "L %.2f", q->low);
        snprintf(s2, sizeof s2, "H %.2f", q->high);
        text_draw(st, &font_mono11, s1, 26, y + 15, 0, C_GOLD_DIM, a);
        text_draw(st, &font_mono11, s2, 145 - text_width(&font_mono11, s2, 0), y + 15, 0, C_GOLD_DIM, a);
        fmt_big(q->volume, s2, sizeof s2);
        row_lr(st, 250 + yo, "VOLUME", s2, C_GOLD, a);
        snprintf(s2, sizeof s2, "%.2f", q->prev_close);
        row_lr(st, 265 + yo, "PREV", s2, C_GOLD, a);
    }
    if ((ms == MKT_PRE || ms == MKT_AFTER) && q->ext_price > 0) {
        snprintf(s1, sizeof s1, "%s $%.2f %+.2f%%", ms == MKT_PRE ? "PRE" : "AFTER", q->ext_price, q->ext_pct);
        text_draw_centered(st, &font_mono11, s1, cx, 281 + yo, 0, q->ext_pct >= 0 ? C_UP : C_DOWN, a);
    } else if (now > 0) {
        if (ms == MKT_OPEN) { fmt_dur(close_at - now, s2, sizeof s2); snprintf(s1, sizeof s1, "CLOSES IN %s", s2); }
        else { fmt_dur(nopen - now, s2, sizeof s2); snprintf(s1, sizeof s1, "OPENS IN %s", s2); }
        text_draw_centered(st, &font_caps11, s1, cx, 281 + yo, 1.2f, rgb565(130, 114, 88), a);
    }
}

static void page_held(strip_t *st, int a, int yo)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    int cx = SCR_W / 2;
    char s1[40], s2[40];
    text_draw_centered(st, &font_caps11, "SINCE YOU BOUGHT", cx, 36 + yo, 0.4f, C_GOLD_DIM, a);
    if (q->valid) {
        double g = (q->price / p->price - 1) * 100;
        snprintf(s1, sizeof s1, "%+.2f%%", g);
        text_draw_centered(st, &font_mono32, s1, cx, 80 + yo, 0, g >= 0 ? C_UP : C_DOWN, a);
        snprintf(s1, sizeof s1, "$%.2f  >  $%.2f", p->price, q->price);
        text_draw_centered(st, &font_mono11, s1, cx, 99 + yo, 0, C_GOLD_DIM, a);
    }
    if (CHT.ok) draw_chart(st, a, yo);
    int d = days_held(S.m);
    snprintf(s2, sizeof s2, d > 0 ? "%d" : "--", d);
    row_lr(st, 222 + yo, "DAYS HELD", s2, C_GOLD, a);
    /* best and worst close while held, from the chart's own points */
    if (CHT.ok && CHT.page == 1) {
        static cpt_t pt[INTRA_MAX + 2];
        float r1, r2, mx, mv;
        int n = chart_points(1, pt, INTRA_MAX + 2, &r1, &r2, &mx, &mv);
        float hi = -1e9f, lo = 1e9f;
        for (int i = 0; i < n; i++) { hi = fmaxf(hi, pt[i].v); lo = fminf(lo, pt[i].v); }
        snprintf(s2, sizeof s2, "%.2f", hi);
        row_lr(st, 240 + yo, "HIGHEST", s2, C_GOLD, a);
        snprintf(s2, sizeof s2, "%.2f", lo);
        row_lr(st, 258 + yo, "LOWEST", s2, C_GOLD, a);
    }
    if (q->valid) {
        int sh = p->shares > 0 ? p->shares : 1;
        snprintf(s2, sizeof s2, "%+.2f", (q->price - p->price) * sh);
        row_lr(st, 276 + yo, "P / L  $", s2, q->price >= p->price ? C_UP : C_DOWN, a);
    }
}

static void page_ipo(strip_t *st, int a, int yo)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    int cx = SCR_W / 2;
    char s1[40], s2[40];
    text_draw_centered(st, &font_caps11, "SINCE IPO", cx, 36 + yo, 2.0f, C_GOLD_DIM, a);
    if (q->valid) {
        snprintf(s1, sizeof s1, "x%.2f", q->price / IPO_PRICE);
        text_draw_centered(st, &font_mono32, s1, cx, 80 + yo, 0, q->price >= IPO_PRICE ? C_UP : C_DOWN, a);
    }
    text_draw_centered(st, &font_mono11, "IPO $83.00  NOV 19", cx, 99 + yo, 0, C_GOLD_DIM, a);
    if (CHT.ok) draw_chart(st, a, yo);
    double total = (q->valid && q->total_shares > 1e6) ? q->total_shares : DEFAULT_TOTAL_SHARES;
    if (q->valid) {
        fmt_big(total * q->price, s2, sizeof s2);
        char s3[24];
        snprintf(s3, sizeof s3, "$%s", s2);
        row_lr(st, 222 + yo, "MKT CAP", s3, C_GOLD, a);
    }
    snprintf(s2, sizeof s2, "%.2f", p->price);
    row_lr(st, 240 + yo, "ENTRY", s2, C_GOLD, a);
    fmt_pct_tiny(100.0 * (p->shares > 0 ? p->shares : 1) / total, s1, sizeof s1);
    text_draw_centered(st, &font_caps11, "YOU OWN", cx, 262 + yo, 1.4f, C_GOLD_DIM, a);
    text_draw_centered(st, &font_mono11, s1, cx, 277 + yo, 0, C_GOLD, a);
}

static void render_ledger(strip_t *st)
{
    render_enamel(st);
    if (st->y0 > PANEL_Y1 || st->y0 + st->h < PANEL_Y0) return;
    /* page crossfade: out (0..0.12s), switch, in (0.12..0.30s) */
    int a = 256, yo = 0;
    if (S.mfading) {
        float t = S.mfade_t;
        if (t < 0.12f) a = (int)(256 * (1 - t / 0.12f));
        else { float u = clampf((t - 0.12f) / 0.18f, 0, 1); a = (int)(256 * ease_out_cubic(u)); yo = (int)lrintf(6 * (1 - ease_out_cubic(u))); }
    }
    int pg = S.mpage;
    if (pg == 0) page_today(st, a, yo);
    else if (pg == 1) page_held(st, a, yo);
    else page_ipo(st, a, yo);
    /* page dots */
    for (int i = 0; i < 3; i++) {
        float x = SCR_W / 2.0f + (i - 1) * 10;
        fill_rrect(st, x - 2, 296, x + 2, 300, 2, i == S.mpage ? C_GOLD_HI : rgb565(70, 60, 44), 256);
    }
    /* a big day gets a small mark, nothing louder */
    const quote_t *q = &S.m->quote;
    if (q->valid && fabs(q->chg_pct) >= 5) {
        uint16_t c = q->chg_pct >= 0 ? C_UP : C_DOWN;
        draw_seg(st, 149, 18, 152, 21, 1.6f, c, 256);
        draw_seg(st, 152, 21, 149, 24, 1.6f, c, 256);
        draw_seg(st, 149, 24, 146, 21, 1.6f, c, 256);
        draw_seg(st, 146, 21, 149, 18, 1.6f, c, 256);
    }
}

/* ---------------------------------------------------------------- faces + flip */

static void render_face(int f, strip_t *st)
{
    if (f == F_FRONT) {
        if (S.front) render_metal(st, S.front, S.lut_front, false);
    } else if (f == F_LINE) {
        render_line_face(st);
    } else {
        render_ledger(st);
    }
}

static float flip_angle(void) { return S.fth; }

/* Damped spring toward PI: bounce sets the overshoot, flip_ms the settle time. */
static bool flip_step(float dt)
{
    float b = clampf(S.m->tune.flip_bounce, 0.001f, 0.6f);
    float lb = logf(b);
    float zeta = -lb / sqrtf(PI * PI + lb * lb);
    float T = S.m->tune.flip_ms / 1000.0f;
    float w = 4.6f / (zeta * (T > 0.1f ? T : 0.1f));
    int n = (int)(dt / 0.002f) + 1;
    float h = dt / n;
    for (int i = 0; i < n; i++) {
        float acc = w * w * (PI - S.fth) - 2 * zeta * w * S.fvel;
        S.fvel += acc * h;
        S.fth += S.fvel * h;
    }
    return S.flip_t > 0.15f && fabsf(PI - S.fth) < 0.004f && fabsf(S.fvel) < 0.08f;
}

static void render_flip(strip_t *st)
{
    static uint16_t tmp[SCR_W * STRIP_MAX];
    float th = flip_angle();
    float c = cosf(th), s = fabsf(sinf(th));
    bool second = c < 0;
    int f = second ? S.face_to : S.face;
    const float T = 20;                         /* bar thickness in px */
    float fw = SCR_W * fabsf(c), sw = T * s;
    float L = SCR_W / 2.0f - (fw + sw) / 2.0f;
    float faceL = second ? L + sw : L, sideL = second ? L : L + fw;
    strip_t ts = { tmp, st->y0, st->h };
    render_face(f, &ts);
    /* brightness: dims edge-on, plus a specular catch when the face is ~20 degrees off */
    float off = second ? fabsf(PI - th) : th;
    float spec = 0.38f * expf(-powf((off - 0.42f) / 0.2f, 2));
    int m = (int)(256 * (0.5f + 0.5f * fabsf(c) + spec));
    int inv = fw > 0.5f ? (int)(65536.0f * SCR_W / fw) : 0;
    int fL = (int)lrintf(faceL * 256), fR = (int)lrintf((faceL + fw) * 256);
    int sL = (int)lrintf(sideL * 256), sR = (int)lrintf((sideL + sw) * 256);
    int side_shade = (int)(30 + 70 * s * (second ? 0.55f : 1.0f));
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        uint16_t *o = &st->px[yy * SCR_W];
        const uint16_t *src = &tmp[yy * SCR_W];
        bool siderow = y > BAR_Y0 + 5 && y < BAR_Y1 - 5;
        int ss = side_shade + (int)(hash32(y * 31) & 7) - 3;
        if (ss < 1) ss = 1;
        if (ss > 127) ss = 127;
        uint16_t sidec = S.lut_front_ok ? S.lut_front[ss] : rgb565(200, 160, 80);
        for (int x = 0; x < SCR_W; x++) {
            int xp = x * 256 + 128;
            if (xp >= fL && xp < fR && inv) {
                int sx = (int)(((int64_t)(xp - fL) * inv) >> 24);
                if (sx >= SCR_W) sx = SCR_W - 1;
                o[x] = scale565(src[sx], m);
            } else if (siderow && xp >= sL && xp < sR) {
                o[x] = sidec;
            } else {
                o[x] = S.bg_row[y];
            }
        }
    }
}

/* ---------------------------------------------------------------- zoom */

static void render_zoom(strip_t *st)
{
    double N = S.ztotal;
    double C = ceil(sqrt(N));
    double z = exp(S.zlog);
    double cw = SCR_W * z, ch = SCR_H * z;
    double ox = SCR_W / 2.0 - cw / 2, oy = SCR_H / 2.0 - ch / 2;     /* your cell's origin */
    double X0 = ox - floor(C / 2) * cw, X1 = X0 + C * cw;
    double Y0 = oy - floor(C / 2) * ch, Y1 = Y0 + C * ch;
    const double g = 0.09;
    int idet = (int)(256 * clampf((float)(cw - 3) / 3.0f, 0, 1));
    bool tex = cw >= 22;
    static int16_t cov_x[SCR_W], cell_x[SCR_W], src_x[SCR_W], edge_x[SCR_W];
    static uint8_t comp_x[SCR_W], line_x[SCR_W], grad_x[SCR_W];
    /* decade grid: blocks of 10^L shares, visible while a block is 8..300 px */
    static float la[12];
    static double bpx[12], bpy[12];
    bool first = st->y0 == 0;
    float lend = S.zphase == 0 ? 1 - clampf((S.zt * 1000.0f / S.m->tune.zoom_ms - 0.8f) / 0.15f, 0, 1) : (S.zphase == 1 ? 0 : 1);
    if (first) {
        double p10 = 1;
        for (int L = 1; L < 12; L++) {
            p10 *= 10;
            bpx[L] = cw * p10; bpy[L] = ch * p10;
            la[L] = lend * clampf((float)(bpx[L] - 8) / 16, 0, 1) * clampf((float)(320 - bpx[L]) / 160, 0, 1);
        }
    }
    for (int x = 0; first && x < SCR_W; x++) {
        double u = (x + 0.5 - ox) / cw;
        double fl = floor(u), f = u - fl;
        double dpx = fmin(f - g, 1 - g - f) * cw;
        cov_x[x] = (int16_t)(256 * fmin(1, fmax(0, dpx + 0.5)));
        cell_x[x] = (int16_t)fmax(-30000, fmin(30000, fl));
        src_x[x] = (int16_t)((f - g) / (1 - 2 * g) * SCR_W);
        edge_x[x] = (f - g) * cw < 1.5 ? 1 : ((1 - g - f) * cw < 1.5 ? -1 : 0);
        comp_x[x] = (uint8_t)(255 * fmin(1, fmax(0, fmin(x + 1 - X0, X1 - x))));
        grad_x[x] = (uint8_t)(255 * fmin(1, fmax(0, (x + 0.5 - X0) / (X1 - X0))));
        float lx = 0;
        for (int L = 1; L < 12; L++) {
            if (la[L] <= 0) continue;
            double bp = bpx[L], w = (x + 0.5 - ox) / bp;
            double d = fabs(w - floor(w + 0.5)) * bp;
            if (d < 0.8) lx = fmaxf(lx, la[L] * (float)(0.8 - d) / 0.8f);
        }
        line_x[x] = (uint8_t)(lx * 255);
    }
    const int flat = 52;
    float pulse = 0.5f + 0.5f * sinf(S.clock * 5);
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        uint16_t *o = &st->px[yy * SCR_W];
        double v = (y + 0.5 - oy) / ch;
        double fl = floor(v), f = v - fl;
        double dpy = fmin(f - g, 1 - g - f) * ch;
        int cov_y = (int)(256 * fmin(1, fmax(0, dpy + 0.5)));
        int cell_y = (int)fmax(-30000, fmin(30000, fl));
        int src_y = (int)((f - g) / (1 - 2 * g) * SCR_H);
        int edge_y = (f - g) * ch < 1.5 ? 1 : ((1 - g - f) * ch < 1.5 ? -1 : 0);
        int comp_y = (int)(255 * fmin(1, fmax(0, fmin(y + 1 - Y0, Y1 - y))));
        int grad_y = (int)(255 * fmin(1, fmax(0, (y + 0.5 - Y0) / (Y1 - Y0))));
        float ly = 0;
        for (int L = 1; L < 12; L++) {
            if (la[L] <= 0) continue;
            double bp = bpy[L], w = (y + 0.5 - oy) / bp;
            double d = fabs(w - floor(w + 0.5)) * bp;
            if (d < 0.8) ly = fmaxf(ly, la[L] * (float)(0.8 - d) / 0.8f);
        }
        int line_y = (int)(ly * 255);
        uint16_t bg = S.bg_row[y];
        for (int x = 0; x < SCR_W; x++) {
            int comp = comp_x[x] * comp_y >> 8;
            if (!comp) { o[x] = bg; continue; }
            /* one giant bar when the shares blur together: lit from the upper left */
            int sh0 = flat + 26 - ((grad_x[x] * 10 + grad_y * 22) >> 8);
            uint16_t c = S.lut_front[sh0];
            if (idet > 0) {
                bool you = cell_x[x] == 0 && cell_y == 0;
                int cov = cov_x[x] * cov_y >> 8;
                uint16_t cc;
                if (tex && src_y >= 0 && src_y < SCR_H && src_x[x] >= 0 && src_x[x] < SCR_W) {
                    int id = S.front[src_y * SCR_W + src_x[x]];
                    cc = id ? S.lut_front[id] : bg;
                    if (!you) {
                        uint32_t h = hash32((uint32_t)cell_x[x] * 73856093u ^ (uint32_t)cell_y * 19349663u);
                        cc = scale565(cc, 120 + (h & 31));
                    }
                } else {
                    int sh;
                    if (you) sh = 100 + (int)(20 * pulse);
                    else {
                        uint32_t h = hash32((uint32_t)cell_x[x] * 73856093u ^ (uint32_t)cell_y * 19349663u);
                        sh = 40 + (int)(h & 15) + (edge_x[x] + edge_y) * 14;
                    }
                    if (sh < 1) sh = 1;
                    if (sh > 127) sh = 127;
                    cc = S.lut_front[sh];
                }
                cc = blend565(bg, cc, cov);
                c = idet < 256 ? blend565(c, cc, idet) : cc;
            }
            int l = line_x[x] > line_y ? line_x[x] : line_y;
            if (l) c = blend565(c, S.lut_front[idet ? 20 : 92], l * (idet ? 120 : 200) >> 8);
            o[x] = comp < 255 ? blend565(bg, c, comp) : c;
        }
    }
    /* you-are-here marker once your bar is small */
    if (cw < 14) {
        float a = clampf((14 - (float)cw) / 6, 0, 1);
        ring_arc(st, SCR_W / 2.0f, SCR_H / 2.0f, 8 + 2 * pulse, 1.6f, 0, 2 * PI, rgb565(255, 240, 200), (int)(a * 240));
        fill_rrect(st, SCR_W / 2.0f - 1.5f, SCR_H / 2.0f - 1.5f, SCR_W / 2.0f + 1.5f, SCR_H / 2.0f + 1.5f, 1.5f, rgb565(255, 244, 214), (int)(a * 256));
        text_draw(st, &font_caps11, "YOU", SCR_W / 2.0f + 14, SCR_H / 2 - 10, 1.4f, rgb565(255, 240, 200), (int)(a * 256));
    }
    /* counter */
    double vis = fmin(N, (1.0 / z) * (1.0 / z));
    char n[32];
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        if (y >= 46) break;
        int k = y < 30 ? 110 : 110 + (y - 30) * 9;
        uint16_t *o = &st->px[yy * SCR_W];
        for (int x = 0; x < SCR_W; x++) o[x] = scale565(o[x], k);
    }
    if (vis < 1.5) text_draw_centered(st, &font_caps13, "YOUR SHARE", SCR_W / 2, 30, 2.0f, C_GOLD_HI, 256);
    else {
        double p10 = pow(10, floor(log10(vis)) - 2);
        double r = vis >= 1000 ? floor(vis / p10) * p10 : floor(vis);
        if (vis >= N * 0.999) r = N;
        fmt_commas(r, n, sizeof n);
        text_draw_centered(st, &font_mono14, n, SCR_W / 2, 24, 0, C_GOLD_HI, 256);
        text_draw_centered(st, &font_caps11, "SHARES", SCR_W / 2, 38, 2.0f, C_GOLD_DIM, 256);
    }
    float endt = S.zphase == 0 ? (S.zt * 1000.0f / S.m->tune.zoom_ms - 0.85f) / 0.15f : (S.zphase == 1 ? 1.0f : 0);
    if (endt > 0) {
        int a = (int)(256 * clampf(endt, 0, 1));
        fmt_pct_tiny(100.0 / N, n, sizeof n);
        text_draw_centered(st, &font_caps11, "YOU OWN", SCR_W / 2, 288, 2, C_GOLD_DIM, a);
        text_draw_centered(st, &font_mono14, n, SCR_W / 2, 305, 0, C_GOLD_HI, a);
    }
}

/* ---------------------------------------------------------------- overlays */

static void render_overlays(strip_t *st)
{
    /* long-press ring */
    if (S.pressed && !S.long_fired && S.press_t > 0.12f) {
        float p = clampf((S.press_t - 0.12f) / (S.m->tune.longpress_ms / 1000.0f - 0.12f), 0, 1);
        int a = (int)(256 * smoothstep01((S.press_t - 0.12f) / 0.15f));
        ring_arc(st, S.px, S.py, 24, 2.0f, 0, 2 * PI, rgb565(0, 0, 0), a / 3);
        ring_arc(st, S.px, S.py, 24, 2.6f, 0, 2 * PI * ease_in_out_cubic(p), C_GOLD_HI, a);
    }
    if (S.ring_fade > 0) {
        float k = 1 - S.ring_fade;
        ring_arc(st, S.px, S.py, 24 + 30 * k, 2.6f * (1 - k) + 0.5f, 0, 2 * PI, C_GOLD_HI, (int)(256 * S.ring_fade));
    }
    if (S.m->settings_mode) {
        for (int yy = 0; yy < st->h; yy++) {
            int y = st->y0 + yy;
            if (y < 288) continue;
            uint16_t *o = &st->px[yy * SCR_W];
            for (int x = 0; x < SCR_W; x++) o[x] = scale565(o[x], 60);
        }
        text_draw_centered(st, &font_caps11, "SETTINGS", SCR_W / 2, 302, 2, C_GOLD_HI, 256);
        text_draw_centered(st, &font_mono11, S.m->settings_hint, SCR_W / 2, 315, 0, rgb565(200, 190, 170), 256);
    }
}

/* ================================================================== public */

bool scene_init(model_t *m)
{
    memset(&S, 0, sizeof S);
    S.m = m;
    S.fade = 0;
    build_bg();
    if (m->mint.minted) {
        S.st = ST_FACES;
        ensure_base();
        if (!ensure_front(&m->mint.p, m->mint.serial)) return false;
        build_back(true);
        glint_start(1.2f, 26, 22, 6, 8, 0);
    } else {
        S.st = ST_SETUP;
        ensure_base();
        if (!S.base) return false;
    }
    S.dirty = true;
    return true;
}

void scene_invalidate(void) { S.dirty = true; }

void scene_rehearse(void)
{
    if (!S.m->mint.minted) return;
    ensure_base();
    if (!S.base) return;
    S.rehearsal = true;
    S.heat = 0;
    S.st = ST_READY;
    S.t = 0;
    S.flipping = false;
    S.face = F_FRONT;
    S.dirty = true;
}

static void begin_mint(void)
{
    S.st = ST_MINT;
    S.t = 0;
    S.mt = 0;
    S.stamps_done = 0;
    S.dy = S.dyv = 0;
    memset(S.band_flash, 0, sizeof S.band_flash);
    if (!S.rehearsal) {
        model_t *m = S.m;
        m->mint.minted = true;
        m->mint.p = m->setup.pending;
        m->mint.mint_ts = m->now;
        m->patina_day = days_held(m);
        S.events |= EV_MINTED;
    }
}

static void ledger_prepare(void)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    int sh = p->shares > 0 ? p->shares : 1;
    double from = S.have_shown ? S.shown_price : p->price;
    double to = q->valid ? q->price : from;
    S.led_from[0] = from; S.led_to[0] = to;
    S.led_from[1] = 0; S.led_to[1] = q->chg_pct;
    S.led_from[2] = from * sh; S.led_to[2] = to * sh;
    S.led_from[3] = (from / p->price - 1) * 100; S.led_to[3] = (to / p->price - 1) * 100;
    S.led_from[4] = from / IPO_PRICE; S.led_to[4] = to / IPO_PRICE;
    S.shown_price = to;
    S.have_shown = q->valid;
    S.ledger_ready = true;
    S.ledger_t = 0;
    S.ledger_anim = false;
}

static void land_on(int f)
{
    S.face = f;
    S.flipping = false;
    S.cooldown = S.m->tune.shake_cooldown_ms / 1000.0f;
    if (f == F_FRONT) glint_start(0.5f, 24, 18, 6, 8, 0);
    if (f == F_LINE) {
        int day = days_held(S.m);
        if (!S.back_built || S.line_for_day != day || strcmp(S.line, current_line(day))) build_back(false);
        bool seen = S.m->line_seen_day == day && day > 0;
        if (seen && S.burin_done < S.nchars) { for (int k = S.burin_done; k < S.nchars; k++) engrave_char(k); S.burin_done = S.nchars; }
        S.burin = S.burin_done < S.nchars;
        if (!S.burin) glint_start(0.5f, 20, 18, 6, 8, 0);
    }
    if (f == F_LEDGER) {
        if (!S.ledger_ready) ledger_prepare();
        S.ledger_ready = false;
        S.ledger_t = 0;
        S.ledger_anim = S.m->quote.valid;
    }
}

static void start_flip(void)
{
    if (S.face == F_LINE && S.burin) {           /* finish the engraving before turning away */
        for (int k = S.burin_done; k < S.nchars; k++) engrave_char(k);
        S.burin_done = S.nchars;
        S.burin = false;
        S.m->line_seen_day = S.line_for_day;
        S.events |= EV_LINE_SEEN;
    }
    S.face_to = (S.face + 1) % F_COUNT;
    if (S.face_to != F_FRONT && !S.back_built) build_back(false);
    S.flipping = true;
    S.flip_t = 0;
    S.fth = 0;
    S.fvel = 2.5f;          /* the shake gives it a small kick */
    if (S.face_to == F_LEDGER) { ledger_prepare(); build_chart(S.mpage); }
    S.glint_on = false;
}

static void update_qr(void)
{
    char want[64];
    snprintf(want, sizeof want, "WIFI:T:nopass;S:%s;;", S.m->setup.ap_ssid);
    if (S.qr_ok && !strncmp(S.qr_for, want, sizeof S.qr_for)) return;
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    S.qr_ok = qrcodegen_encodeText(want, tmp, S.qr, qrcodegen_Ecc_LOW, 1, 6, qrcodegen_Mask_AUTO, true);
    snprintf(S.qr_for, sizeof S.qr_for, "%s", want);
}

void scene_tick(float dt, const input_t *in)
{
    model_t *m = S.m;
    const tune_t *tu = &m->tune;
    if (dt > 0.1f) dt = 0.1f;
    S.clock += dt;
    S.t += dt;
    if (S.fade < 1) S.fade = fminf(1, S.fade + dt / 0.6f);

    bool act = in->shake || in->touch || in->btn_short;
    if (act) S.events |= EV_ACTIVITY;

    /* ---- touch: long press → zoom, tap → skip */
    bool tap = false;
    if (in->touch) {
        if (!S.pressed) { S.pressed = true; S.press_t = 0; S.px = in->tx; S.py = in->ty; S.long_fired = false; }
        S.press_t += dt;
        if (abs(in->tx - S.px) > 18 || abs(in->ty - S.py) > 18) S.press_t = 0, S.px = in->tx, S.py = in->ty;
    } else if (S.pressed) {
        S.pressed = false;
        if (!S.long_fired && S.press_t < 0.3f) tap = true;
    }
    if (S.ring_fade > 0) S.ring_fade = fmaxf(0, S.ring_fade - dt / 0.35f);

    if (S.glint_on) { S.g_t += dt; if (S.g_t >= S.g_dur) S.glint_on = false; }

    switch (S.st) {
    case ST_SETUP: {
        update_qr();
        for (int i = 0; i < 3; i++) {
            if (m->setup.chk[i] != S.chk_seen[i]) { S.chk_seen[i] = m->setup.chk[i]; S.chk_anim[i] = 0; }
            S.chk_anim[i] += dt;
        }
        if (!S.glint_on && fmodf(S.clock, 5.0f) < dt) glint_start(2.4f, 9, 30, 6, 8, 0);
        if (m->setup.stage == SETUP_READY) {
            if (ensure_front(&m->setup.pending, m->mint.serial)) { S.st = ST_READY; S.t = 0; S.heat = 0; S.rehearsal = false; }
        }
        set_front_params((metal_params_t){ .finish = 0, .gain = 1 });
        break;
    }
    case ST_READY: {
        if (!S.rehearsal && m->setup.stage != SETUP_READY) { S.st = ST_SETUP; break; }
        float e = in->shake_energy - tu->heat_floor;
        if (e > 0) S.heat += e * tu->heat_gain * dt;
        else S.heat -= tu->heat_decay * dt * (0.4f + S.heat);
        if (e > 0 && in->shake_energy < tu->heat_floor * 2) S.heat -= tu->heat_decay * dt * 0.3f;
        S.heat = clampf(S.heat, 0, 1);
        if (!S.glint_on && S.heat < 0.05f && fmodf(S.clock, 5.0f) < dt) glint_start(2.4f, 9, 30, 6, 8, 0);
        float T = S.heat * 0.74f + (S.heat > 0.1f ? 0.015f * sinf(S.clock * 23) * sinf(S.clock * 7) : 0);
        set_front_params((metal_params_t){ .finish = 0.15f * S.heat, .heat = T, .gain = 1 });
        if (S.heat >= 1) begin_mint();
        break;
    }
    case ST_MINT: {
        float sp = tu->mint_speed > 0.2f ? tu->mint_speed : 1;
        S.mt += dt * sp;
        float t = S.mt;
        /* heat: flash, hold, cool exponentially */
        float T;
        if (t < 0.12f) T = lerpf(0.74f, 1.0f, t / 0.12f);
        else if (t < 0.28f) T = 1.0f;
        else {
            float u = clampf((t - 0.28f) / 2.3f, 0, 1);
            T = (expf(-2.4f * u) - expf(-2.4f)) / (1 - expf(-2.4f));
        }
        float fin = smoothstep01((t - 0.3f) / 1.6f);
        float gain = 1.0f + 0.25f * clampf(1 - fabsf(t - 0.18f) / 0.18f, 0, 1);
        set_front_params((metal_params_t){ .finish = fin, .heat = T, .gain = gain, .patina = 0 });
        /* stamps */
        const float t0 = 1.85f, gap = 0.17f;
        while (S.stamps_done < S.lay.nbands && t >= t0 + S.stamps_done * gap) {
            float w = S.lay.bands[S.stamps_done].weight;
            S.dyv += 70 * w;
            S.band_flash[S.stamps_done] = 1;
            S.led_pulse = fmaxf(S.led_pulse, 0.5f + 0.4f * w);
            S.stamps_done++;
        }
        for (int b = 0; b < 6; b++) S.band_flash[b] = fmaxf(0, S.band_flash[b] - dt / 0.28f);
        /* damped spring for the stamp jolt */
        float k = 1400, c = 34;
        float acc = -k * S.dy - c * S.dyv;
        S.dyv += acc * dt;
        S.dy += S.dyv * dt;
        if (t >= 2.95f && t - dt * sp < 2.95f) glint_start(0.7f, 42, 20, 6, 8, 0);
        if (t >= 3.8f) {
            S.st = ST_FACES;
            S.t = 0;
            S.face = F_FRONT;
            S.dy = S.dyv = 0;
            S.rehearsal = false;
            S.cooldown = 0.6f;
            S.need_calm = true;     /* the hand that minted is probably still shaking */
            S.calm_t = 0;
            if (!S.back_built) build_back(false);
        }
        break;
    }
    case ST_FACES: {
        int day = days_held(m);
        if (!S.sweeping && day > 0 && m->patina_day > 0 && day > m->patina_day && S.face == F_FRONT && !S.flipping) {
            S.sweeping = true;
            S.sweep_t = 0;
            S.sweep_from_day = m->patina_day;
            glint_start(1.6f, 16, 34, 0, 10, 0.3f);
        }
        if (m->patina_day == 0 && day > 0) { m->patina_day = day; S.events |= EV_PATINA_SEEN; }
        if (S.sweeping) {
            S.sweep_t += dt;
            if (S.sweep_t >= 1.6f) { S.sweeping = false; m->patina_day = day; S.events |= EV_PATINA_SEEN; }
        }
        float cool = S.sweeping ? 0.25f * sinf(PI * clampf(S.sweep_t / 1.6f, 0, 1)) : 0;
        float pat = patina_for_days(display_days());
        set_front_params((metal_params_t){ .finish = 1, .patina = pat, .gain = 1, .cool = cool });
        set_back_params((metal_params_t){ .finish = 1, .patina = pat, .gain = 0.97f });

        if (S.cooldown > 0) S.cooldown -= dt;
        if (S.need_calm) {
            S.calm_t = in->shake_energy < tu->heat_floor ? S.calm_t + dt : 0;
            if (S.calm_t > 0.35f) S.need_calm = false;
        }
        if (S.flipping) {
            S.flip_t += dt;
            if (flip_step(dt) || S.flip_t * 1000 > tu->flip_ms * 2.5f) land_on(S.face_to);
        } else if ((in->btn_short || (in->shake && !S.need_calm)) && S.cooldown <= 0) {
            start_flip();
        }
        /* daily line engraving */
        if (S.face == F_LINE && !S.flipping && S.burin) {
            if (tap) S.burin_pos = (float)S.nchars;
            else {
                S.burin_pos += dt * 1000.0f / (tu->burin_ms > 5 ? tu->burin_ms : 5);
            }
            while (S.burin_done < (int)S.burin_pos && S.burin_done < S.nchars) engrave_char(S.burin_done++);
            if (S.burin_done >= S.nchars) {
                S.burin = false;
                m->line_seen_day = S.line_for_day;
                S.events |= EV_LINE_SEEN;
                glint_start(0.6f, 22, 18, 6, 8, 0);
            }
        }
        /* new data while showing it */
        if (S.face == F_LINE && !S.flipping && !S.burin) {
            int d = days_held(m);
            if (d != S.line_for_day || strcmp(S.line, current_line(d))) { build_back(false); S.burin = true; }
        }
        if (S.face == F_LEDGER && !S.flipping) {
            if (tap && !S.mfading) { S.mfading = true; S.mfade_t = 0; S.mpage_to = (S.mpage + 1) % 3; }
            if (S.mfading) {
                float before = S.mfade_t;
                S.mfade_t += dt;
                if (before < 0.12f && S.mfade_t >= 0.12f) { S.mpage = S.mpage_to; build_chart(S.mpage); }
                if (S.mfade_t >= 0.30f) S.mfading = false;
            }
            if (!S.mfading && (CHT.page != S.mpage || CHT.rev != chart_rev_now())) { build_chart(S.mpage); S.dirty = true; }
            int minute = (int)(m->now / 60);
            if (minute != S.last_minute) { S.last_minute = minute; S.dirty = true; }
            if (S.ledger_anim) { S.ledger_t += dt; if (S.ledger_t > 1.2f) S.ledger_anim = false; }
            if (!S.ledger_anim && m->quote.valid && (!S.have_shown || fabs(m->quote.price - S.shown_price) > 0.004)) { ledger_prepare(); land_on(F_LEDGER); }
        }
        /* long press → zoom */
        if (S.pressed && !S.long_fired && !S.flipping && S.press_t * 1000 >= tu->longpress_ms) {
            S.long_fired = true;
            S.ring_fade = 1;
            S.st = ST_ZOOM;
            S.zt = 0;
            S.zphase = 0;
            S.zlog = 0;
            S.ztotal = (m->quote.valid && m->quote.total_shares > 1e6) ? m->quote.total_shares : DEFAULT_TOTAL_SHARES;
            S.ztotal /= (m->mint.p.shares > 0 ? m->mint.p.shares : 1);
            if (S.ztotal < 1) S.ztotal = 1;
        }
        break;
    }
    case ST_ZOOM: {
        double C = ceil(sqrt(S.ztotal));
        float zend = (float)log(96.0 / (SCR_W * C));
        S.zt += dt;
        set_front_params((metal_params_t){ .finish = 1, .patina = patina_for_days(display_days()), .gain = 1 });
        if (S.zphase == 0) {
            float u = clampf(S.zt * 1000.0f / tu->zoom_ms, 0, 1);
            float e = u * u * u * (u * (u * 6 - 15) + 10);
            S.zlog = zend * e;
            if (u >= 1) { S.zphase = 1; S.zt = 0; }
            if (tap || in->shake || in->btn_short) { S.zphase = 2; S.zt = 0; S.zlog_from = S.zlog; }
        } else if (S.zphase == 1) {
            if (tap || in->shake || in->btn_short || S.zt > 15) { S.zphase = 2; S.zt = 0; S.zlog_from = S.zlog; }
        } else {
            float u = clampf(S.zt / 1.1f, 0, 1);
            S.zlog = S.zlog_from * (1 - ease_in_out_cubic(u));
            if (u >= 1) { S.st = ST_FACES; S.face = F_FRONT; S.flipping = false; S.cooldown = 0.4f; glint_start(0.5f, 24, 18, 6, 8, 0); }
        }
        break;
    }
    }
    if (S.st == ST_SETUP || S.st == ST_READY || S.st == ST_MINT) {
        if (S.st != ST_MINT) S.led_pulse = 0;
    }
    if (S.led_pulse > 0) S.led_pulse = fmaxf(0, S.led_pulse - dt / 0.18f);
    glint_update();
}

void scene_render(strip_t *st)
{
    switch (S.st) {
    case ST_SETUP:
    case ST_READY:
        if (S.base) render_metal(st, S.base, S.lut_front, false);
        if (S.st == ST_SETUP) render_setup_overlay(st);
        else render_ready_overlay(st);
        break;
    case ST_MINT:
        render_metal(st, S.front, S.lut_front, true);
        break;
    case ST_FACES:
        if (S.flipping) render_flip(st);
        else render_face(S.face, st);
        break;
    case ST_ZOOM:
        render_zoom(st);
        break;
    }
    render_overlays(st);
    if (S.fade < 1) {
        int m = (int)(256 * ease_in_out_cubic(S.fade));
        for (int i = 0; i < SCR_W * st->h; i++) st->px[i] = scale565(st->px[i], m);
    }
}

void scene_led(uint8_t *r, uint8_t *g, uint8_t *b)
{
    float R = 0, G = 0, B = 0;
    float k = S.m->tune.led / 100.0f;
    if (S.st == ST_READY) {
        float br = 0.05f + 0.03f * sinf(S.clock * 2.2f);
        float er, eg, eb;
        blackbody(0.35f + S.heat * 0.4f, &er, &eg, &eb);
        float lv = br + S.heat * S.heat * 0.9f;
        R = er * lv; G = eg * lv; B = eb * lv;
    } else if (S.st == ST_MINT) {
        float er, eg, eb;
        float T = S.mp_front.heat;
        blackbody(T, &er, &eg, &eb);
        float lv = T * T;
        R = er * lv; G = eg * lv; B = eb * lv;
        R += S.led_pulse * 1.0f; G += S.led_pulse * 0.7f; B += S.led_pulse * 0.3f;
    } else if (S.st == ST_FACES && S.burin && !S.flipping && S.face == F_LINE) {
        float f = 0.08f + 0.06f * sinf(S.clock * 53) * sinf(S.clock * 31);
        R = f; G = f * 0.75f; B = f * 0.45f;
    }
    if (S.st == ST_FACES && !(S.burin && S.face == F_LINE) && S.m->quote.valid && S.m->now > 0 &&
        market_state(S.m->now, NULL, NULL) == MKT_OPEN) {
        float br = 0.035f + 0.02f * sinf(S.clock * 1.6f);
        if (S.m->quote.price >= S.m->mint.p.price) { R += br; G += br * 0.68f; B += br * 0.22f; }
        else { R += br * 0.4f; G += br * 0.5f; B += br * 0.75f; }
    }
    if (S.m->settings_mode) {
        float br = 0.04f + 0.03f * sinf(S.clock * 2);
        G += br * 0.7f; B += br;
    }
    *r = (uint8_t)(clampf(R * k, 0, 1) * 255);
    *g = (uint8_t)(clampf(G * k, 0, 1) * 255);
    *b = (uint8_t)(clampf(B * k, 0, 1) * 255);
}

uint32_t scene_take_events(void)
{
    uint32_t e = S.events;
    S.events = 0;
    return e;
}

bool scene_animating(void)
{
    if (S.dirty) { S.dirty = false; return true; }
    if (S.fade < 1 || S.st != ST_FACES || S.flipping || S.glint_on || S.burin || S.ledger_anim || S.mfading ||
        S.sweeping || S.pressed || S.ring_fade > 0 || S.m->settings_mode)
        return true;
    return false;
}
