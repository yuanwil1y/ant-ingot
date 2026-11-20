/* The ingot's whole visual life: setup, heating, minting, faces, zoom.
 * Portable C; the firmware and the host simulator both drive it.
 *
 * Threading: call scene_tick() with the model locked; scene_render() only touches
 * scene-owned state, so it can run without the lock.
 */
#pragma once
#include "gfx.h"
#include "model.h"

typedef struct {
    float shake_energy;     /* smoothed high-passed |accel| in g */
    bool shake;             /* a back-and-forth shake gesture just happened */
    bool touch;
    int tx, ty;
    bool btn_short;         /* BOOT button click */
} input_t;

enum {
    EV_MINTED = 1,          /* model->mint was filled in: persist it */
    EV_LINE_SEEN = 2,       /* model->line_seen_day updated */
    EV_PATINA_SEEN = 4,     /* model->patina_day updated */
    EV_ACTIVITY = 8,        /* user did something (wake backlight) */
};

bool scene_init(model_t *m);            /* allocates buffers, generates surfaces */
void scene_tick(float dt, const input_t *in);
void scene_render(strip_t *st);
void scene_led(uint8_t *r, uint8_t *g, uint8_t *b);
uint32_t scene_take_events(void);
bool scene_animating(void);             /* false → frame is static, rendering can pause */
void scene_invalidate(void);            /* model changed: render at least one frame */
void scene_rehearse(void);              /* replay heating + minting without touching data */

/* Surface cache provided by the platform (the firmware keeps them in flash).
 * lookup: return a stored surface for key, or NULL.
 * commit: take ownership of a freshly generated RAM surface and return where it now lives. */
const uint8_t *surf_lookup(int slot, uint32_t key);
const uint8_t *surf_commit(int slot, uint32_t key, uint8_t *ram);
