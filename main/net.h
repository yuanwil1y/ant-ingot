/* Wi-Fi, captive portal, market data, DeepSeek, clock. */
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "model.h"
#include "store.h"

#define AP_SSID "A Share of Anthropic Setup"

void net_start(model_t *m, SemaphoreHandle_t lock, const creds_t *c);
void net_settings(bool on);
bool net_settings_on(void);
void net_on_minted(void);
bool net_take_rehearse(void);
bool net_time_valid(void);
