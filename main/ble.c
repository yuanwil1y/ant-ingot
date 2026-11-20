/* Advertise the device name and nothing else: non-connectable, discoverable. */
#include "ble.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static char s_name[32];

static void advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)s_name;
    f.name_len = strlen(s_name);
    f.name_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;
    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    p.itvl_max = BLE_GAP_ADV_ITVL_MS(600);
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    ble_gap_adv_start(own, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
}

static void on_sync(void) { advertise(); }

static void host_task(void *arg)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_beacon_start(const char *name)
{
    snprintf(s_name, sizeof s_name, "%s", name);
    if (nimble_port_init() != ESP_OK) { ESP_LOGW("ble", "nimble init failed"); return; }
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
}
