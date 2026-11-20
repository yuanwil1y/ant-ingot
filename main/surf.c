/* Generated ingot surfaces live in their own flash partition and are read through mmap,
 * so they cost no RAM and survive reboots (no regeneration unless the engraving changes). */
#include <stdlib.h>
#include <string.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "nvs.h"
#include "scene.h"

#define SLOT_SIZE 0x10000
static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_h;

static bool ready(void)
{
    if (s_map) return true;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "surf");
    if (!s_part) return false;
    const void *p;
    if (esp_partition_mmap(s_part, 0, 2 * SLOT_SIZE, ESP_PARTITION_MMAP_DATA, &p, &s_h) != ESP_OK) return false;
    s_map = p;
    return true;
}

static uint32_t stored_key(int slot)
{
    nvs_handle_t h;
    int32_t v = 0;
    char k[8] = "surf0";
    k[4] = (char)('0' + slot);
    if (nvs_open("ingot", NVS_READONLY, &h) == ESP_OK) { nvs_get_i32(h, k, &v); nvs_close(h); }
    return (uint32_t)v;
}

const uint8_t *surf_lookup(int slot, uint32_t key)
{
    if (!ready() || !key || stored_key(slot) != key) return NULL;
    return s_map + slot * SLOT_SIZE;
}

const uint8_t *surf_commit(int slot, uint32_t key, uint8_t *ram)
{
    if (!ready()) return ram;               /* no partition: keep it in RAM */
    if (esp_partition_erase_range(s_part, slot * SLOT_SIZE, SLOT_SIZE) != ESP_OK ||
        esp_partition_write(s_part, slot * SLOT_SIZE, ram, SCR_W * SCR_H) != ESP_OK) {
        ESP_LOGW("surf", "flash write failed, keeping slot %d in RAM", slot);
        return ram;
    }
    free(ram);
    nvs_handle_t h;
    char k[8] = "surf0";
    k[4] = (char)('0' + slot);
    if (nvs_open("ingot", NVS_READWRITE, &h) == ESP_OK) { nvs_set_i32(h, k, (int32_t)key); nvs_commit(h); nvs_close(h); }
    return s_map + slot * SLOT_SIZE;
}
