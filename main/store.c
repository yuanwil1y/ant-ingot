#include "store.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"

#define NS "ingot"
#define TUNE_VER 2

void store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
}

static void get_str(nvs_handle_t h, const char *k, char *out, size_t cap)
{
    size_t n = cap;
    if (nvs_get_str(h, k, out, &n) != ESP_OK) out[0] = 0;
}

void store_load(model_t *m, creds_t *c)
{
    memset(c, 0, sizeof *c);
    m->tune = tune_defaults();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof m->mint;
    mint_rec_t r;
    if (nvs_get_blob(h, "mint", &r, &n) == ESP_OK && n == sizeof r) m->mint = r;
    purchase_t p;
    n = sizeof p;
    if (nvs_get_blob(h, "pend", &p, &n) == ESP_OK && n == sizeof p) m->setup.pending = p;
    struct { int ver; tune_t t; } tb;
    n = sizeof tb;
    if (nvs_get_blob(h, "tune", &tb, &n) == ESP_OK && n == sizeof tb && tb.ver == TUNE_VER) {
        m->tune = tb.t;
        m->tune.preview_days = -1;
    }
    get_str(h, "ssid", c->ssid, sizeof c->ssid);
    get_str(h, "pass", c->pass, sizeof c->pass);
    get_str(h, "dskey", c->ds_key, sizeof c->ds_key);
    get_str(h, "line", m->line, sizeof m->line);
    int32_t v;
    if (nvs_get_i32(h, "line_day", &v) == ESP_OK) m->line_day = v;
    if (nvs_get_i32(h, "seen_day", &v) == ESP_OK) m->line_seen_day = v;
    if (nvs_get_i32(h, "patina_day", &v) == ESP_OK) m->patina_day = v;
    nvs_close(h);
}

static void put_blob(const char *k, const void *d, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, k, d, n);
    nvs_commit(h);
    nvs_close(h);
}

void store_save_mint(const mint_rec_t *r) { put_blob("mint", r, sizeof *r); }
void store_save_pending(const purchase_t *p) { put_blob("pend", p, sizeof *p); }

void store_save_tune(const tune_t *t)
{
    struct { int ver; tune_t t; } tb = { TUNE_VER, *t };
    tb.t.preview_days = -1;
    put_blob("tune", &tb, sizeof tb);
}

void store_save_creds(const creds_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "dskey", c->ds_key);
    nvs_commit(h);
    nvs_close(h);
}

void store_save_line(const char *line, int day)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "line", line);
    nvs_set_i32(h, "line_day", day);
    nvs_commit(h);
    nvs_close(h);
}

void store_save_int(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
}
