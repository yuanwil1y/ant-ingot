#include "motion.h"
#include <math.h>
#include <string.h>

#define JOLTS 3                 /* separate jolts that make a shake */
#define WINDOW_US 600000        /* ...within this long */
#define JOLT_MIN_SAMPLES 3      /* a jolt lasts at least this many samples (15 ms) */

void motion_init(motion_t *m)
{
    memset(m, 0, sizeof *m);
    m->g[2] = 1;
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
    /* thr is the user's "shake threshold": a jolt is |a| leaving 1 g by half of it */
    float on = thr * 0.42f, off = on * 0.55f;
    float dev = fabsf(an - 1);
    if (!m->in_jolt) {
        if (dev > on) { m->in_jolt = true; m->jolt_len = 1; }
    } else if (dev > off) {
        m->jolt_len++;
    } else {
        m->in_jolt = false;
        if (m->jolt_len >= JOLT_MIN_SAMPLES) {
            if (m->nj == 8) { memmove(m->jolts, m->jolts + 1, 7 * sizeof m->jolts[0]); m->nj = 7; }
            m->jolts[m->nj++] = now_us;
            int recent = 0;
            for (int i = 0; i < m->nj; i++) if (now_us - m->jolts[i] <= WINDOW_US) recent++;
            if (recent >= JOLTS) { ev = true; m->nj = 0; }
        }
    }
    float gn = sqrtf(m->g[0] * m->g[0] + m->g[1] * m->g[1] + m->g[2] * m->g[2]);
    if (gn < 0.2f) gn = 1;
    out->shake = ev;
    out->energy = m->energy;
    out->lin = fabsf(an - 1);      /* what the detector looks at, for the shake meter */
    out->gx = m->g[0] / gn; out->gy = m->g[1] / gn; out->gz = m->g[2] / gn;
}
