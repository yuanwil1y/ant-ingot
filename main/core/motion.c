#include "motion.h"
#include <math.h>
#include <string.h>

#define SHAKE_SWINGS 3          /* back, forth, back */
#define SWING_GAP_US 450000     /* max time between swings */
#define NORM_DEV_MIN 0.35f      /* a swing must push |a| this far from 1 g */

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
    if (m->armed && mag > thr && fabsf(an - 1) > NORM_DEV_MIN) {
        m->armed = false;
        float dir[3] = { lin[0] / mag, lin[1] / mag, lin[2] / mag };
        float dot = dir[0] * m->last_dir[0] + dir[1] * m->last_dir[1] + dir[2] * m->last_dir[2];
        if (m->count > 0 && dot < -0.3f) m->count++;
        else m->count = 1;
        memcpy(m->last_dir, dir, sizeof dir);
        m->last_peak_us = now_us;
        if (m->count >= SHAKE_SWINGS) { ev = true; m->count = 0; m->last_peak_us = 0; }
    } else if (!m->armed && mag < thr * 0.45f) {
        m->armed = true;
    }
    float gn = sqrtf(m->g[0] * m->g[0] + m->g[1] * m->g[1] + m->g[2] * m->g[2]);
    if (gn < 0.2f) gn = 1;
    out->shake = ev;
    out->energy = m->energy;
    out->gx = m->g[0] / gn; out->gy = m->g[1] / gn; out->gz = m->g[2] / gn;
}
