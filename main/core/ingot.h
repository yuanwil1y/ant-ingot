/* Ingot surface generation and palettes.
 *
 * Every face is an 8-bit index image: 0 = background, bit 7 = recessed (engraved,
 * collects patina), bits 0..6 = lighting shade 1..127. Colour comes from a 256-entry
 * palette, so cooling, ageing and polish are palette changes, not per-pixel work.
 */
#pragma once
#include <stdint.h>
#include "gfx.h"
#include "model.h"

#define IG_RECESS 0x80
#define IG_SHADE(i) ((i) & 0x7F)

#define BAR_X0 3
#define BAR_Y0 3
#define BAR_X1 167
#define BAR_Y1 317
#define BAR_R 16
#define BAR_BEVEL 8

typedef struct { int y0, y1; float t; float weight; } stamp_band_t;

typedef struct {
    float rcx, rcy, rmax;       /* rosette centre / outer radius */
    int nbands;
    stamp_band_t bands[6];      /* engraved text bands in the order they are stamped */
} ingot_layout_t;

/* Blank cast bar. back=1 → brushed reverse side. */
void ingot_gen_base(uint8_t *buf, uint32_t seed, int back);

/* Engrave the front (rosette + inscriptions) on top of base. buf receives the result. */
void ingot_gen_front(uint8_t *buf, const uint8_t *base, const mint_rec_t *m, ingot_layout_t *lay);

void ingot_front_layout(ingot_layout_t *lay);

/* Engrave one string straight into a finished face (used for the daily line, char by char). */
void ingot_engrave_text(uint8_t *buf, const font_t *f, const char *s, float x, int baseline,
                        float track, int only_char);

void ingot_engrave_rule(uint8_t *buf, int x0, int x1, int y);

typedef struct {
    float finish;   /* 0 = raw casting, 1 = polished */
    float heat;     /* 0..1 incandescence */
    float patina;   /* 0..1 age */
    float gain;     /* brightness multiplier */
    float cool;     /* 0..1 tint toward pale silver light (dawn sweep), usually 0 */
} metal_params_t;

void lut_metal(uint16_t lut[256], const metal_params_t *p);
void lut_ledger(uint16_t lut[256]);
void blackbody(float t, float *r, float *g, float *b);
float patina_for_days(int days);

const char *roman(int n, char *out, int cap);
