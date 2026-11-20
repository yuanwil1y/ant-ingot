/* Persistent storage (NVS). */
#pragma once
#include "model.h"

typedef struct {
    char ssid[33];
    char pass[65];
    char ds_key[96];
} creds_t;

void store_init(void);
void store_load(model_t *m, creds_t *c);
void store_save_mint(const mint_rec_t *r);
void store_save_pending(const purchase_t *p);
void store_save_creds(const creds_t *c);
void store_save_tune(const tune_t *t);
void store_save_line(const char *line, int day);
void store_save_int(const char *key, int v);
