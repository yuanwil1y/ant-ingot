/* Board support for the Waveshare ESP32-C6-Touch-LCD-1.9 (pins from the official demo). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "model.h"

#define STRIP_H 40

bool hw_lcd_init(void);
uint16_t *hw_lcd_strip_buf(int i);              /* two DMA buffers, SCR_W * STRIP_H */
void hw_lcd_push(int i, int y0, int h);         /* byte-swaps and sends; returns immediately */
void hw_lcd_wait(int i);                        /* until buffer i is free again */

void hw_backlight(int percent);                 /* 0..100, gamma-corrected */

bool hw_inputs_init(void);
/* thread-safe snapshot of the sensors; consumes the edge events */
typedef struct {
    float energy;
    float gx, gy;           /* gravity in the IMU frame, g, ~70 ms smoothing */
    bool gvalid;
    bool shake;
    bool touch;
    int tx, ty;
    bool btn_short, btn_long;
} hw_input_t;
void hw_inputs_take(hw_input_t *out, const tune_t *tune);

void hw_led_init(void);
void hw_led_set(uint8_t r, uint8_t g, uint8_t b);
