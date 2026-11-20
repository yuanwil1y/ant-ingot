/* Telling a shake from picking the bar up to look at it.
 *
 * Only the size of the total acceleration is used, never its direction: turning the
 * bar over (however fast) keeps |a| at 1 g, so it can never look like a shake, and
 * there is no gravity estimate to fall behind when the wrist twists. A shake is a
 * run of jolts: |a| leaves 1 g by a margin three or more separate times within
 * 0.6 s. Lifting, putting down or snatching the bar up makes one or two.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool init;
    float g[3];             /* gravity estimate, device frame, g */
    float bias[3];          /* gyro bias, rad/s */
    float energy;           /* smoothed |linear accel|, g */
    /* jolts: |a| away from 1 g */
    bool in_jolt;
    int jolt_len;
    int64_t jolts[8];
    int nj;
} motion_t;

typedef struct {
    bool shake;
    float energy;
    float lin;              /* how far |a| is from 1 g this sample (for the shake meter) */
    float gx, gy, gz;
} motion_out_t;

void motion_init(motion_t *m);
/* a: accel in g. w: gyro in rad/s, or NULL if there is no gyro. */
void motion_step(motion_t *m, const float a[3], const float *w, float dt, int64_t now_us,
                 float thresh_g, motion_out_t *out);
