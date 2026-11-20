/* A Share of Anthropic — a commemorative digital ingot. */
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "hw.h"
#include "net.h"
#include "store.h"
#include "ble.h"
#include "scene.h"

static const char *TAG = "ingot";
static model_t M;
static SemaphoreHandle_t L;

static void render_frame(void)
{
    static int which;
    for (int y = 0; y < SCR_H; y += STRIP_H) {
        int h = SCR_H - y < STRIP_H ? SCR_H - y : STRIP_H;
        hw_lcd_wait(which);
        strip_t st = { hw_lcd_strip_buf(which), y, h };
        scene_render(&st);
        hw_lcd_push(which, y, h);
        which ^= 1;
    }
}

void app_main(void)
{
    store_init();
    creds_t creds;
    store_load(&M, &creds);
    if (!M.mint.minted) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        uint32_t s = 2166136261u;
        for (int i = 0; i < 6; i++) s = (s ^ mac[i]) * 16777619u;
        M.mint.serial = hash32(s);
    }
    L = xSemaphoreCreateMutex();

    hw_led_init();
    hw_backlight(0);
    if (!hw_lcd_init()) ESP_LOGE(TAG, "lcd init failed");
    hw_inputs_init();

    int64_t t_gen = esp_timer_get_time();
    if (!scene_init(&M)) ESP_LOGE(TAG, "scene init failed (out of memory?)");
    ESP_LOGI(TAG, "surfaces generated in %lld ms, free heap %u", (esp_timer_get_time() - t_gen) / 1000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    net_start(&M, L, &creds);
    ble_beacon_start("A Share of Anthropic");
    ESP_LOGI(TAG, "running, free heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    int64_t last = esp_timer_get_time(), last_act = last, fps_t = last;
    float bl = 0;
    int frames = 0;
    bool first = true;
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        float dt = (t0 - last) / 1e6f;
        last = t0;

        hw_input_t hi;
        hw_inputs_take(&hi, &M.tune);
        if (hi.btn_long && M.mint.minted) net_settings(!net_settings_on());
        input_t in = {
            .shake_energy = hi.energy, .shake = hi.shake, .touch = hi.touch,
            .tx = hi.tx, .ty = hi.ty, .btn_short = hi.btn_short,
        };

        xSemaphoreTake(L, portMAX_DELAY);
        if (net_take_rehearse()) scene_rehearse();
        if (net_time_valid()) M.now = time(NULL);
        scene_tick(dt, &in);
        uint32_t ev = scene_take_events();
        bool anim = scene_animating() || first;
        if (anim) render_frame();
        uint8_t r, g, b;
        scene_led(&r, &g, &b);
        mint_rec_t mint = M.mint;
        int seen = M.line_seen_day, patina = M.patina_day, bright = M.tune.brightness;
        xSemaphoreGive(L);
        hw_led_set(r, g, b);

        if (ev & EV_MINTED) {
            store_save_mint(&mint);
            store_save_int("patina_day", patina);
            net_on_minted();
            ESP_LOGI(TAG, "minted");
        }
        if (ev & EV_LINE_SEEN) store_save_int("seen_day", seen);
        if (ev & EV_PATINA_SEEN) store_save_int("patina_day", patina);
        if (ev & EV_ACTIVITY) last_act = t0;

        /* backlight: fade in after the first frame, dim when idle */
        float idle = (t0 - last_act) / 1e6f;
        float target = bright;
        if (net_settings_on()) idle = 0;
        if (idle > 600) target = bright * 0.12f;
        else if (idle > 180) target = bright * 0.4f;
        float k = first ? 0 : (target > bl ? 0.12f : 0.04f);
        bl += (target - bl) * k;
        hw_backlight((int)(bl + 0.5f));
        first = false;

        frames += anim;
        if (t0 - fps_t > 5000000) {
            ESP_LOGD(TAG, "%.1f fps, heap %u", frames / ((t0 - fps_t) / 1e6f), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            frames = 0;
            fps_t = t0;
        }
        int64_t spent = (esp_timer_get_time() - t0) / 1000;
        int wait = 33 - (int)spent;
        vTaskDelay(pdMS_TO_TICKS(wait > 1 ? wait : 1));
    }
}
