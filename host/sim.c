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
    bool minted_start = !strcmp(script, "faces");
    if (minted_start) { M.mint.minted = 1; M.mint.p = M.setup.pending; M.patina_day = 1; }
    if (!scene_init(&M)) { fprintf(stderr, "init failed\n"); return 1; }
    const float dt = 1.0f / 30;
    float t = 0;
    int nframes = 0;
    float end = minted_start ? 30 : 46;
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
