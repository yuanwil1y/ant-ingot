/* Host simulator: drives the scene with a scripted timeline and writes raw RGB frames to stdout. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../main/core/scene.h"

static model_t M;
static uint16_t px[SCR_W * 40];

static void frame(FILE *out, int scale)
{
    static unsigned char row[SCR_W * 3 * 4];
    for (int y0 = 0; y0 < SCR_H; y0 += 40) {
        strip_t st = { px, y0, 40 };
        scene_render(&st);
        for (int yy = 0; yy < 40; yy++) {
            for (int x = 0; x < SCR_W; x++) {
                uint16_t c = px[yy * SCR_W + x];
                unsigned char r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
                r |= r >> 5; g |= g >> 6; b |= b >> 5;
                for (int k = 0; k < scale; k++) { unsigned char *p = &row[(x * scale + k) * 3]; p[0] = r; p[1] = g; p[2] = b; }
            }
            for (int k = 0; k < scale; k++) fwrite(row, 1, SCR_W * scale * 3, out);
        }
    }
}

int main(int argc, char **argv)
{
    const char *script = argc > 1 ? argv[1] : "full";
    int scale = 2;
    M.tune = tune_defaults();
    snprintf(M.setup.ap_ssid, sizeof M.setup.ap_ssid, "A Share of Anthropic Setup");
    M.setup.pending.price = 91.42; M.setup.pending.buy_ts = 1795269060; M.setup.pending.shares = 1;
    M.mint.serial = 0x4f2a91c3;
    M.now = 1795269060 + 3600;
    M.quote.valid = 1; M.quote.price = 88.10; M.quote.chg_pct = -9.33; M.quote.total_shares = 13086672591.0;
    snprintf(M.quote.when, sizeof M.quote.when, "2026-11-20 10:31:02");
    snprintf(M.line, sizeof M.line, "Day one. The price wobbled; I did not. Gold is patient by design.");
    M.line_day = 1;
    bool market = !strcmp(script, "market");
    bool tilt = !strcmp(script, "tilt");
    bool assay = !strcmp(script, "assay");
    bool heal = !strcmp(script, "heal");
    bool flipcount = !strcmp(script, "flipcount");     /* the v0.4.6 bug: boot before the clock is set */
    bool minted_start = !strcmp(script, "faces") || market || tilt || assay || heal || flipcount;
    if (market || tilt || assay || heal || flipcount) {
        M.setup.pending.price = 88.00; M.setup.pending.buy_ts = 1795185000;
        snprintf(M.setup.pending.inscription, sizeof M.setup.pending.inscription, "FOR THE LONG RUN.");
        M.now = 1795185000 + 13200;
        FILE *f = fopen("mkt.txt", "r");
        char tag;
        while (f && fscanf(f, " %c", &tag) == 1) {
            if (tag == 'I') fscanf(f, "%d", &M.intra.date);
            else if (tag == 'M') { int mi; float px; fscanf(f, "%d %f", &mi, &px); M.intra.min[M.intra.n] = mi; M.intra.px[M.intra.n++] = px; }
            else if (tag == 'D') { int d; float c; fscanf(f, "%d %f", &d, &c); M.daily.day[M.daily.n] = d; M.daily.close[M.daily.n++] = c; }
            else if (tag == 'Q') fscanf(f, "%lf %lf %lf %lf %lf %lf %lf", &M.quote.price, &M.quote.prev_close, &M.quote.open, &M.quote.high, &M.quote.low, &M.quote.chg_pct, &M.quote.volume);
        }
        if (f) fclose(f);
        M.intra.rev = M.daily.rev = 1;
        M.quote.valid = 1;
        M.quote.gold = 3520.48;
        M.quote.total_shares = 13086672591.0;
        M.quote.fetched_ts = M.now;
        snprintf(M.quote.when, sizeof M.quote.when, "2026-11-20 13:10:00");
        M.assay = (assay_t){ .valid = 1, .price = 88.00, .grams = 0.7522, .close_day = 0 };
        M.line_day = 1;
    }
    if (minted_start) { M.mint.minted = 1; M.mint.p = M.setup.pending; M.patina_day = 1; }
    if (!scene_init(&M)) { fprintf(stderr, "init failed\n"); return 1; }
    const float dt = 1.0f / 30;
    float t = 0;
    int nframes = 0;
    float end = flipcount ? 9 : heal ? 10 : assay ? 12 : (tilt ? 14 : (market ? 16 : (minted_start ? 30 : 46)));
    while (t < end) {
        input_t in = { 0 };
        if (!minted_start) {
            /* setup flow */
            if (t > 2.5f && t < 5.5f) {
                M.setup.stage = SETUP_CHECKING;
                M.setup.chk[0] = t < 3.2f ? CHK_RUNNING : CHK_OK;
                M.setup.chk[1] = t < 3.2f ? CHK_PENDING : (t < 4.0f ? CHK_RUNNING : CHK_OK);
                M.setup.chk[2] = t < 4.0f ? CHK_PENDING : (t < 4.9f ? CHK_RUNNING : CHK_OK);
            }
            if (t >= 5.5f && M.setup.stage != SETUP_READY && !M.mint.minted) M.setup.stage = SETUP_READY;
            /* shaking from 7.5s for ~2s, with a pause in the middle */
            if (t > 7.5f && t < 10.2f && !(t > 8.4f && t < 8.8f)) in.shake_energy = 1.4f + 0.4f * sinf(t * 9);
        }
        if (flipcount) {
            /* 0.5-2.0 s: one long hard shake (detector fires every ~0.2 s). 2.0-3.5 s: still.
               3.5-4.0 s: a short shake. 4.0-6 s: still. 6.0-6.3 s: shake again. */
            bool shaking = (t > 0.5f && t < 2.0f) || (t > 3.5f && t < 4.0f) || (t > 6.0f && t < 6.3f);
            in.shake_energy = shaking ? 1.8f : 0.05f;
            static float next = 0;
            if (shaking && t >= next) { in.shake = 1; next = t + 0.2f; }
            static int last_face = -1, flips = 0;
            scene_tick(dt, &in);
            scene_take_events();
            frame(stdout, scale);
            extern int scene_debug_face(void);
            int f = scene_debug_face();
            if (f < 0) { t += dt; continue; }
            if (last_face >= 0 && f != last_face) { flips++; fprintf(stderr, "t=%.2f landed on %s (flip %d)\n", t, f ? "back" : "front", flips); }
            last_face = f;
            t += dt;
            continue;
        }
        if (heal) {
            /* 0-3 s: no clock yet, intraday quote (must NOT weigh). 3-6 s: clock set, market open.
               6 s: after the close, with a stale wrong weighing on record → re-weigh once. */
            static int phase = -1;
            if (phase < 0) { M.now = 0; snprintf(M.quote.when, sizeof M.quote.when, "2026-11-20 13:10:00"); M.quote.price = 87.45; phase = 0; }
            if (t >= 3 && phase == 0) { M.now = 1795185000 + 13200; phase = 1; }
            if (t >= 6 && phase == 1) {
                M.assay = (assay_t){ .valid = 1, .price = 87.45, .grams = 0.77, .close_day = 20777 };
                M.now = 1795185000 + 23800; M.quote.price = 88.36;
                snprintf(M.quote.when, sizeof M.quote.when, "2026-11-20 16:00:01");
                phase = 2;
            }
            M.quote.fetched_ts = M.now;
            scene_tick(dt, &in);
            uint32_t ev = scene_take_events();
            if (ev & EV_ASSAY) fprintf(stderr, "t=%.2f assay -> $%.2f day %d\n", t, M.assay.price, M.assay.close_day);
            frame(stdout, scale);
            t += dt;
            continue;
        }
        if (assay) {
            /* market open: the letters glow; then the close arrives and the bar is weighed */
            in.tilt_valid = 1; in.tilt_x = 0.1f * sinf(t * 0.7f); in.tilt_y = 0.85f;
            if (t >= 4.5f && M.now < 1795185000 + 23800) {
                M.now = 1795185000 + 23800;
                M.quote.price = 84.78; M.quote.fetched_ts = M.now;
                snprintf(M.quote.when, sizeof M.quote.when, "2026-11-20 16:00:01");
            }
            M.quote.fetched_ts = M.now;
            scene_tick(dt, &in);
            if (scene_take_events() & EV_ASSAY) fprintf(stderr, "t=%.2f assay -> $%.2f day %d\n", t, M.assay.price, M.assay.close_day);
            frame(stdout, scale);
            t += dt;
            continue;
        }
        if (tilt) {
            /* held upright, a slow wrist wobble, then laid flat on the desk, then the back */
            in.tilt_valid = 1;
            if (t < 9) {
                float w = t < 1 ? t : 1;
                in.tilt_x = 0.42f * sinf(t * 1.25f) * w;
                in.tilt_y = 0.80f + 0.18f * sinf(t * 0.8f + 1.0f) * w;
            } else { in.tilt_x = 0.25f * sinf(t * 1.6f); in.tilt_y = 0.85f; }
            if (fabsf(t - 9.0f) < dt / 2) in.shake = 1;
            scene_tick(dt, &in);
            scene_take_events();
            frame(stdout, scale);
            t += dt;
            continue;
        }
        if (market) {
            M.quote.fetched_ts = M.now;
            if (fabsf(t - 1.0f) < dt / 2 || fabsf(t - 14.5f) < dt / 2) in.shake = 1;
            if ((t > 7.5f && t < 7.6f) || (t > 11.0f && t < 11.1f)) { in.touch = 1; in.tx = 85; in.ty = 160; }
            scene_tick(dt, &in);
            scene_take_events();
            frame(stdout, scale);
            t += dt;
            continue;
        }
        float o = minted_start ? -13.5f : 0;   /* faces script starts at the front face */
        float tt = t - o;
        if (fabsf(tt - 15.5f) < dt / 2) in.shake = 1;   /* to daily line */
        if (fabsf(tt - 21.0f) < dt / 2) in.shake = 1;   /* to ledger */
        if (fabsf(tt - 24.5f) < dt / 2) in.shake = 1;   /* back to front */
        if (tt > 26.5f && tt < 27.4f) { in.touch = 1; in.tx = 90; in.ty = 200; }   /* long press */
        if (tt > 38.0f && tt < 38.1f) { in.touch = 1; in.tx = 90; in.ty = 200; }   /* tap to return */
        scene_tick(dt, &in);
        uint32_t ev = scene_take_events();
        (void)ev;
        frame(stdout, scale);
        nframes++;
        t += dt;
    }
    fprintf(stderr, "%d frames\n", nframes);
    return 0;
}

const uint8_t *surf_lookup(int slot, uint32_t key) { (void)slot; (void)key; return NULL; }
const uint8_t *surf_commit(int slot, uint32_t key, uint8_t *ram) { (void)slot; (void)key; return ram; }
