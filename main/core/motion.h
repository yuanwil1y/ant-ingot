/* Telling a shake from picking the bar up to look at it.
 *
 * A shake is two strong, alternating swings of linear acceleration in quick
 * succession, each of which also pushes the total acceleration well away from 1 g.
 * Turning the bar over, however fast, keeps |a| at 1 g, so it never counts.
 * Gyro-tracked gravity is supported (pass w) but the firmware passes NULL: the
 * board's gyro scale could not be verified and a wrong scale hides real shakes.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool init;
    float g[3];             /* gravity estimate, device frame, g */
    float bias[3];          /* gyro bias, rad/s */
    float energy;           /* smoothed |linear accel|, g */
    int64_t last_peak_us;
    float last_dir[3];
    int count;
    bool armed;
    /* the swing in progress (between crossing the threshold and falling back) */
    float sw_mag, sw_dev, sw_dir[3];
} motion_t;

typedef struct {
    bool shake;
    float energy;
    float lin;              /* |linear accel| this sample, g (for the shake meter) */
    float gx, gy, gz;
} motion_out_t;

void motion_init(motion_t *m);
/* a: accel in g. w: gyro in rad/s, or NULL if there is no gyro. */
void motion_step(motion_t *m, const float a[3], const float *w, float dt, int64_t now_us,
                 float thresh_g, motion_out_t *out);
