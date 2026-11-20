/* Synthetic IMU traces: picking up and inspecting must never flip; shaking must. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../main/core/motion.h"
/* the v0.4 detector, for comparison */
typedef struct { float g[3]; int init; int armed; long long lp; float ld[3]; int c; } old_t;
static int old_step(old_t *o, const float a[3], long long now)
{
    if (!o->init) { memcpy(o->g, a, 12); o->init = 1; o->armed = 1; }
    float hp[3], m2 = 0;
    for (int i = 0; i < 3; i++) { o->g[i] += (a[i] - o->g[i]) * 0.012f; hp[i] = a[i] - o->g[i]; m2 += hp[i] * hp[i]; }
    float mag = sqrtf(m2);
    if (o->armed && mag > 1.1f) {
        o->armed = 0;
        float d[3] = { hp[0] / mag, hp[1] / mag, hp[2] / mag };
        float dot = d[0] * o->ld[0] + d[1] * o->ld[1] + d[2] * o->ld[2];
        if (now - o->lp < 450000 && dot < -0.1f) o->c++; else o->c = 1;
        memcpy(o->ld, d, 12); o->lp = now;
        if (o->c >= 2) { o->c = 0; o->lp = 0; return 1; }
    } else if (!o->armed && mag < 0.55f) o->armed = 1;
    return 0;
}

typedef struct { float R[3][3]; } rot_t;
static void rot_apply(const float R[3][3], const float v[3], float o[3])
{ for (int i = 0; i < 3; i++) o[i] = R[i][0] * v[0] + R[i][1] * v[1] + R[i][2] * v[2]; }
static void rot_from_euler(float rx, float ry, float rz, float R[3][3])
{
    float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
    float X[3][3] = { { 1, 0, 0 }, { 0, cx, -sx }, { 0, sx, cx } };
    float Y[3][3] = { { cy, 0, sy }, { 0, 1, 0 }, { -sy, 0, cy } };
    float Z[3][3] = { { cz, -sz, 0 }, { sz, cz, 0 }, { 0, 0, 1 } };
    float T[3][3] = { 0 };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) T[i][j] += Y[i][k] * X[k][j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { R[i][j] = 0; for (int k = 0; k < 3; k++) R[i][j] += Z[i][k] * T[k][j]; }
}
static float noise(void) { return ((rand() & 1023) / 1023.0f - 0.5f) * 0.02f; }

/* orientation (euler, body→world) and world linear accel as functions of time */
typedef void (*traj_fn)(float t, float e[3], float lw[3]);

static int run(const char *name, traj_fn f, float T, int use_gyro)
{
    motion_t m; motion_init(&m);
    old_t od = { 0 }; int olds = 0;
    const float dt = 0.005f;
    int shakes = 0;
    float e0[3], l0[3];
    f(0, e0, l0);
    for (float t = 0; t < T; t += dt) {
        float e[3], lw[3], e2[3], lw2[3];
        f(t, e, lw);
        f(t + dt, e2, lw2);
        float R[3][3], R2[3][3];
        rot_from_euler(e[0], e[1], e[2], R);
        rot_from_euler(e2[0], e2[1], e2[2], R2);
        /* specific force in world = linear accel + (0,0,1) (up); body = R^T * world */
        float fw[3] = { lw[0], lw[1], lw[2] + 1 }, a[3];
        for (int i = 0; i < 3; i++) a[i] = R[0][i] * fw[0] + R[1][i] * fw[1] + R[2][i] * fw[2] + noise();
        /* body angular rate from R^T * dR/dt (skew part) */
        float W[3][3];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) {
            W[i][j] = 0;
            for (int k = 0; k < 3; k++) W[i][j] += R[k][i] * (R2[k][j] - R[k][j]) / dt;
        }
        float w[3] = { W[2][1] + noise() * 5, W[0][2] + noise() * 5, W[1][0] + noise() * 5 };
        motion_out_t o;
        motion_step(&m, a, use_gyro ? w : NULL, dt, (int64_t)(t * 1e6), 0.9f, &o);
        if (o.shake) shakes++;
        olds += old_step(&od, a, (long long)(t * 1e6));
    }
    printf("%-34s gyro=%d  new=%d  old=%d\n", name, use_gyro, shakes, olds);
    return shakes;
}

#define DEG (3.14159265f / 180)
/* lift from the desk, turn to face you, inspect with lively wrist tilts, put back */
static void pickup(float t, float e[3], float lw[3])
{
    float up = t < 0.5f ? t / 0.5f : 1;
    up = up * up * (3 - 2 * up);
    float wob = t > 0.5f && t < 4.5f ? 1 : 0;
    e[0] = -70 * DEG * up + wob * 25 * DEG * sinf(t * 6.0f);
    e[1] = wob * 40 * DEG * sinf(t * 4.3f);
    e[2] = wob * 15 * DEG * sinf(t * 3.1f);
    lw[0] = 0.25f * sinf(t * 9); lw[1] = 0.2f * sinf(t * 7); lw[2] = t < 0.5f ? 0.5f * sinf(t * 6.28f) : 0;
}
/* turn it over to look at the back, quickly, and turn it back */
static void flipover(float t, float e[3], float lw[3])
{
    float u = fmodf(t, 1.6f);
    float s = u < 0.35f ? u / 0.35f : (u < 0.8f ? 1 : (u < 1.15f ? 1 - (u - 0.8f) / 0.35f : 0));
    s = s * s * (3 - 2 * s);
    e[0] = -60 * DEG; e[1] = 180 * DEG * s; e[2] = 0;
    float k = sinf(3.14159f * fminf(1, u / 0.35f));
    lw[0] = 0.4f * k; lw[1] = 0; lw[2] = 0.3f * k;
}
/* a real shake: hard back-and-forth along x, wrist rotating a bit */
static void shake(float t, float e[3], float lw[3])
{
    e[0] = -60 * DEG + 10 * DEG * sinf(t * 31); e[1] = 8 * DEG * sinf(t * 31); e[2] = 0;
    float on = t > 0.3f && t < 1.1f ? 1 : 0;
    lw[0] = on * 2.2f * sinf(2 * 3.14159f * 5 * t); lw[1] = 0; lw[2] = on * 0.4f * sinf(2 * 3.14159f * 5 * t);
}
/* a gentle shake: what someone does when told "shake it" */
static void soft_shake(float t, float e[3], float lw[3])
{
    e[0] = -60 * DEG; e[1] = 5 * DEG * sinf(t * 25); e[2] = 0;
    float on = t > 0.3f && t < 1.3f ? 1 : 0;
    lw[0] = 0; lw[1] = on * 1.5f * sinf(2 * 3.14159f * 4 * t); lw[2] = on * 1.2f * sinf(2 * 3.14159f * 4 * t + 0.4f);
}

/* snatch it off the desk and snap it round to face you, then a quick look at the back and back again */
static void snatch(float t, float e[3], float lw[3])
{
    float u = t < 0.25f ? t / 0.25f : 1;
    u = u * u * (3 - 2 * u);
    float v = t > 1.0f && t < 1.2f ? (t - 1.0f) / 0.2f : (t >= 1.2f && t < 1.5f ? 1 : (t >= 1.5f && t < 1.7f ? 1 - (t - 1.5f) / 0.2f : 0));
    v = v * v * (3 - 2 * v);
    e[0] = -80 * DEG * u; e[1] = 180 * DEG * v; e[2] = 20 * DEG * u;
    float k = t < 0.25f ? sinf(3.14159f * t / 0.25f) : 0;
    lw[0] = 0.6f * k; lw[1] = 0.5f * k; lw[2] = 1.2f * k;
}
/* fidgeting: turning it over and over in the hand while talking */
static void fidget(float t, float e[3], float lw[3])
{
    e[0] = -60 * DEG + 50 * DEG * sinf(t * 7.5f); e[1] = 70 * DEG * sinf(t * 5.2f + 1); e[2] = 30 * DEG * sinf(t * 3.3f);
    lw[0] = 0.35f * sinf(t * 11); lw[1] = 0.3f * sinf(t * 9 + 2); lw[2] = 0.3f * sinf(t * 13);
}

/* a flick: peek at the back and straight back again, ~0.3 s round trip */
static void peek(float t, float e[3], float lw[3])
{
    float u = fmodf(t, 1.2f);
    float v = u < 0.15f ? u / 0.15f : (u < 0.30f ? 1 - (u - 0.15f) / 0.15f : 0);
    v = v * v * (3 - 2 * v);
    e[0] = -60 * DEG; e[1] = 150 * DEG * v; e[2] = 0;
    lw[0] = 0.3f * sinf(3.14159f * fminf(1, u / 0.3f)); lw[1] = 0; lw[2] = 0;
}

/* a lazy shake: slower and softer, what people do when half-paying attention */
static void lazy_shake(float t, float e[3], float lw[3])
{
    e[0] = -60 * DEG + 6 * DEG * sinf(t * 16); e[1] = 0; e[2] = 0;
    float on = t > 0.3f && t < 1.5f ? 1 : 0;
    lw[0] = on * 1.15f * sinf(2 * 3.14159f * 2.8f * t); lw[1] = 0; lw[2] = on * 0.2f * sinf(2 * 3.14159f * 2.8f * t);
}

int main(void)
{
    int bad = 0;
    for (int g = 1; g >= 0; g--) {
        bad += run("pick up + inspect (5 s)", pickup, 5, g) != 0;
        bad += run("turn over to look at back (x3)", flipover, 4.8f, g) != 0;
        bad += run("snatch up + quick look at back", snatch, 2.5f, g) != 0;
        bad += run("fidgeting in the hand (6 s)", fidget, 6, g) != 0;
        bad += run("quick peek at the back (x4)", peek, 4.8f, g) != 0;
        bad += run("hard shake (0.8 s)", shake, 1.6f, g) == 0;
        bad += run("gentle shake (1 s)", soft_shake, 1.8f, g) == 0;
        bad += run("lazy shake (2.8 Hz, 1.15 g)", lazy_shake, 1.8f, g) == 0;
    }
    printf(bad ? "FAIL (%d)\n" : "all good\n", bad);
    return bad;
}
