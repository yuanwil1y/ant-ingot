/* Telling a shake from picking the bar up to look at it.
 *
 * Gravity is tracked with the gyro (a complementary filter), so turning the bar over
 * does not look like acceleration. What remains is real linear acceleration; a shake
 * is three or more strong, alternating swings of it in quick succession, each of
 * which also pushes the total acceleration well away from 1 g. Rotation alone never
 * does that, however fast.
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
} motion_t;

typedef struct {
    bool shake;
    float energy;
    float gx, gy, gz;
} motion_out_t;

void motion_init(motion_t *m);
/* a: accel in g. w: gyro in rad/s, or NULL if there is no gyro. */
void motion_step(motion_t *m, const float a[3], const float *w, float dt, int64_t now_us,
                 float thresh_g, motion_out_t *out);
