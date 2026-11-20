#include "motion.h"
#include <math.h>
#include <string.h>

#define SHAKE_SWINGS 2          /* back and forth */
#define SWING_GAP_US 600000     /* max time between swings */
#define NORM_DEV_MIN 0.34f      /* somewhere in the swing, |a| must leave 1 g by this much */

void motion_init(motion_t *m)
{
    memset(m, 0, sizeof *m);
    m->g[2] = 1;
    m->armed = true;
}

void motion_step(motion_t *m, const float a[3], const float *w, float dt, int64_t now_us,
                 float thr, motion_out_t *out)
{
    if (!m->init) { memcpy(m->g, a, sizeof m->g); m->init = true; }
    float an = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (w) {
        float wc[3] = { w[0] - m->bias[0], w[1] - m->bias[1], w[2] - m->bias[2] };
        float wn = sqrtf(wc[0] * wc[0] + wc[1] * wc[1] + wc[2] * wc[2]);
        /* a world-fixed vector seen from a rotating body: dg/dt = -w x g */
        float cx = wc[1] * m->g[2] - wc[2] * m->g[1];
        float cy = wc[2] * m->g[0] - wc[0] * m->g[2];
        float cz = wc[0] * m->g[1] - wc[1] * m->g[0];
        m->g[0] -= cx * dt; m->g[1] -= cy * dt; m->g[2] -= cz * dt;
        /* drift correction toward the accelerometer, gentler while it is being shaken */
        float k = fabsf(an - 1) < 0.15f ? 0.02f : 0.004f;
        for (int i = 0; i < 3; i++) m->g[i] += (a[i] - m->g[i]) * k;
        /* learn the gyro bias while resting */
        if (fabsf(an - 1) < 0.04f && wn < 0.12f)
            for (int i = 0; i < 3; i++) m->bias[i] += (w[i] - m->bias[i]) * 0.004f;
    } else {
        for (int i = 0; i < 3; i++) m->g[i] += (a[i] - m->g[i]) * 0.012f;
    }
    float lin[3] = { a[0] - m->g[0], a[1] - m->g[1], a[2] - m->g[2] };
    float mag = sqrtf(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
    m->energy += (mag - m->energy) * (dt / 0.15f);

    bool ev = false;
    if (now_us - m->last_peak_us > SWING_GAP_US) m->count = 0;
    if (m->armed) {
        if (mag > thr) {                      /* a swing begins */
            m->armed = false;
            m->sw_mag = 0;
            m->sw_dev = 0;
        }
    }
    if (!m->armed) {
        /* follow the swing to its peak: direction at the peak, largest departure from 1 g */
        if (mag > m->sw_mag) {
            m->sw_mag = mag;
            for (int i = 0; i < 3; i++) m->sw_dir[i] = lin[i] / mag;
        }
        float dev = fabsf(an - 1);
        if (dev > m->sw_dev) m->sw_dev = dev;
        if (mag < thr * 0.45f) {              /* the swing is over: judge it as a whole */
            m->armed = true;
            if (m->sw_dev >= NORM_DEV_MIN) {
                float dot = m->sw_dir[0] * m->last_dir[0] + m->sw_dir[1] * m->last_dir[1] + m->sw_dir[2] * m->last_dir[2];
                if (m->count > 0 && dot < -0.3f) m->count++;
                else m->count = 1;
                memcpy(m->last_dir, m->sw_dir, sizeof m->sw_dir);
                m->last_peak_us = now_us;
                if (m->count >= SHAKE_SWINGS) { ev = true; m->count = 0; m->last_peak_us = 0; }
            }
        }
    }
    float gn = sqrtf(m->g[0] * m->g[0] + m->g[1] * m->g[1] + m->g[2] * m->g[2]);
    if (gn < 0.2f) gn = 1;
    out->shake = ev;
    out->energy = m->energy;
    out->lin = mag;
    out->gx = m->g[0] / gn; out->gy = m->g[1] / gn; out->gz = m->g[2] / gn;
}
