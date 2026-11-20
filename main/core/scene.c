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
enum { F_FRONT, F_BACK, F_COUNT };

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

    /* living zone on the front: price + gold weight, re-cut at each close */
    uint8_t *zink_a, *zink_b;   /* ZONE_H x SCR_W text masks: shown / incoming */
    uint8_t *zbuf;              /* heights of rows ZONE_Y0-1 .. ZONE_Y1+1 */
    float zm_a, zm_b1, zm_b2;   /* how deep each mask is cut (0 = melted flat) */
    float zflash[2];
    int as_phase;               /* 0 idle, 1 holding after mint, 2 assaying */
    float as_t, as_wait;
    assay_t as_new;
    float furnace;              /* 0..1 market-open glow */
    float zglow;                /* current glow strength 0..1 */
    int zglow_r, zglow_g, zglow_b;
    float zscroll;

    /* back, page 0: the daily line */
    char line[160];
    int line_for_day;
    qline_t ql[8];
    int nql, nchars;
    bool burin;
    float burin_pos;            /* characters revealed (float) */
    char header[40];

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

    /* light */
    float tx, ty, tx_applied, ty_applied, drift, still_t;
    float ref_tx, ref_ty;

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

    bool partial;               /* only the zone needs redrawing */
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

static int32_t ymd_to_days_s(int ymd)
{
    int y = ymd / 10000, m = ymd / 100 % 100, d = ymd % 100;
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

/* ================================================================== surfaces */


static void build_band_rows(void)
{
    memset(S.band_of_row, -1, sizeof S.band_of_row);
    for (int b = 0; b < S.lay.nbands; b++)
        for (int y = S.lay.bands[b].y0; y <= S.lay.bands[b].y1 && y < SCR_H; y++) S.band_of_row[y] = (int8_t)b;
}

#define SURF_VERSION 6u
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

#define QL_MID 116

static void layout_line(const char *txt)
{
    const font_t *f = &font_ital17;
    const int maxw = 128;
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
    const int lh = 22;
    int top = QL_MID - (S.nql * lh) / 2 + 15;
    for (int i = 0; i < S.nql; i++) {
        S.ql[i].x = SCR_W / 2.0f - text_width(f, S.ql[i].s, 0.2f) / 2.0f;
        S.ql[i].y = top + i * lh;
        S.ql[i].first = S.nchars;
        S.ql[i].n = (int)strlen(S.ql[i].s);
        S.nchars += S.ql[i].n;
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
    *x = SCR_W / 2.0f; *y = QL_MID;
}

static void prepare_line(void)
{
    int day = days_held(S.m);
    char r[24];
    if (day > 0) snprintf(S.header, sizeof S.header, "DAY %s", roman(day, r, sizeof r));
    else snprintf(S.header, sizeof S.header, "HELD");
    snprintf(S.line, sizeof S.line, "%s", current_line(day));
    S.line_for_day = day;
    layout_line(S.line);
}

static bool line_is_stale(void)
{
    int d = days_held(S.m);
    return d != S.line_for_day || strcmp(S.line, current_line(d)) != 0;
}

/* ---------------------------------------------------------------- living zone */

static uint8_t NOISE[64][64];

static void build_noise(void)
{
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            /* two octaves of tileable value noise */
            float v = 0, amp = 0.65f;
            for (int o = 0; o < 2; o++) {
                int per = o ? 8 : 4;
                float fx = x * per / 64.0f, fy = y * per / 64.0f;
                int ix = (int)fx, iy = (int)fy;
                float ux = fx - ix, uy = fy - iy;
                ux = ux * ux * (3 - 2 * ux); uy = uy * uy * (3 - 2 * uy);
                #define NV(a, b) ((hash32((uint32_t)(((a) % per) * 73 + ((b) % per) * 151 + o * 977)) & 255) / 255.0f)
                float n = lerpf(lerpf(NV(ix, iy), NV(ix + 1, iy), ux), lerpf(NV(ix, iy + 1), NV(ix + 1, iy + 1), ux), uy);
                #undef NV
                v += n * amp;
                amp = 0.35f;
            }
            NOISE[y][x] = (uint8_t)(v * 255);
        }
}

static void zone_texts(double price, double grams, char *l1, int c1, char *l2, int c2)
{
    snprintf(l1, c1, "$%.2f", price);
    if (grams > 0) snprintf(l2, c2, grams < 10 ? "%.3f G FINE GOLD" : "%.2f G FINE GOLD", grams);
    else l2[0] = 0;
}

static double grams_for(double price)
{
    double gold = S.m->quote.gold;
    int sh = S.m->mint.p.shares > 0 ? S.m->mint.p.shares : 1;
    return gold > 0 ? price * sh / (gold / TROY_OZ_G) : 0;
}

static void zbuild(void)
{
    if (!S.zbuf || !S.front) return;
    for (int r = ZONE_Y0 - 1; r <= ZONE_Y1 + 1; r++) {
        const uint8_t *f = S.front + r * SCR_W;
        uint8_t *o = S.zbuf + (r - (ZONE_Y0 - 1)) * SCR_W;
        int zr = r - ZONE_Y0;
        if (zr < 0 || zr >= ZONE_H) { memcpy(o, f, SCR_W); continue; }
        int mb = (int)(256 * (r <= ZONE_PRICE_Y1 ? S.zm_b1 : S.zm_b2));
        int ma = (int)(256 * S.zm_a);
        const uint8_t *ia = S.zink_a + zr * SCR_W, *ib = S.zink_b + zr * SCR_W;
        for (int x = 0; x < SCR_W; x++) {
            if (!f[x]) { o[x] = 0; continue; }
            int a = ia[x] * ma >> 8, b = ib[x] * mb >> 8;
            int ink = a > b ? a : b;
            int h = (f[x] & 127) - GROOVE_DEPTH * ink / 255;
            if (h < 1) h = 1;
            o[x] = (uint8_t)(h | ((f[x] & 128) || ink > 107 ? 128 : 0));
        }
    }
}

static bool zone_alloc(void)
{
    if (!S.zink_a) S.zink_a = calloc(ZONE_H, SCR_W);
    if (!S.zink_b) S.zink_b = calloc(ZONE_H, SCR_W);
    if (!S.zbuf) S.zbuf = calloc(ZONE_H + 2, SCR_W);
    return S.zink_a && S.zink_b && S.zbuf;
}

/* Show a weighing immediately (no ceremony). */
static void zone_show(double price, double grams)
{
    if (!zone_alloc()) return;
    char l1[16], l2[28];
    zone_texts(price, grams, l1, sizeof l1, l2, sizeof l2);
    ingot_ink_zone(S.zink_a, l1, l2);
    memset(S.zink_b, 0, ZONE_H * SCR_W);
    S.zm_a = 1;
    S.zm_b1 = S.zm_b2 = 0;
    zbuild();
}

/* The latest official close we know of, as an ET day index. */
static bool latest_close(int32_t *day)
{
    const quote_t *q = &S.m->quote;
    if (!q->valid || strlen(q->when) < 16) return false;
    if (S.m->now > 0 && q->fetched_ts > 0 && S.m->now - q->fetched_ts > 1800) return false;
    int Y = atoi(q->when), Mo = atoi(q->when + 5), D = atoi(q->when + 8);
    int hh = atoi(q->when + 11), mi = atoi(q->when + 14);
    if (hh * 60 + mi < 9 * 60 + 30) return false;
    int32_t d = ymd_to_days_s(Y * 10000 + Mo * 100 + D);
    /* give the official close a few minutes to settle before weighing it */
    if (S.m->now > 0) {
        int64_t et = S.m->now + us_et_offset(S.m->now);
        if (et / 86400 == d && (et % 86400) < 16 * 3600 + 5 * 60) return false;
    }
    /* only closes after the purchase count */
    int64_t close_utc = (int64_t)d * 86400 + (hh * 60 + mi) * 60;
    close_utc -= us_et_offset(close_utc);
    if (close_utc <= S.m->mint.p.buy_ts) return false;
    *day = d;
    return true;
}

static void start_assay(int32_t day)
{
    double price = S.m->quote.price;
    double grams = grams_for(price);
    S.as_new = (assay_t){ .valid = true, .price = price, .grams = grams, .close_day = day };
    char l1[16], l2[28];
    zone_texts(price, grams, l1, sizeof l1, l2, sizeof l2);
    ingot_ink_zone(S.zink_b, l1, l2);
    S.zm_b1 = S.zm_b2 = 0;
    S.as_phase = 2;
    S.as_t = 0;
    S.as_wait = 0;
}

static void zone_tick(float dt)
{
    model_t *m = S.m;
    if (!S.zbuf) return;
    mkt_t ms = m->now > 0 ? market_state(m->now, NULL, NULL) : MKT_CLOSED;
    if (S.as_phase == 1) { S.as_t += dt; if (S.as_t >= 3.0f) { S.as_phase = 0; S.as_t = 0; } }

    /* a close we have not weighed yet → the ceremony (waits briefly for a gold price) */
    if (S.as_phase == 0 && ms != MKT_OPEN && !S.flipping && S.st == ST_FACES) {
        int32_t d;
        if (latest_close(&d) && (!m->assay.valid || d > m->assay.close_day)) {
            if (m->quote.gold > 0 || S.as_wait > 90) start_assay(d);
            else S.as_wait += dt;
        }
    }
    float target = (S.as_phase == 0 && ms == MKT_OPEN && m->quote.valid) ? 1.0f : 0.0f;
    S.furnace += (target - S.furnace) * clampf(dt / 0.9f, 0, 1);
    if (S.furnace < 0.003f && target == 0) S.furnace = 0;

    float melt = 0;
    if (S.as_phase == 2) {
        float t = (S.as_t += dt);
        S.zm_a = 1 - smoothstep01(t / 0.9f);
        melt = sinf(PI * clampf(t / 1.05f, 0, 1));
        if (t >= 1.05f && S.zm_b1 < 1) { S.zm_b1 = 1; S.zflash[0] = 1; S.dyv += 70 * 1.4f; S.led_pulse = 1.0f; }
        if (t >= 1.25f && S.zm_b2 < 1) { S.zm_b2 = 1; S.zflash[1] = 1; S.dyv += 70 * 0.8f; S.led_pulse = fmaxf(S.led_pulse, 0.7f); }
        if (t >= 1.5f && t - dt < 1.5f) glint_start(0.7f, 36, 20, 6, 8, 0);
        if (t >= 2.4f) {
            memcpy(S.zink_a, S.zink_b, ZONE_H * SCR_W);
            S.zm_a = 1;
            S.zm_b1 = S.zm_b2 = 0;
            m->assay = S.as_new;
            S.events |= EV_ASSAY;
            S.as_phase = 0;
        }
        zbuild();
    }
    for (int i = 0; i < 2; i++) S.zflash[i] = fmaxf(0, S.zflash[i] - dt / 0.3f);
    /* stamp jolt (the mint has its own) */
    if (S.st == ST_FACES && (S.dy != 0 || S.dyv != 0)) {
        float acc = -1400 * S.dy - 34 * S.dyv;
        S.dyv += acc * dt;
        S.dy += S.dyv * dt;
        if (fabsf(S.dy) < 0.05f && fabsf(S.dyv) < 0.5f) S.dy = S.dyv = 0;
    }
    /* glow colour: open → hot when above cost, a dull red when below; melting → bright */
    float er, eg, eb, k;
    if (melt > 0) { blackbody(0.7f, &er, &eg, &eb); k = melt; }
    else {
        bool up = m->quote.valid && m->quote.price >= m->mint.p.price;
        blackbody(up ? 0.6f : 0.36f, &er, &eg, &eb);
        k = S.furnace * 0.62f;
    }
    S.zglow = k;
    S.zglow_r = (int)(er * 255); S.zglow_g = (int)(eg * 255); S.zglow_b = (int)(eb * 255);
    S.zscroll += dt * 9;
}

/* ================================================================== renderers */

/* Metal face: index image through palette, plus glint, stamp offset, mint reveal. */
/* ---------------------------------------------------------------- lighting
 * Faces are height maps (bit 7 = recessed, bits 0..6 = height). Light comes from a
 * direction that follows the device's tilt: diffuse from the height gradient, a
 * short cast shadow from the neighbour toward the light, broad studio reflections
 * that slide with the tilt, and a fine grating that beats against the guilloche. */

#define RN 820
#define ROFF0 230

typedef struct {
    int lx, ly;             /* light direction in the screen plane, *512 */
    int shx, shy;           /* cast-shadow sample offset (px, toward the light) */
    int roff;               /* reflection table shift */
    int silk_amp, silk_ca, silk_sa, silk_phase;   /* grating */
    int16_t R[2][RN];       /* studio reflections: front polished, back brushed */
    uint32_t key;
} light_t;
static light_t LT;

static void light_update(float tx, float ty, float silk, float shadow_px, float drift_phase)
{
    /* light toward world-up: opposite of gravity in the screen plane, biased to the upper left */
    float Lx = -tx * 0.9f - 0.30f, Ly = -ty * 0.9f - 0.45f;
    float l = sqrtf(Lx * Lx + Ly * Ly);
    if (l > 1) { Lx /= l; Ly /= l; }
    int lx = (int)lrintf(Lx * 512), ly = (int)lrintf(Ly * 512);
    float ln = sqrtf(Lx * Lx + Ly * Ly) + 1e-3f;
    int shx = (int)lrintf(Lx / ln * shadow_px), shy = (int)lrintf(Ly / ln * shadow_px);
    int roff = (int)lrintf(tx * 150 + ty * 110);
    float ang = 0.55f + tx * 0.35f;
    int ca = (int)lrintf(cosf(ang) * 256 / 1.9f), sa = (int)lrintf(sinf(ang) * 256 / 1.9f);   /* period ~1.9 px */
    int ph = (int)lrintf((tx * 3.1f + ty * 2.3f + drift_phase) * 256) & 0xFFFF;
    int amp = (int)lrintf(silk * 22);
    uint32_t key = hash32(lx * 7 + ly) ^ hash32(roff * 13 + shx * 3 + shy) ^ hash32(ca * 31 + sa) ^ hash32(ph * 5 + amp);
    if (key == LT.key) return;
    LT.key = key;
    LT.lx = lx; LT.ly = ly; LT.shx = shx; LT.shy = shy;
    LT.silk_amp = amp; LT.silk_ca = ca; LT.silk_sa = sa; LT.silk_phase = ph;
    if (roff != LT.roff || !LT.R[0][0]) {
        LT.roff = roff;
        for (int i = 0; i < RN; i++) {
            float u = (float)(i - ROFF0 - roff);
            float g1 = (u - 87) / 40, g2 = (u - 231) / 54, g3 = (u - 343) / 29;
            LT.R[0][i] = (int16_t)(40 + 52 * expf(-g1 * g1) + 30 * expf(-g2 * g2) + 12 * expf(-g3 * g3));
            float b1 = (u - 108) / 58, b2 = (u - 267) / 72;
            LT.R[1][i] = (int16_t)(50 + 32 * expf(-b1 * b1) + 20 * expf(-b2 * b2));
        }
    }
    S.dirty = true;
}

static inline int tri256(int v) { v &= 0xFF; return v < 128 ? v * 2 : (255 - v) * 2; }

static inline const uint8_t *rowp(const uint8_t *buf, int r)
{
    if (r < 0) r = 0;
    if (r >= SCR_H) r = SCR_H - 1;
    if (buf == S.front && S.zbuf && r >= ZONE_Y0 - 1 && r <= ZONE_Y1 + 1) return S.zbuf + (r - (ZONE_Y0 - 1)) * SCR_W;
    return buf + r * SCR_W;
}

static void render_metal_k(strip_t *st, const uint8_t *buf, const uint16_t *lut, bool minting, int kind)
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
    const int16_t *R = LT.R[kind];
    const int lx = LT.lx, ly = LT.ly, shx = LT.shx, shy = LT.shy;
    const bool silk = kind == 0 && LT.silk_amp > 0 && !minting;
    const int silk_r2 = 58 * 58;
    const bool zone = buf == S.front && S.zbuf && !minting;
    const int zg = (int)(S.zglow * 256);
    const int zs = (int)S.zscroll;
    const int zma = (int)(256 * S.zm_a);
    const int zpool = S.as_phase == 2 ? (int)(70 * sinf(PI * clampf(S.as_t / 1.05f, 0, 1))) : 0;
    const uint8_t *B = (minting && S.base) ? S.base : buf;
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy;
        int sy = y - dy;
        uint16_t *o = &st->px[yy * SCR_W];
        if (sy < 1 || sy >= SCR_H - 1) { for (int x = 0; x < SCR_W; x++) o[x] = S.bg_row[y]; continue; }
        int band = S.band_of_row[sy];
        int dyc = sy - rcy;
        bool rosrow = minting && dyc * dyc < (int)(S.lay.rmax * S.lay.rmax) + 64;
        bool silkrow = silk && dyc * dyc < silk_r2;
        int zr = sy - ZONE_Y0;
        bool zrow = zone && zr >= 0 && zr < ZONE_H;
        int add_row = (minting && band >= 0) ? flash[band] : 0;
        if (zrow) add_row += (int)(S.zflash[sy <= ZONE_PRICE_Y1 ? 0 : 1] * 40);
        bool row_revealed = !minting || (band >= 0 ? bandt[band] : !rosrow);
        int sys = sy + shy;
        const uint8_t *rc[2] = { rowp(buf, sy), rowp(B, sy) };
        const uint8_t *ru[2] = { rowp(buf, sy - 1), rowp(B, sy - 1) };
        const uint8_t *rd[2] = { rowp(buf, sy + 1), rowp(B, sy + 1) };
        const uint8_t *rs[2] = { rowp(buf, sys), rowp(B, sys) };
        const uint8_t *zi = zrow ? S.zink_a + zr * SCR_W : NULL;
        const uint8_t *nrow = NOISE[(zr + 640 - zs) & 63];
        /* the molten pool fades out toward the zone's top and bottom */
        int ptaper = zrow ? (zr < 14 ? zr * 18 : (ZONE_H - 1 - zr < 14 ? (ZONE_H - 1 - zr) * 18 : 256)) : 0;
        int zpool_row = zpool * ptaper >> 8;
        int gbase = sy * gy;
        int ubase = sy * 15, sbase = sy * LT.silk_sa + LT.silk_phase;
        for (int x = 1; x < SCR_W - 1; x++) {
            int k;
            int hot = 0;
            if (rosrow) {
                int dx = x - rcx;
                int d2 = dx * dx + dyc * dyc;
                if (d2 < r2) {
                    k = 0;
                    if (d2 >= rin2 && r2 > rin2) hot = (d2 - rin2) * 255 / (r2 - rin2);
                } else k = 1;
            } else {
                k = row_revealed ? 0 : 1;
            }
            int idx = rc[k][x];
            if (!idx) { o[x] = S.bg_row[y]; continue; }
            int h = idx & 127;
            int ddx = (rc[k][x + 1] & 127) - (rc[k][x - 1] & 127);
            int ddy = (rd[k][x] & 127) - (ru[k][x] & 127);
            int s = R[((x * 7 + ubase) >> 4) + ROFF0 + LT.roff] - ((ddx * lx + ddy * ly) >> 8);
            int xs = x + shx;
            if (xs >= 0 && xs < SCR_W) {
                int hs = rs[k][xs] & 127;
                if (hs > h + 3) s -= (hs - h) >> 1;
            }
            if (idx & 128) {
                s -= 15;            /* groove floors catch less light: keeps the lettering legible */
                if (silkrow) {
                    int dx = x - rcx;
                    if (dx * dx + dyc * dyc < silk_r2) s += ((tri256(x * LT.silk_ca + sbase) - 255) * LT.silk_amp) >> 8;
                }
            }
            if (gl) {
                int gi = ((x * gx + gbase) * 6554) >> 16;   /* /10 */
                if (gi >= 0 && gi < GL_N) s += (idx & 128) ? S.glint[gi] >> 2 : S.glint[gi];
            }
            s += add_row;
            if (s > 127) s = 127;
            if (s < 1) s = 1;
            uint16_t c = lut[(idx & 128) | s];
            if (hot) {
                int hh = hot * hotk >> 8;
                c = add565(c, hh, hh * 6 / 10, hh / 5);
            }
            if (zi && zg) {
                /* molten metal in the letters (fading as they melt), plus a thin molten
                   sheen over the whole zone while it is being re-cast */
                int ink = zi[x] * zma >> 8;
                int xt = x < 40 ? (x - 18) * 12 : (x > 130 ? (152 - x) * 12 : 256);
                if (xt < 0) xt = 0;
                if (xt > 256) xt = 256;
                int e = ((ink + (zpool_row * xt >> 8)) * zg >> 8) * (96 + nrow[(x + (zs >> 2)) & 63]) >> 8;
                if (e) c = add565(c, S.zglow_r * e >> 8, S.zglow_g * e >> 8, S.zglow_b * e >> 8);
            }
            o[x] = c;
        }
        o[0] = S.bg_row[y];
        o[SCR_W - 1] = S.bg_row[y];
    }
}

static void render_metal(strip_t *st, const uint8_t *buf, const uint16_t *lut, bool minting)
{
    render_metal_k(st, buf, lut, minting, 0);
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
#define PANEL_X0 17
#define PANEL_X1 153
#define PANEL_Y0 17
#define PANEL_Y1 303

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
    float ref1, ref2, mark_x, mark_v, end_x, end_v, mark_line_y;
    bool label_below;
    int16_t top[CH_W];      /* line height per column, 12.4 fixed; -1 = no line */
    int32_t inv[CH_W];      /* 2^24 / (distance from line to chart bottom) */
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
        if (colmin[x] > 1e8f) { CHT.top[x] = -1; continue; }
        CHT.top[x] = (int16_t)(colmin[x] * 16);                 /* 12.4 fixed */
        CHT.inv[x] = (int32_t)(65536.0f * 256 / fmaxf(1, CH_H - colmin[x]));
    }
    /* where the purchase marker sits relative to the line */
    CHT.mark_line_y = -1;
    if (CHT.mark_x >= 0) {
        int mx = (int)lrintf(CHT.mark_x * (CH_W - 1));
        if (mx >= 0 && mx < CH_W && colmin[mx] < 1e8f) CHT.mark_line_y = colmin[mx];
        /* put the label on the side the line is not: look at the line just around the marker */
        float my = chart_y(CHT.mark_v), sum = 0;
        int cnt = 0;
        for (int x = mx - 24; x <= mx + 24; x++)
            if (x >= 0 && x < CH_W && colmin[x] < 1e8f) { sum += colmin[x]; cnt++; }
        CHT.label_below = cnt ? (sum / cnt < my) : true;
        if (my > CH_H - 14) CHT.label_below = false;
        if (my < 14) CHT.label_below = true;
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
    uint16_t line = C_GOLD_HI;
    int y0 = CH_Y0 + yoff;
    int r1 = isnan(CHT.ref1) ? -999 : (int)lrintf(chart_y(CHT.ref1));
    int r2 = isnan(CHT.ref2) ? -999 : (int)lrintf(chart_y(CHT.ref2));
    static const uint8_t bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    for (int yy = 0; yy < st->h; yy++) {
        int y = st->y0 + yy, cy = y - y0;
        if (cy < 0 || cy >= CH_H) continue;
        uint16_t *o = &st->px[yy * SCR_W + CH_X0];
        const uint8_t *mr = &CHT.mask[cy * CH_W];
        bool d1 = cy == r1, d2 = cy == r2;
        int cy16 = cy * 16;
        for (int x = 0; x < CH_W; x++) {
            uint16_t c = o[x];
            if ((d1 || d2) && ((x >> 2) & 1) == 0) c = blend565(c, d1 ? C_GOLD : C_GOLD_DIM, (d1 ? 150 : 110) * alpha >> 8);
            int t = CHT.top[x];
            if (t >= 0 && cy16 > t) {
                /* gradient below the line, squared falloff, ordered dither before 565 */
                int u = 256 - (int)(((int64_t)(cy16 - t) * CHT.inv[x]) >> 20);
                if (u > 0) {
                    int a = (u * u >> 8) * alpha >> 8;              /* 0..256 */
                    int d = bayer[((y & 3) << 2) | ((x + CH_X0) & 3)];
                    int r = ((c >> 11) << 3) + (150 * a * 10 / 16 >> 8) + (d >> 1);
                    int g = (((c >> 5) & 63) << 2) + (110 * a * 10 / 16 >> 8) + (d >> 2);
                    int b = ((c & 31) << 3) + (40 * a * 10 / 16 >> 8) + (d >> 1);
                    c = rgb565(r, g, b);
                }
            }
            int l = mr[x] >> 4;
            if (l) c = blend565(c, line, (l * 17 * alpha) >> 8);
            o[x] = c;
        }
    }
    /* now: a warm point at the end of the line */
    float ex = CH_X0 + CHT.end_x + 0.5f, ey = y0 + chart_y(CHT.end_v);
    glow_add(st, ex, ey, 7, 255, 210, 140, 0.35f * alpha / 256);
    fill_rrect(st, ex - 2, ey - 2, ex + 2, ey + 2, 2, rgb565(255, 244, 214), alpha);
    /* your purchase: a tiny ingot, tied to the line when it sits off it */
    if (CHT.mark_x >= 0) {
        float mx = CH_X0 + CHT.mark_x * (CH_W - 1) + 0.5f, my = y0 + chart_y(CHT.mark_v);
        if (CHT.mark_line_y >= 0) {
            float ly = y0 + CHT.mark_line_y;
            if (fabsf(ly - my) > 4) {
                float a0 = fminf(ly, my) + 3, a1 = fmaxf(ly, my) - 3;
                for (float yy = a0; yy < a1; yy += 3)
                    fill_rrect(st, mx - 0.6f, yy, mx + 0.6f, fminf(yy + 1.5f, a1), 0, C_GOLD, alpha * 200 >> 8);
                fill_rrect(st, mx - 1.6f, ly - 1.6f, mx + 1.6f, ly + 1.6f, 1.6f, C_GOLD_HI, alpha);
            }
        }
        fill_rrect(st, mx - 4.5f, my - 3, mx + 4.5f, my + 3, 1.5f, rgb565(0, 0, 0), alpha * 160 >> 8);
        fill_rrect(st, mx - 3.5f, my - 2, mx + 3.5f, my + 2, 1, rgb565(236, 196, 104), alpha);
        /* label on whichever side has room */
        bool off_line = CHT.mark_line_y >= 0 && fabsf(y0 + CHT.mark_line_y - my) > 4;
        int w = text_width(&font_caps11, "YOU", 1.0f);
        float lx = clampf(mx - w / 2.0f, CH_X0, CH_X0 + CH_W - w);
        float lyb = CHT.label_below ? my + 15 : my - 6;
        (void)off_line; (void)lx; (void)lyb;
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
    if (S.base) render_metal_k(st, S.base, S.lut_back, false, 1);
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

static void hairline_d(strip_t *st, int y, int a)
{
    fill_rrect(st, 34, y - 0.5f, 76, y + 0.5f, 0, C_GOLD_DIM, a * 160 >> 8);
    fill_rrect(st, 94, y - 0.5f, 136, y + 0.5f, 0, C_GOLD_DIM, a * 160 >> 8);
    draw_seg(st, 85, y - 2.5f, 87.5f, y, 1.2f, C_GOLD_DIM, a);
    draw_seg(st, 87.5f, y, 85, y + 2.5f, 1.2f, C_GOLD_DIM, a);
    draw_seg(st, 85, y + 2.5f, 82.5f, y, 1.2f, C_GOLD_DIM, a);
    draw_seg(st, 82.5f, y, 85, y - 2.5f, 1.2f, C_GOLD_DIM, a);
}

typedef struct { strip_t *st; int first; float pos; int a; } qdctx_t;
static void q_px(void *vc, int x, int y, int a15, int ci)
{
    qdctx_t *c = vc;
    if (x < 0 || x >= SCR_W) return;
    float r = clampf((c->pos - (c->first + ci)) / 2.5f, 0, 1);     /* each letter fades in over ~2.5 letters */
    int al = (int)(r * c->a);
    if (!al) return;
    uint16_t *d = &c->st->px[(y - c->st->y0) * SCR_W + x];
    *d = blend565(*d, C_GOLD_HI, a15 * al / 15);
}

/* Back, page 0: today's line, and what has happened since you bought */
static void page_line(strip_t *st, int a, int yo)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    int cx = SCR_W / 2;
    char s1[48];
    text_draw_centered(st, &font_caps13, S.header, cx, 38 + yo, 2.4f, C_GOLD_DIM, a);
    hairline_d(st, 50 + yo, a);
    float pos = S.burin ? S.burin_pos : (float)S.nchars + 3;
    for (int i = 0; i < S.nql; i++) {
        int base = S.ql[i].y + yo;
        if (base - 20 > st->y0 + st->h || base + 8 < st->y0) continue;
        qdctx_t c = { st, S.ql[i].first, pos, a };
        text_walk(&font_ital17, S.ql[i].s, S.ql[i].x, base, 0.2f, st->y0, st->y0 + st->h, q_px, &c);
    }
    if (S.burin && S.burin_pos < S.nchars) {
        float x, y;
        char_pos(S.burin_pos, &x, &y);
        float fl = 0.75f + 0.25f * sinf(S.clock * 53) * sinf(S.clock * 31);
        glow_add(st, x, y + yo, 9, 255, 220, 160, 0.5f * fl * a / 256);
        glow_add(st, x, y + yo, 3, 255, 255, 240, 1.0f * a / 256);
    }
    hairline_d(st, 186 + yo, a);
    text_draw_centered(st, &font_caps11, "SINCE YOU BOUGHT", cx, 206 + yo, 0.4f, C_GOLD_DIM, a);
    if (q->valid) {
        double g = (q->price / p->price - 1) * 100;
        snprintf(s1, sizeof s1, "%+.2f%%", g);
        text_draw_centered(st, &font_mono32, s1, cx, 242 + yo, 0, g >= 0 ? C_UP : C_DOWN, a);
        snprintf(s1, sizeof s1, "$%.2f  >  $%.2f", p->price, q->price);
        text_draw_centered(st, &font_mono11, s1, cx, 260 + yo, 0, C_GOLD_DIM, a);
    }
    /* provenance: the moment it began */
    char d[24];
    date_of(p->buy_ts, d, sizeof d);
    int64_t bj = ((p->buy_ts + LOCAL_UTC_OFFSET) % 86400 + 86400) % 86400;
    snprintf(s1, sizeof s1, "%s  %02d:%02d", d, (int)(bj / 3600), (int)(bj / 60 % 60));
    text_draw_centered(st, &font_mono11, s1, cx, 280 + yo, 0, rgb565(120, 104, 80), a);
}

/* Back, page 1: the market, only what matters */
static void page_market(strip_t *st, int a, int yo)
{
    const quote_t *q = &S.m->quote;
    int cx = SCR_W / 2;
    char s1[40], s2[40];
    int64_t now = S.m->now, nopen = 0, close_at = 0;
    mkt_t ms = now > 0 ? market_state(now, &nopen, &close_at) : MKT_CLOSED;
    static const char *MS[] = { "CLOSED", "PRE", "OPEN", "AFTER" };
    text_draw(st, &font_caps11, "ANT", 26, 36 + yo, 2.0f, C_GOLD_DIM, a);
    {
        const char *t = MS[ms];
        int w = text_width(&font_caps11, t, 1.6f);
        uint16_t c = ms == MKT_OPEN ? C_GOLD_HI : (ms == MKT_CLOSED ? rgb565(120, 112, 100) : C_GOLD);
        text_draw(st, &font_caps11, t, 140 - w, 36 + yo, 1.6f, c, a);
        fill_rrect(st, 140 - w - 9, 30 + yo, 140 - w - 4, 35 + yo, 2.5f, c, a);
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
        snprintf(s1, sizeof s1, "%+.2f%% TODAY", chg);
        text_draw_centered(st, &font_mono11, s1, cx, 99 + yo, 0, chg >= 0 ? C_UP : C_DOWN, a);
    } else {
        text_draw_centered(st, &font_mono32, "$--.--", cx, 80 + yo, 0, C_GOLD_DIM, a);
    }
    if (CHT.ok) draw_chart(st, a, yo);
    if (q->valid) {
        snprintf(s2, sizeof s2, "x%.2f", q->price / IPO_PRICE);
        row_lr(st, 222 + yo, "SINCE IPO", s2, q->price >= IPO_PRICE ? C_UP : C_DOWN, a);
        double total = q->total_shares > 1e6 ? q->total_shares : DEFAULT_TOTAL_SHARES;
        char s3[24];
        fmt_big(total * q->price, s3, sizeof s3);
        snprintf(s2, sizeof s2, "$%s", s3);
        row_lr(st, 240 + yo, "MKT CAP", s2, C_GOLD, a);
    }
    if ((ms == MKT_PRE || ms == MKT_AFTER) && q->ext_price > 0) {
        snprintf(s1, sizeof s1, "%s $%.2f %+.2f%%", ms == MKT_PRE ? "PRE" : "AFTER", q->ext_price, q->ext_pct);
        text_draw_centered(st, &font_mono11, s1, cx, 270 + yo, 0, q->ext_pct >= 0 ? C_UP : C_DOWN, a);
    } else if (now > 0) {
        if (ms == MKT_OPEN) { fmt_dur(close_at - now, s2, sizeof s2); snprintf(s1, sizeof s1, "CLOSES IN %s", s2); }
        else { fmt_dur(nopen - now, s2, sizeof s2); snprintf(s1, sizeof s1, "OPENS IN %s", s2); }
        text_draw_centered(st, &font_caps11, s1, cx, 270 + yo, 1.0f, rgb565(130, 114, 88), a);
    }
}

static void render_back(strip_t *st)
{
    render_enamel(st);
    if (st->y0 > PANEL_Y1 || st->y0 + st->h < PANEL_Y0) return;
    int a = 256, yo = 0;
    if (S.mfading) {
        float t = S.mfade_t;
        if (t < 0.12f) a = (int)(256 * (1 - t / 0.12f));
        else { float u = clampf((t - 0.12f) / 0.18f, 0, 1); a = (int)(256 * ease_out_cubic(u)); yo = (int)lrintf(6 * (1 - ease_out_cubic(u))); }
    }
    if (S.mpage == 0) page_line(st, a, yo);
    else page_market(st, a, yo);
    for (int i = 0; i < 2; i++) {
        float x = SCR_W / 2.0f + (i ? 5 : -5);
        fill_rrect(st, x - 2, 292, x + 2, 296, 2, i == S.mpage ? C_GOLD_HI : rgb565(70, 60, 44), 256);
    }
    const quote_t *q = &S.m->quote;
    if (q->valid && fabs(q->chg_pct) >= 5) {
        uint16_t c = q->chg_pct >= 0 ? C_UP : C_DOWN;
        draw_seg(st, 144, 21, 147, 24, 1.6f, c, 256);
        draw_seg(st, 147, 24, 144, 27, 1.6f, c, 256);
        draw_seg(st, 144, 27, 141, 24, 1.6f, c, 256);
        draw_seg(st, 141, 24, 144, 21, 1.6f, c, 256);
    }
}

/* ---------------------------------------------------------------- faces + flip */

static void render_face(int f, strip_t *st)
{
    if (f == F_FRONT) { if (S.front) render_metal(st, S.front, S.lut_front, false); }
    else render_back(st);
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
                    int hs = (id & 127) - 25;
                    cc = id ? S.lut_front[(id & 128) | (hs < 1 ? 1 : hs)] : bg;
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
    build_noise();
    ensure_base();
    if (!S.base) return false;
    if (m->mint.minted) {
        S.st = ST_FACES;
        if (!ensure_front(&m->mint.p, m->mint.serial)) return false;
        if (!m->assay.valid) {      /* first weighing = the purchase itself */
            m->assay = (assay_t){ .valid = true, .price = m->mint.p.price, .grams = 0, .close_day = 0 };
            S.events |= EV_ASSAY;
        }
        zone_show(m->assay.price, m->assay.grams);
        prepare_line();
        glint_start(1.2f, 26, 22, 6, 8, 0);
    } else {
        S.st = ST_SETUP;
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
    S.as_phase = 0;
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
    model_t *m = S.m;
    if (!S.rehearsal) {
        m->mint.minted = true;
        m->mint.p = m->setup.pending;
        m->mint.mint_ts = m->now;
        m->patina_day = days_held(m);
        /* the first weighing is the purchase itself */
        m->assay = (assay_t){ .valid = true, .price = m->mint.p.price, .grams = grams_for(m->mint.p.price), .close_day = 0 };
        S.events |= EV_MINTED | EV_ASSAY;
        zone_show(m->assay.price, m->assay.grams);
    } else {
        zone_show(m->assay.price, m->assay.grams);
    }
}

static void ledger_prepare(void)
{
    const quote_t *q = &S.m->quote;
    const purchase_t *p = &S.m->mint.p;
    double from = S.have_shown ? S.shown_price : p->price;
    double to = q->valid ? q->price : from;
    S.led_from[0] = from; S.led_to[0] = to;
    S.led_from[1] = 0; S.led_to[1] = q->chg_pct;
    S.shown_price = to;
    S.have_shown = q->valid;
    S.ledger_ready = true;
    S.ledger_t = 0;
    S.ledger_anim = false;
}

static void finish_line(void)
{
    S.burin = false;
    S.burin_pos = (float)S.nchars;
    if (S.m->line_seen_day != S.line_for_day) {
        S.m->line_seen_day = S.line_for_day;
        S.events |= EV_LINE_SEEN;
    }
}

static void land_on(int f)
{
    S.face = f;
    S.flipping = false;
    S.cooldown = S.m->tune.shake_cooldown_ms / 1000.0f;
    if (f == F_FRONT) glint_start(0.5f, 24, 18, 6, 8, 0);
    if (f == F_BACK) {
        if (S.mpage == 0) {
            int day = days_held(S.m);
            S.burin = !(S.m->line_seen_day == day && day > 0);
            S.burin_pos = S.burin ? -1.5f : (float)S.nchars;
        } else {
            if (!S.ledger_ready) ledger_prepare();
            S.ledger_ready = false;
            S.ledger_t = 0;
            S.ledger_anim = S.m->quote.valid;
        }
    }
}

static void start_flip(void)
{
    if (S.face == F_BACK && S.burin) finish_line();
    S.face_to = S.face == F_FRONT ? F_BACK : F_FRONT;
    if (S.face_to == F_BACK) {
        if (line_is_stale()) prepare_line();
        int day = days_held(S.m);
        if (!(S.m->line_seen_day == day && day > 0)) S.mpage = 0;     /* a new line comes first */
        if (S.mpage == 0) { S.burin = false; S.burin_pos = -100; }    /* blank until it lands */
        else { ledger_prepare(); build_chart(0); }
    }
    S.flipping = true;
    S.flip_t = 0;
    S.fth = 0;
    S.fvel = 2.5f;          /* the shake gives it a small kick */
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

static void back_tick(float dt, bool tap)
{
    model_t *m = S.m;
    const tune_t *tu = &m->tune;
    if (tap) {
        if (S.mpage == 0 && S.burin) finish_line();          /* first tap completes the line */
        else if (!S.mfading) { S.mfading = true; S.mfade_t = 0; S.mpage_to = 1 - S.mpage; }
    }
    if (S.mfading) {
        float before = S.mfade_t;
        S.mfade_t += dt;
        if (before < 0.12f && S.mfade_t >= 0.12f) {
            S.mpage = S.mpage_to;
            if (S.mpage == 1) { ledger_prepare(); build_chart(0); S.ledger_ready = false; S.ledger_anim = m->quote.valid; }
        }
        if (S.mfade_t >= 0.30f) S.mfading = false;
    }
    if (S.mpage == 0) {
        if (S.burin) {
            S.burin_pos += dt * 1000.0f / (tu->burin_ms > 5 ? tu->burin_ms : 5);
            if (S.burin_pos >= S.nchars + 3) { finish_line(); glint_start(0.6f, 18, 18, 6, 8, 0); }
        } else if (line_is_stale()) {
            prepare_line();
            S.burin = true;
            S.burin_pos = -1.5f;
        }
    } else {
        if (CHT.page != 0 || CHT.rev != chart_rev_now()) { build_chart(0); S.dirty = true; }
        int minute = (int)(m->now / 60);
        if (minute != S.last_minute) { S.last_minute = minute; S.dirty = true; }
        if (S.ledger_anim) { S.ledger_t += dt; if (S.ledger_t > 1.2f) S.ledger_anim = false; }
        if (!S.ledger_anim && !S.mfading && m->quote.valid && (!S.have_shown || fabs(m->quote.price - S.shown_price) > 0.004)) {
            ledger_prepare();
            S.ledger_ready = false;
            S.ledger_anim = true;
        }
    }
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

    /* ---- tilt → light. Light only re-renders when it actually moves. */
    {
        float ax = in->tilt_valid ? in->tilt_x : 0, ay = in->tilt_valid ? in->tilt_y : 0;
        if (tu->tilt_swap) { float t = ax; ax = ay; ay = t; }
        if (tu->tilt_flip_x) ax = -ax;
        if (tu->tilt_flip_y) ay = -ay;
        ax *= tu->tilt_gain;
        ay *= tu->tilt_gain;
        float k = clampf(dt / 0.09f, 0, 1);
        S.tx += (ax - S.tx) * k;
        S.ty += (ay - S.ty) * k;
        S.drift += dt * tu->desk_drift * 0.2f;
        bool moved = fabsf(S.tx - S.tx_applied) > 0.004f || fabsf(S.ty - S.ty_applied) > 0.004f || tu->desk_drift > 0;
        if (moved || !LT.key) {
            S.tx_applied = S.tx;
            S.ty_applied = S.ty;
            light_update(S.tx, S.ty, tu->silk, tu->shadow_px, S.drift);
        }
        float dmove = fabsf(S.tx - S.ref_tx) + fabsf(S.ty - S.ref_ty);
        if (dmove > 0.12f) {
            if (S.still_t > 45 && S.st == ST_FACES && !S.flipping) glint_start(0.9f, 26, 22, 6, 8, 0);
            S.ref_tx = S.tx; S.ref_ty = S.ty;
            S.still_t = 0;
            act = true;
        } else {
            S.still_t += dt;
        }
    }
    if (act) S.events |= EV_ACTIVITY;

    /* ---- touch: long press → zoom, tap → page / skip */
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
            if (ensure_front(&m->setup.pending, m->mint.serial)) {
                S.st = ST_READY; S.t = 0; S.heat = 0; S.rehearsal = false;
                zone_show(m->setup.pending.price, 0);
            }
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
        const float t0 = 1.85f, gap = 0.17f;
        while (S.stamps_done < S.lay.nbands && t >= t0 + S.stamps_done * gap) {
            float w = S.lay.bands[S.stamps_done].weight;
            S.dyv += 70 * w;
            S.band_flash[S.stamps_done] = 1;
            S.led_pulse = fmaxf(S.led_pulse, 0.5f + 0.4f * w);
            S.stamps_done++;
        }
        for (int b = 0; b < 6; b++) S.band_flash[b] = fmaxf(0, S.band_flash[b] - dt / 0.28f);
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
            S.as_phase = 1;         /* hold the first weighing for a few seconds */
            S.as_t = 0;
            prepare_line();
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
        } else if ((in->btn_short || (in->shake && !S.need_calm)) && S.cooldown <= 0 && S.as_phase != 2) {
            start_flip();
        }
        zone_tick(dt);
        if (S.face == F_BACK && !S.flipping) back_tick(dt, tap);
        if (S.pressed && !S.long_fired && !S.flipping && S.as_phase != 2 && S.press_t * 1000 >= tu->longpress_ms) {
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
    if (S.st == ST_SETUP || S.st == ST_READY) S.led_pulse = 0;
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
    } else if (S.st == ST_FACES && S.burin && !S.flipping && S.face == F_BACK && S.mpage == 0) {
        float f = 0.08f + 0.06f * sinf(S.clock * 53) * sinf(S.clock * 31);
        R = f; G = f * 0.75f; B = f * 0.45f;
    } else if (S.st == ST_FACES && S.as_phase == 2) {
        float er, eg, eb;
        blackbody(0.6f, &er, &eg, &eb);
        float lv = S.zglow * 0.5f;
        R = er * lv; G = eg * lv; B = eb * lv;
    }
    R += S.led_pulse * 1.0f; G += S.led_pulse * 0.7f; B += S.led_pulse * 0.3f;
    if (S.st == ST_FACES && !(S.burin && S.face == F_BACK) && S.m->quote.valid && S.m->now > 0 &&
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
    S.partial = false;
    if (S.dirty) { S.dirty = false; return true; }
    if (S.fade < 1 || S.st != ST_FACES || S.flipping || S.glint_on || S.burin || S.ledger_anim || S.mfading ||
        S.sweeping || S.pressed || S.ring_fade > 0 || S.m->settings_mode || S.as_phase == 2 || S.dy != 0)
        return true;
    if (S.face == F_FRONT && S.furnace > 0.003f) { S.partial = true; return true; }   /* only the molten letters move */
    return false;
}

void scene_dirty_rows(int *y0, int *y1)
{
    if (S.partial) { *y0 = ZONE_Y0 - 2; *y1 = ZONE_Y1 + 3; }
    else { *y0 = 0; *y1 = SCR_H; }
}
