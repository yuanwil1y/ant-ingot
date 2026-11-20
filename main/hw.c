#include "hw.h"
#include "gfx.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "led_strip.h"
#include "motion.h"

static const char *TAG = "hw";

/* pins */
#define PIN_MOSI 4
#define PIN_SCLK 5
#define PIN_LCD_CS 7
#define PIN_LCD_DC 6
#define PIN_LCD_RST 14
#define PIN_LCD_BL 15
#define PIN_SD_CS 20
#define PIN_I2C_SCL 8
#define PIN_I2C_SDA 18
#define PIN_BOOT 9
#define PIN_LED 3
#define LCD_X_GAP 35

/* ================================================================ LCD */

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_io;
static uint16_t *s_buf[2];
static SemaphoreHandle_t s_free[2];
static volatile int s_fifo[4], s_head, s_tail;

static bool IRAM_ATTR on_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    if (s_head != s_tail) {
        int i = s_fifo[s_tail & 3];
        s_tail++;
        xSemaphoreGiveFromISR(s_free[i], &woke);
    }
    return woke == pdTRUE;
}

static void tx(uint8_t cmd, const uint8_t *d, int n) { esp_lcd_panel_io_tx_param(s_io, cmd, d, n); }

bool hw_lcd_init(void)
{
    gpio_config_t cs = { .pin_bit_mask = 1ULL << PIN_SD_CS, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&cs);
    gpio_set_level(PIN_SD_CS, 1);           /* keep the SD card off the shared bus */

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = -1, .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = SCR_W * STRIP_H * 2 + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t ioc = {
        .dc_gpio_num = PIN_LCD_DC, .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = 40 * 1000 * 1000, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 4, .on_color_trans_done = on_done,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &ioc, &s_io));
    esp_lcd_panel_dev_config_t pc = {
        .reset_gpio_num = PIN_LCD_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &pc, &s_panel));
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    /* panel tuning from Waveshare's factory firmware */
    tx(0xB2, (uint8_t[]){ 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 5);
    tx(0xB7, (uint8_t[]){ 0x35 }, 1);
    tx(0xBB, (uint8_t[]){ 0x13 }, 1);
    tx(0xC0, (uint8_t[]){ 0x2C }, 1);
    tx(0xC2, (uint8_t[]){ 0x01 }, 1);
    tx(0xC3, (uint8_t[]){ 0x0B }, 1);
    tx(0xC4, (uint8_t[]){ 0x20 }, 1);
    tx(0xC6, (uint8_t[]){ 0x0F }, 1);
    tx(0xD0, (uint8_t[]){ 0xA4, 0xA1 }, 2);
    tx(0xD6, (uint8_t[]){ 0xA1 }, 1);
    tx(0xE0, (uint8_t[]){ 0x00, 0x03, 0x07, 0x08, 0x07, 0x15, 0x2A, 0x44, 0x42, 0x0A, 0x17, 0x18, 0x25, 0x27 }, 14);
    tx(0xE1, (uint8_t[]){ 0x00, 0x03, 0x08, 0x07, 0x07, 0x23, 0x2A, 0x43, 0x42, 0x09, 0x18, 0x17, 0x25, 0x27 }, 14);
    esp_lcd_panel_invert_color(s_panel, true);
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_malloc(SCR_W * STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_free[i] = xSemaphoreCreateBinary();
        xSemaphoreGive(s_free[i]);
        if (!s_buf[i]) return false;
    }
    /* clear to black before the backlight comes on */
    memset(s_buf[0], 0, SCR_W * STRIP_H * 2);
    for (int y = 0; y < SCR_H; y += STRIP_H) {
        hw_lcd_wait(0);
        hw_lcd_push(0, y, STRIP_H);
    }
    hw_lcd_wait(0);
    xSemaphoreGive(s_free[0]);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return true;
}

uint16_t *hw_lcd_strip_buf(int i) { return s_buf[i]; }

void hw_lcd_wait(int i) { xSemaphoreTake(s_free[i], portMAX_DELAY); }

void hw_lcd_push(int i, int y0, int h)
{
    uint16_t *p = s_buf[i];
    int n = SCR_W * h;
    for (int k = 0; k < n; k++) p[k] = (uint16_t)((p[k] >> 8) | (p[k] << 8));
    s_fifo[s_head & 3] = i;
    s_head++;
    esp_lcd_panel_draw_bitmap(s_panel, 0, y0, SCR_W, y0 + h, p);
}

/* ================================================================ backlight */

static bool s_bl_init;

void hw_backlight(int percent)
{
    if (!s_bl_init) {
        ledc_timer_config_t t = {
            .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&t);
        ledc_channel_config_t c = {
            .gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
            .timer_sel = LEDC_TIMER_0, .duty = 0,
        };
        ledc_channel_config(&c);
        s_bl_init = true;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    uint32_t duty = (uint32_t)(powf(percent / 100.0f, 2.2f) * 1023 + 0.5f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* ================================================================ inputs */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch, s_imu;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static struct {
    float gx, gy;
    bool gvalid;
    float energy;
    bool shake, touch, btn_short, btn_long;
    int tx, ty;
} s_in;
static float s_thresh = 1.1f;
static volatile float s_meter_peak;
static volatile int64_t s_meter_t;
static volatile uint32_t s_meter_count;

void hw_shake_meter(float *peak, uint32_t *count)
{
    *peak = s_meter_peak;
    *count = s_meter_count;
}

static esp_err_t reg_write(i2c_master_dev_handle_t d, uint8_t r, uint8_t v)
{
    uint8_t b[2] = { r, v };
    return i2c_master_transmit(d, b, 2, 50);
}

static esp_err_t reg_read(i2c_master_dev_handle_t d, uint8_t r, uint8_t *out, int n)
{
    return i2c_master_transmit_receive(d, &r, 1, out, n, 50);
}

static bool imu_init(void)
{
    uint8_t addrs[2] = { 0x6B, 0x6A };
    for (int i = 0; i < 2; i++) {
        if (i2c_master_probe(s_bus, addrs[i], 50) != ESP_OK) continue;
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addrs[i], .scl_speed_hz = 400000 };
        i2c_master_bus_add_device(s_bus, &dc, &s_imu);
        uint8_t who = 0;
        reg_read(s_imu, 0x00, &who, 1);
        ESP_LOGI(TAG, "QMI8658 at 0x%02x, whoami 0x%02x", addrs[i], who);
        reg_write(s_imu, 0x60, 0xB0);         /* soft reset */
        vTaskDelay(pdMS_TO_TICKS(20));
        reg_write(s_imu, 0x02, 0x40);         /* CTRL1: auto-increment, little endian */
        reg_write(s_imu, 0x03, 0x25);         /* CTRL2: accel +-8g, ~250 Hz */
        reg_write(s_imu, 0x06, 0x00);         /* CTRL5: no LPF tweaks */
        reg_write(s_imu, 0x08, 0x01);         /* CTRL7: accel only */
        return true;
    }
    ESP_LOGW(TAG, "IMU not found");
    return false;
}

static void input_task(void *arg)
{
    motion_t mo;
    motion_init(&mo);
    int64_t btn_down = 0;
    bool btn_prev = false, long_sent = false;
    int tick = 0;
    const float dt = 0.005f;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        int64_t now = esp_timer_get_time();
        /* ---- accelerometer + gyro */
        if (s_imu) {
            uint8_t b[6];
            if (reg_read(s_imu, 0x35, b, 6) == ESP_OK) {
                float a[3];
                for (int i = 0; i < 3; i++) a[i] = (int16_t)(b[2 * i] | (b[2 * i + 1] << 8)) / 4096.0f;
                motion_out_t o;
                motion_step(&mo, a, NULL, dt, now, s_thresh, &o);
                /* shake meter: strongest swing in the last 3 s, and how many shakes were seen */
                if (o.lin > s_meter_peak || now - s_meter_t > 3000000) { s_meter_peak = o.lin; s_meter_t = now; }
                if (o.shake) { s_meter_count++; ESP_LOGI(TAG, "shake (peak %.2f g)", s_meter_peak); }
                taskENTER_CRITICAL(&s_mux);
                s_in.energy = o.energy;
                s_in.gx = o.gx;
                s_in.gy = o.gy;
                s_in.gvalid = true;
                if (o.shake) s_in.shake = true;
                taskEXIT_CRITICAL(&s_mux);
            }
        }
        /* ---- touch, 50 Hz */
        if (s_touch && (tick & 3) == 0) {
            uint8_t t[7];
            if (reg_read(s_touch, 0x00, t, 7) == ESP_OK) {
                bool down = t[2] != 0;
                int x = ((t[3] & 0x0F) << 8) | t[4], y = ((t[5] & 0x0F) << 8) | t[6];
                taskENTER_CRITICAL(&s_mux);
                s_in.touch = down;
                if (down) { s_in.tx = x; s_in.ty = y; }
                taskEXIT_CRITICAL(&s_mux);
            }
        }
        /* ---- BOOT button */
        bool b = gpio_get_level(PIN_BOOT) == 0;
        if (b && !btn_prev) { btn_down = now; long_sent = false; }
        if (b && !long_sent && now - btn_down > 2000000) {
            long_sent = true;
            taskENTER_CRITICAL(&s_mux); s_in.btn_long = true; taskEXIT_CRITICAL(&s_mux);
        }
        if (!b && btn_prev && !long_sent && now - btn_down > 30000) {
            taskENTER_CRITICAL(&s_mux); s_in.btn_short = true; taskEXIT_CRITICAL(&s_mux);
        }
        btn_prev = b;
        tick++;
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(5));
    }
}

bool hw_inputs_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_I2C_SDA, .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_bus) != ESP_OK) return false;
    if (i2c_master_probe(s_bus, 0x15, 50) == ESP_OK) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x15, .scl_speed_hz = 400000 };
        i2c_master_bus_add_device(s_bus, &dc, &s_touch);
        reg_write(s_touch, 0x00, 0x00);       /* normal mode */
    } else {
        ESP_LOGW(TAG, "touch not found");
    }
    imu_init();
    gpio_config_t bt = { .pin_bit_mask = 1ULL << PIN_BOOT, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&bt);
    xTaskCreate(input_task, "inputs", 4096, NULL, 6, NULL);
    return true;
}

void hw_inputs_take(hw_input_t *o, const tune_t *t)
{
    s_thresh = t->shake_g;
    taskENTER_CRITICAL(&s_mux);
    o->energy = s_in.energy;
    o->gx = s_in.gx;
    o->gy = s_in.gy;
    o->gvalid = s_in.gvalid;
    o->shake = s_in.shake;
    o->touch = s_in.touch;
    o->tx = s_in.tx;
    o->ty = s_in.ty;
    o->btn_short = s_in.btn_short;
    o->btn_long = s_in.btn_long;
    s_in.shake = s_in.btn_short = s_in.btn_long = false;
    taskEXIT_CRITICAL(&s_mux);
}

/* ================================================================ RGB LED */

static led_strip_handle_t s_led;
static uint8_t s_lr = 1, s_lg = 1, s_lb = 1;

void hw_led_init(void)
{
    led_strip_config_t sc = {
        .strip_gpio_num = PIN_LED, .max_leds = 2, .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    led_strip_rmt_config_t rc = { .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&sc, &rc, &s_led) != ESP_OK) s_led = NULL;
    hw_led_set(0, 0, 0);
}

void hw_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led || (r == s_lr && g == s_lg && b == s_lb)) return;
    s_lr = r; s_lg = g; s_lb = b;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_set_pixel(s_led, 1, r, g, b);
    led_strip_refresh(s_led);
}
