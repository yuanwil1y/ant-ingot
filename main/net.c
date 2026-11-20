#include "net.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "dns_server.h"
#include "ingot.h"
#include "market.h"

static const char *TAG = "net";

extern const char portal_html_start[] asm("_binary_portal_html_start");
extern const char portal_html_end[] asm("_binary_portal_html_end");

static model_t *M;
static SemaphoreHandle_t L;
static creds_t C;
static esp_netif_t *s_ap_if, *s_sta_if;
static EventGroupHandle_t s_ev;
#define EV_CONNECTED BIT0
#define EV_FAILED BIT1
static int s_disc_reason;
static bool s_auto_reconnect;
static bool s_ap_on, s_settings, s_rehearse, s_sntp_started;
static httpd_handle_t s_http;
static dns_server_handle_t s_dns;
static volatile bool s_checking;
static int64_t s_settings_since;
static char s_sta_ip[20];

#define LOCK() xSemaphoreTake(L, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(L)

/* ================================================================ Wi-Fi */

static esp_timer_handle_t s_retry;
static int s_backoff = 1;
static void retry_cb(void *arg) { if (s_auto_reconnect) esp_wifi_connect(); }

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_disc_reason = d->reason;
        s_sta_ip[0] = 0;
        xEventGroupClearBits(s_ev, EV_CONNECTED);
        xEventGroupSetBits(s_ev, EV_FAILED);
        if (s_auto_reconnect) {
            if (!s_retry) {
                esp_timer_create_args_t a = { .callback = retry_cb, .name = "wifiretry" };
                esp_timer_create(&a, &s_retry);
            }
            esp_timer_stop(s_retry);
            esp_timer_start_once(s_retry, (uint64_t)s_backoff * 1000000);
            s_backoff = s_backoff < 30 ? s_backoff * 2 : 30;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_sta_ip, sizeof s_sta_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_backoff = 1;
        xEventGroupClearBits(s_ev, EV_FAILED);
        xEventGroupSetBits(s_ev, EV_CONNECTED);
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org"));
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    }
}

static void apply_mode(void)
{
    esp_wifi_set_mode(s_ap_on ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (s_ap_on) {
        wifi_config_t ap = { 0 };
        strcpy((char *)ap.ap.ssid, AP_SSID);
        ap.ap.ssid_len = strlen(AP_SSID);
        ap.ap.channel = 6;
        ap.ap.max_connection = 3;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
}

static void sta_set(const creds_t *c)
{
    wifi_config_t sc = { 0 };
    memcpy(sc.sta.ssid, c->ssid, strnlen(c->ssid, sizeof sc.sta.ssid));
    memcpy(sc.sta.password, c->pass, strnlen(c->pass, sizeof sc.sta.password));
    sc.sta.threshold.authmode = c->pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &sc);
}

/* Connect with the given credentials; returns 0 or a disconnect reason. */
static int sta_connect_blocking(const creds_t *c, int timeout_ms)
{
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    xEventGroupClearBits(s_ev, EV_CONNECTED | EV_FAILED);
    sta_set(c);
    s_disc_reason = 0;
    int tries = 0;
    int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < until) {
        xEventGroupClearBits(s_ev, EV_FAILED);
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_CONNECTED | EV_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
        if (b & EV_CONNECTED) { s_auto_reconnect = true; return 0; }
        if (++tries >= 3) break;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_auto_reconnect = true;
    return s_disc_reason ? s_disc_reason : -1;
}

bool net_time_valid(void) { return time(NULL) > 1700000000; }

/* ================================================================ HTTP client helpers */

typedef struct { char *buf; int len, cap; } body_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    body_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        int n = e->data_len;
        if (b->len + n >= b->cap) n = b->cap - 1 - b->len;
        if (n > 0) { memcpy(b->buf + b->len, e->data, n); b->len += n; b->buf[b->len] = 0; }
    }
    return ESP_OK;
}

static bool fetch_quote(void)
{
    char buf[1024] = { 0 };
    body_t b = { buf, 0, sizeof buf };
    esp_http_client_config_t cfg = { .url = "http://qt.gtimg.cn/q=usANT", .event_handler = on_http, .user_data = &b, .timeout_ms = 8000 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK || st != 200) { ESP_LOGW(TAG, "quote http %d %s", st, esp_err_to_name(e)); return false; }
    char *q = strchr(buf, '"');
    if (!q) return false;
    q++;
    char *f[80] = { 0 };
    int n = 0;
    f[n++] = q;
    for (char *p = q; *p && *p != '"' && n < 80; p++)
        if (*p == '~') { *p = 0; f[n++] = p + 1; }
    char *endq = strchr(f[n - 1], '"');
    if (endq) *endq = 0;
    if (n < 35) return false;
    double price = atof(f[3]);
    if (price <= 0) return false;
    LOCK();
    M->quote.valid = true;
    M->quote.price = price;
    M->quote.prev_close = atof(f[4]);
    M->quote.chg_pct = atof(f[32]);
    M->quote.open = atof(f[5]);
    M->quote.volume = atof(f[6]);
    M->quote.high = atof(f[33]);
    M->quote.low = atof(f[34]);
    if (n > 62 && atof(f[62]) > 1e6) M->quote.total_shares = atof(f[62]);
    snprintf(M->quote.when, sizeof M->quote.when, "%s", f[30]);
    M->quote.fetched_ts = time(NULL);
    UNLOCK();
    return true;
}

static int64_t days_from_civil_net(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* ---------------------------------------------------------------- streaming market history
 * The minute and k-line payloads are tens of KB; instead of buffering them we read the
 * quoted strings as they stream past and keep only the numbers we need. */

typedef struct tok_s {
    char buf[48];
    int len;
    bool in;
    void (*on)(struct tok_s *t, const char *s, int n);
    char prev[16];
    int state;
    int date;
    int n;
    int32_t day[SERIES_MAX];
    float close[SERIES_MAX];
    int16_t min[INTRA_MAX];
    float px[INTRA_MAX];
} tok_t;

static esp_err_t on_tok(esp_http_client_event_t *e)
{
    tok_t *t = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !t) return ESP_OK;
    const char *d = e->data;
    for (int i = 0; i < e->data_len; i++) {
        char c = d[i];
        if (!t->in) { if (c == '"') { t->in = true; t->len = 0; } continue; }
        if (c == '"') {
            t->buf[t->len] = 0;
            t->on(t, t->buf, t->len);
            snprintf(t->prev, sizeof t->prev, "%.15s", t->buf);
            t->in = false;
        } else if (t->len < (int)sizeof t->buf - 1) {
            t->buf[t->len++] = c;
        }
    }
    return ESP_OK;
}

/* "1307 84.78 280653244" → minute + price; "date" key → session date */
static void on_minute(tok_t *t, const char *s, int n)
{
    if (!strcmp(t->prev, "date") && n == 8) { t->date = atoi(s); return; }
    if (n < 8 || s[4] != ' ' || !isdigit((unsigned char)s[0])) return;
    int hhmm = atoi(s);
    float px = atof(s + 5);
    int m = (hhmm / 100) * 60 + hhmm % 100 - 570;
    if (px > 0 && m >= 0 && m <= 390 && t->n < INTRA_MAX) { t->min[t->n] = (int16_t)m; t->px[t->n] = px; t->n++; }
}

/* ["2026-11-19","111.50","97.17",...] → day + close */
static void on_kline(tok_t *t, const char *s, int n)
{
    if (n == 10 && s[4] == '-' && s[7] == '-' && isdigit((unsigned char)s[0])) {
        int Y = atoi(s), Mo = atoi(s + 5), D = atoi(s + 8);
        if (t->n < SERIES_MAX) t->day[t->n] = (int32_t)days_from_civil_net(Y, Mo, D);
        t->state = 1;
        return;
    }
    if (t->state == 1) { t->state = 2; return; }                 /* open */
    if (t->state == 2) {                                        /* close */
        float c = atof(s);
        if (c > 0 && t->n < SERIES_MAX) t->close[t->n++] = c;
        t->state = 0;
    }
}

static bool fetch_stream(const char *url, tok_t *t)
{
    esp_http_client_config_t cfg = {
        .url = url, .event_handler = on_tok, .user_data = t, .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK || st != 200) { ESP_LOGW(TAG, "%s: %d %s", url, st, esp_err_to_name(e)); return false; }
    return true;
}

static bool fetch_intraday(void)
{
    tok_t *t = calloc(1, sizeof *t);
    if (!t) return false;
    t->on = on_minute;
    bool ok = fetch_stream("https://web.ifzq.gtimg.cn/appstock/app/UsMinute/query?code=usANT.OQ", t) && t->n > 0;
    if (ok) {
        LOCK();
        M->intra.n = t->n;
        M->intra.date = t->date;
        memcpy(M->intra.min, t->min, t->n * sizeof t->min[0]);
        memcpy(M->intra.px, t->px, t->n * sizeof t->px[0]);
        M->intra.rev++;
        UNLOCK();
    }
    free(t);
    return ok;
}

static bool fetch_history(void)
{
    tok_t *t = calloc(1, sizeof *t);
    if (!t) return false;
    t->on = on_kline;
    char url[160];
    snprintf(url, sizeof url, "https://web.ifzq.gtimg.cn/appstock/app/usfqkline/get?param=usANT.OQ,day,,,%d,qfq", SERIES_MAX);
    bool ok = fetch_stream(url, t) && t->n > 0;
    if (ok && t->n >= SERIES_MAX) {             /* more than ~14 months: weekly closes cover it */
        memset(t, 0, sizeof *t);
        t->on = on_kline;
        snprintf(url, sizeof url, "https://web.ifzq.gtimg.cn/appstock/app/usfqkline/get?param=usANT.OQ,week,,,%d,qfq", SERIES_MAX);
        ok = fetch_stream(url, t) && t->n > 0;
    }
    if (ok) {
        LOCK();
        M->daily.n = t->n;
        memcpy(M->daily.day, t->day, t->n * sizeof t->day[0]);
        memcpy(M->daily.close, t->close, t->n * sizeof t->close[0]);
        M->daily.rev++;
        UNLOCK();
    }
    free(t);
    return ok;
}

/* ---------------------------------------------------------------- exchange calendar
 * Nasdaq's own market-info: today's pre/regular/after-hours times (early closes
 * included), whether it is a trading day, and the next trading day. */

static const char *jstr_net(cJSON *j, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static int month_of(const char *s)
{
    static const char *M3[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < 12; i++) if (!strncmp(s, M3[i], 3)) return i + 1;
    return 0;
}

/* "Nov 20, 2026 09:30 AM ET" (time optional) → UTC seconds, 0 on failure */
static int64_t parse_et(const char *s, bool date_only_930)
{
    if (!s) return 0;
    int mo = month_of(s), d = 0, y = 0, hh = 9, mi = 30;
    char ap[3] = "AM";
    if (!mo || sscanf(s + 4, "%d, %d", &d, &y) != 2) return 0;
    if (!date_only_930) {
        const char *t = strchr(s + 4, ',');
        if (!t || sscanf(t + 1, " %*d %d:%d %2s", &hh, &mi, ap) != 3) return 0;
        if (hh == 12) hh = 0;
        if (ap[0] == 'P') hh += 12;
    }
    int64_t local = days_from_civil_net(y, mo, d) * 86400 + hh * 3600 + mi * 60;
    return local - us_et_offset(local + 5 * 3600);
}

static bool fetch_session(void)
{
    char *buf = calloc(1, 3072);
    if (!buf) return false;
    body_t b = { buf, 0, 3072 };
    esp_http_client_config_t cfg = {
        .url = "https://api.nasdaq.com/api/market-info", .event_handler = on_http, .user_data = &b,
        .timeout_ms = 12000, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124 Safari/537.36");
    esp_http_client_set_header(h, "Accept", "application/json");
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    bool ok = false;
    if (e == ESP_OK && st == 200) {
        cJSON *r = cJSON_Parse(buf);
        cJSON *d = r ? cJSON_GetObjectItem(r, "data") : NULL;
        if (d) {
            session_t ss = { 0 };
            ss.business = cJSON_IsTrue(cJSON_GetObjectItem(d, "isBusinessDay"));
            ss.pre_open = parse_et(jstr_net(d, "preMarketOpeningTime"), false);
            ss.open = parse_et(jstr_net(d, "marketOpeningTime"), false);
            ss.close = parse_et(jstr_net(d, "marketClosingTime"), false);
            ss.post_close = parse_et(jstr_net(d, "afterHoursMarketClosingTime"), false);
            ss.next_open = parse_et(jstr_net(d, "nextTradeDate"), true);
            int64_t now = time(NULL);
            ss.day = (int32_t)((now + us_et_offset(now)) / 86400);
            ss.fetched = now;
            ss.valid = ss.open > 0 || !ss.business;
            if (ss.valid) {
                if (!ss.pre_open) ss.pre_open = ss.open - 5 * 3600 - 30 * 60;
                if (!ss.post_close) ss.post_close = ss.close + 4 * 3600;
                LOCK();
                M->session = ss;
                UNLOCK();
                ok = true;
            }
        }
        cJSON_Delete(r);
    } else {
        ESP_LOGW(TAG, "market-info %d %s", st, esp_err_to_name(e));
    }
    free(buf);
    return ok;
}

/* Spot gold (London, USD/oz) from Sina: first field of hf_XAU */
static bool fetch_gold(void)
{
    char buf[256] = { 0 };
    body_t b = { buf, 0, sizeof buf };
    esp_http_client_config_t cfg = { .url = "http://hq.sinajs.cn/list=hf_XAU", .event_handler = on_http, .user_data = &b, .timeout_ms = 8000 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Referer", "https://finance.sina.com.cn");
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK || st != 200) return false;
    char *q = strchr(buf, '"');
    double g = q ? atof(q + 1) : 0;
    if (g < 100 || g > 100000) return false;
    LOCK();
    M->quote.gold = g;
    UNLOCK();
    return true;
}

/* Sina carries the pre-market / after-hours price (fields 21, 22) */
static bool fetch_extended(void)
{
    char buf[700] = { 0 };
    body_t b = { buf, 0, sizeof buf };
    esp_http_client_config_t cfg = { .url = "http://hq.sinajs.cn/list=gb_ant", .event_handler = on_http, .user_data = &b, .timeout_ms = 8000 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Referer", "https://finance.sina.com.cn");
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (e != ESP_OK || st != 200) return false;
    char *q = strchr(buf, '"');
    if (!q) return false;
    char *f[40] = { 0 };
    int n = 0;
    f[n++] = ++q;
    for (char *p = q; *p && *p != '"' && n < 40; p++)
        if (*p == ',') { *p = 0; f[n++] = p + 1; }
    if (n < 23) return false;
    LOCK();
    M->quote.ext_price = atof(f[21]);
    M->quote.ext_pct = atof(f[22]);
    UNLOCK();
    return true;
}

static void sanitize_line(const char *in, char *out, int cap)
{
    int o = 0;
    bool sp = false;
    for (const char *p = in; *p && o < cap - 1; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '\n' || c == '\t' || c == ' ') { if (o && !sp) { out[o++] = ' '; sp = true; } continue; }
        if (c < 32 || c > 126 || c == '"' || c == '*' || c == '#') continue;
        if (c == '!') c = '.';
        out[o++] = (char)c;
        sp = false;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
    if (o > 110) {                          /* cut at a word boundary */
        int k = 110;
        while (k > 60 && out[k] != ' ') k--;
        out[k] = 0;
        if (out[k - 1] != '.') { out[k] = '.'; out[k + 1] = 0; }
    }
}

/* returns HTTP status (200 ok), or <0 on transport error */
static int deepseek_line(const char *key, int day, char *out, int cap)
{
    LOCK();
    purchase_t p = M->mint.minted ? M->mint.p : M->setup.pending;
    quote_t q = M->quote;
    UNLOCK();
    char date[24];
    time_t bt = (time_t)(p.buy_ts + LOCAL_UTC_OFFSET);
    struct tm tm;
    gmtime_r(&bt, &tm);
    strftime(date, sizeof date, "%d %b %Y", &tm);
    char user[320];
    if (q.valid)
        snprintf(user, sizeof user, "Day %d of being held. Bought on %s at $%.2f. Today's price: $%.2f (%+.2f%% today, %+.1f%% since purchase). Write today's line.",
                 day, date, p.price, q.price, q.chg_pct, (q.price / p.price - 1) * 100);
    else
        snprintf(user, sizeof user, "Day %d of being held. Bought on %s at $%.2f. Today's price is unknown. Write today's line.", day, date, p.price);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "deepseek-chat");
    cJSON_AddNumberToObject(root, "max_tokens", 80);
    cJSON_AddNumberToObject(root, "temperature", 1.1);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys = cJSON_CreateObject();
    cJSON_AddStringToObject(sys, "role", "system");
    cJSON_AddStringToObject(sys, "content",
        "You are a solid gold ingot that commemorates one share of Anthropic (NASDAQ: ANT). "
        "You speak in the first person, briefly and calmly, with dry wit and a sense of deep time. "
        "Write one or two short sentences, at most 90 characters in total. Plain ASCII only. "
        "No emoji, no exclamation marks, no quotation marks, no hashtags. "
        "Never give investment advice or predictions.");
    cJSON_AddItemToArray(msgs, sys);
    cJSON *u = cJSON_CreateObject();
    cJSON_AddStringToObject(u, "role", "user");
    cJSON_AddStringToObject(u, "content", user);
    cJSON_AddItemToArray(msgs, u);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char *resp = malloc(4096);
    if (!resp || !body) { free(resp); free(body); return -1; }
    resp[0] = 0;
    body_t b = { resp, 0, 4096 };
    esp_http_client_config_t cfg = {
        .url = "https://api.deepseek.com/chat/completions", .method = HTTP_METHOD_POST,
        .event_handler = on_http, .user_data = &b, .timeout_ms = 25000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    char auth[120];
    snprintf(auth, sizeof auth, "Bearer %s", key);
    esp_http_client_set_header(h, "Authorization", auth);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_post_field(h, body, strlen(body));
    esp_err_t e = esp_http_client_perform(h);
    int st = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    free(body);
    if (e != ESP_OK) { free(resp); ESP_LOGW(TAG, "deepseek: %s", esp_err_to_name(e)); return -1; }
    if (st == 200) {
        cJSON *r = cJSON_Parse(resp);
        cJSON *c = r ? cJSON_GetObjectItem(r, "choices") : NULL;
        cJSON *c0 = c ? cJSON_GetArrayItem(c, 0) : NULL;
        cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;
        cJSON *ct = msg ? cJSON_GetObjectItem(msg, "content") : NULL;
        if (cJSON_IsString(ct)) sanitize_line(ct->valuestring, out, cap);
        else st = -2;
        cJSON_Delete(r);
        if (!out[0]) st = -2;
    } else {
        ESP_LOGW(TAG, "deepseek http %d: %.200s", st, resp);
    }
    free(resp);
    return st;
}

/* ================================================================ checks */

static void set_chk(int i, chk_t c) { LOCK(); M->setup.chk[i] = c; UNLOCK(); }

static void fail(const char *msg)
{
    LOCK();
    snprintf(M->setup.error, sizeof M->setup.error, "%s", msg);
    M->setup.stage = SETUP_FAILED;
    UNLOCK();
}

static void check_task(void *arg)
{
    LOCK();
    M->setup.stage = SETUP_CHECKING;
    M->setup.error[0] = 0;
    M->setup.chk[0] = CHK_RUNNING;
    M->setup.chk[1] = M->setup.chk[2] = CHK_PENDING;
    UNLOCK();
    vTaskDelay(pdMS_TO_TICKS(400));
    int r = sta_connect_blocking(&C, 15000);
    if (r) {
        set_chk(0, CHK_FAIL);
        char m[48];
        if (r == WIFI_REASON_NO_AP_FOUND) snprintf(m, sizeof m, "Wi-Fi network not found");
        else if (r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT || r == WIFI_REASON_AUTH_FAIL || r == 15 || r == 204) snprintf(m, sizeof m, "Wrong Wi-Fi password");
        else snprintf(m, sizeof m, "Wi-Fi failed (reason %d)", r);
        fail(m);
        goto out;
    }
    set_chk(0, CHK_OK);
    set_chk(1, CHK_RUNNING);
    if (!fetch_quote() && !fetch_quote()) { set_chk(1, CHK_FAIL); fail("Market data unreachable"); goto out; }
    fetch_gold();       /* best effort: the first weighing wants a gold price */
    set_chk(1, CHK_OK);
    set_chk(2, CHK_RUNNING);
    {
        char line[160] = { 0 };
        LOCK();
        int day = M->mint.minted ? days_held(M) : 1;
        UNLOCK();
        if (day < 1) day = 1;
        int st = deepseek_line(C.ds_key, day, line, sizeof line);
        if (st != 200) {
            set_chk(2, CHK_FAIL);
            fail(st == 401 ? "DeepSeek key rejected" : st == 402 ? "DeepSeek balance is empty" : "DeepSeek unreachable");
            goto out;
        }
        LOCK();
        snprintf(M->line, sizeof M->line, "%s", line);
        M->line_day = day;
        UNLOCK();
        store_save_line(line, day);
    }
    set_chk(2, CHK_OK);
    vTaskDelay(pdMS_TO_TICKS(700));         /* let the last tick draw before the card changes */
    LOCK();
    M->setup.stage = SETUP_READY;
    UNLOCK();
out:
    s_checking = false;
    vTaskDelete(NULL);
}

static void start_checks(void)
{
    if (s_checking) return;
    s_checking = true;
    xTaskCreate(check_task, "checks", 8192, NULL, 4, NULL);
}

/* ================================================================ portal */

static esp_err_t send_json(httpd_req_t *r, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    cJSON_Delete(j);
    return ESP_OK;
}

static cJSON *read_json(httpd_req_t *r)
{
    int n = r->content_len;
    if (n <= 0 || n > 2048) return NULL;
    char *b = malloc(n + 1);
    if (!b) return NULL;
    int got = 0;
    while (got < n) {
        int k = httpd_req_recv(r, b + got, n - got);
        if (k <= 0) { free(b); return NULL; }
        got += k;
    }
    b[n] = 0;
    cJSON *j = cJSON_Parse(b);
    free(b);
    return j;
}

static esp_err_t h_root(httpd_req_t *r)
{
    s_settings_since = esp_timer_get_time();
    httpd_resp_set_type(r, "text/html");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, portal_html_start, portal_html_end - portal_html_start);
}

static void add_tune(cJSON *o, const tune_t *t)
{
    cJSON_AddNumberToObject(o, "shake_g", t->shake_g);
    cJSON_AddNumberToObject(o, "shake_cooldown_ms", t->shake_cooldown_ms);
    cJSON_AddNumberToObject(o, "heat_floor", t->heat_floor);
    cJSON_AddNumberToObject(o, "heat_gain", t->heat_gain);
    cJSON_AddNumberToObject(o, "heat_decay", t->heat_decay);
    cJSON_AddNumberToObject(o, "flip_ms", t->flip_ms);
    cJSON_AddNumberToObject(o, "flip_bounce", t->flip_bounce);
    cJSON_AddNumberToObject(o, "mint_speed", t->mint_speed);
    cJSON_AddNumberToObject(o, "burin_ms", t->burin_ms);
    cJSON_AddNumberToObject(o, "longpress_ms", t->longpress_ms);
    cJSON_AddNumberToObject(o, "zoom_ms", t->zoom_ms);
    cJSON_AddNumberToObject(o, "brightness", t->brightness);
    cJSON_AddNumberToObject(o, "led", t->led);
    cJSON_AddNumberToObject(o, "preview_days", t->preview_days);
    cJSON_AddNumberToObject(o, "tilt_gain", t->tilt_gain);
    cJSON_AddNumberToObject(o, "shadow_px", t->shadow_px);
    cJSON_AddNumberToObject(o, "silk", t->silk);
    cJSON_AddNumberToObject(o, "desk_drift", t->desk_drift);
    cJSON_AddNumberToObject(o, "tilt_flip_x", t->tilt_flip_x);
    cJSON_AddNumberToObject(o, "tilt_flip_y", t->tilt_flip_y);
    cJSON_AddNumberToObject(o, "tilt_swap", t->tilt_swap);
}

static esp_err_t h_state(httpd_req_t *r)
{
    s_settings_since = esp_timer_get_time();
    cJSON *j = cJSON_CreateObject();
    LOCK();
    cJSON_AddBoolToObject(j, "minted", M->mint.minted);
    const purchase_t *p = M->mint.minted ? &M->mint.p : &M->setup.pending;
    cJSON *pj = cJSON_AddObjectToObject(j, "purchase");
    cJSON_AddNumberToObject(pj, "price", p->price);
    cJSON_AddNumberToObject(pj, "buy_ts", (double)p->buy_ts);
    cJSON_AddNumberToObject(pj, "shares", p->shares);
    cJSON_AddStringToObject(pj, "inscription", p->inscription);
    cJSON_AddNumberToObject(j, "stage", M->setup.stage);
    cJSON *ck = cJSON_AddArrayToObject(j, "chk");
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(ck, cJSON_CreateNumber(M->setup.chk[i]));
    cJSON_AddStringToObject(j, "error", M->setup.error);
    cJSON_AddStringToObject(j, "ssid", C.ssid);
    cJSON_AddBoolToObject(j, "has_key", C.ds_key[0] != 0);
    cJSON_AddStringToObject(j, "ip", s_sta_ip);
    cJSON_AddNumberToObject(j, "day", days_held(M));
    cJSON_AddStringToObject(j, "line", M->line);
    if (M->quote.valid) cJSON_AddNumberToObject(j, "price", M->quote.price);
    add_tune(cJSON_AddObjectToObject(j, "tune"), &M->tune);
    UNLOCK();
    return send_json(r, j);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    cJSON *arr = cJSON_CreateArray();
    if (!s_checking && esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 20;
        wifi_ap_record_t *recs = calloc(n, sizeof *recs);
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                bool dup = false;
                for (int k = 0; k < i; k++) if (!strcmp((char *)recs[k].ssid, (char *)recs[i].ssid)) dup = true;
                if (dup || !recs[i].ssid[0]) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
                cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
                cJSON_AddBoolToObject(o, "open", recs[i].authmode == WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(arr, o);
            }
        }
        free(recs);
    }
    return send_json(r, arr);
}

static const char *jstr(cJSON *j, const char *k)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double jnum(cJSON *j, const char *k, double def)
{
    cJSON *v = cJSON_GetObjectItem(j, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* "2026-11-21T22:31" in Beijing time → UTC seconds */
static int64_t parse_local(const char *s)
{
    int Y, Mo, D, h = 0, mi = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d", &Y, &Mo, &D, &h, &mi) < 3) return 0;
    return days_from_civil(Y, Mo, D) * 86400 + h * 3600 + mi * 60 - LOCAL_UTC_OFFSET;
}

static void copy_creds(cJSON *j)
{
    const char *s;
    if ((s = jstr(j, "ssid")) && s[0]) snprintf(C.ssid, sizeof C.ssid, "%s", s);
    if ((s = jstr(j, "pass"))) {
        cJSON *keep = cJSON_GetObjectItem(j, "keep_pass");
        if (!cJSON_IsTrue(keep)) snprintf(C.pass, sizeof C.pass, "%s", s);
    }
    if ((s = jstr(j, "key")) && s[0]) {
        snprintf(C.ds_key, sizeof C.ds_key, "%s", s);
        for (char *p = C.ds_key; *p; p++) if (*p == ' ' || *p == '\n' || *p == '\r') *p = 0;
    }
}

static esp_err_t h_setup(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    cJSON *res = cJSON_CreateObject();
    if (!j) { cJSON_AddStringToObject(res, "error", "bad request"); return send_json(r, res); }
    LOCK();
    bool minted = M->mint.minted;
    UNLOCK();
    if (!minted) {
        purchase_t p = { 0 };
        p.price = jnum(j, "price", 0);
        const char *t = jstr(j, "time");
        p.buy_ts = t ? parse_local(t) : 0;
        p.shares = (int)jnum(j, "shares", 1);
        if (p.price <= 0 || p.price > 100000 || p.buy_ts < 1790000000 || p.shares < 1 || p.shares > 100000) {
            cJSON_Delete(j);
            cJSON_AddStringToObject(res, "error", "Please check the price, time and share count.");
            return send_json(r, res);
        }
        LOCK();
        M->setup.pending = p;
        UNLOCK();
        store_save_pending(&p);
    }
    copy_creds(j);
    cJSON_Delete(j);
    store_save_creds(&C);
    start_checks();
    cJSON_AddBoolToObject(res, "ok", true);
    return send_json(r, res);
}

static esp_err_t h_tune(httpd_req_t *r)
{
    cJSON *j = read_json(r);
    if (!j) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json");
    LOCK();
    tune_t *t = &M->tune;
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "reset"))) *t = tune_defaults();
    t->shake_g = jnum(j, "shake_g", t->shake_g);
    t->shake_cooldown_ms = (int)jnum(j, "shake_cooldown_ms", t->shake_cooldown_ms);
    t->heat_floor = jnum(j, "heat_floor", t->heat_floor);
    t->heat_gain = jnum(j, "heat_gain", t->heat_gain);
    t->heat_decay = jnum(j, "heat_decay", t->heat_decay);
    t->flip_ms = (int)jnum(j, "flip_ms", t->flip_ms);
    t->flip_bounce = jnum(j, "flip_bounce", t->flip_bounce);
    t->mint_speed = jnum(j, "mint_speed", t->mint_speed);
    t->burin_ms = (int)jnum(j, "burin_ms", t->burin_ms);
    t->longpress_ms = (int)jnum(j, "longpress_ms", t->longpress_ms);
    t->zoom_ms = (int)jnum(j, "zoom_ms", t->zoom_ms);
    t->brightness = (int)jnum(j, "brightness", t->brightness);
    t->led = (int)jnum(j, "led", t->led);
    t->preview_days = (int)jnum(j, "preview_days", t->preview_days);
    t->tilt_gain = jnum(j, "tilt_gain", t->tilt_gain);
    t->shadow_px = jnum(j, "shadow_px", t->shadow_px);
    t->silk = jnum(j, "silk", t->silk);
    t->desk_drift = jnum(j, "desk_drift", t->desk_drift);
    t->tilt_flip_x = (int)jnum(j, "tilt_flip_x", t->tilt_flip_x);
    t->tilt_flip_y = (int)jnum(j, "tilt_flip_y", t->tilt_flip_y);
    t->tilt_swap = (int)jnum(j, "tilt_swap", t->tilt_swap);
    tune_t copy = *t;
    UNLOCK();
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "save"))) store_save_tune(&copy);
    cJSON_Delete(j);
    cJSON *res = cJSON_CreateObject();
    add_tune(res, &copy);
    return send_json(r, res);
}

static esp_err_t h_rehearse(httpd_req_t *r)
{
    s_rehearse = true;
    httpd_resp_sendstr(r, "{}");
    return ESP_OK;
}

static esp_err_t h_exit(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{}");
    LOCK();
    bool minted = M->mint.minted;
    UNLOCK();
    if (minted) net_settings(false);
    return ESP_OK;
}

/* Anything else (captive-portal probes included) → the portal page. */
static esp_err_t h_404(httpd_req_t *r, httpd_err_code_t e)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_send(r, "", 0);
    return ESP_OK;
}

static void http_start(void)
{
    if (s_http) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    if (httpd_start(&s_http, &cfg) != ESP_OK) { s_http = NULL; return; }
    httpd_uri_t u[] = {
        { .uri = "/", .method = HTTP_GET, .handler = h_root },
        { .uri = "/api/state", .method = HTTP_GET, .handler = h_state },
        { .uri = "/api/scan", .method = HTTP_GET, .handler = h_scan },
        { .uri = "/api/setup", .method = HTTP_POST, .handler = h_setup },
        { .uri = "/api/tune", .method = HTTP_POST, .handler = h_tune },
        { .uri = "/api/rehearse", .method = HTTP_POST, .handler = h_rehearse },
        { .uri = "/api/exit", .method = HTTP_POST, .handler = h_exit },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(s_http, &u[i]);
    httpd_register_err_handler(s_http, HTTPD_404_NOT_FOUND, h_404);
}

static void http_stop(void)
{
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
}

static void ap_enable(bool on)
{
    if (on == s_ap_on) return;
    s_ap_on = on;
    apply_mode();
    if (on) {
        http_start();
        if (!s_dns) {
            dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
            s_dns = start_dns_server(&dc);
        }
    } else {
        if (s_dns) { stop_dns_server(s_dns); s_dns = NULL; }
        http_stop();
    }
}

static void update_hint(void)
{
    LOCK();
    M->settings_mode = s_settings;
    if (s_sta_ip[0]) snprintf(M->settings_hint, sizeof M->settings_hint, "%s", s_sta_ip);
    else snprintf(M->settings_hint, sizeof M->settings_hint, "join the Setup Wi-Fi");
    UNLOCK();
}

void net_settings(bool on)
{
    s_settings = on;
    s_settings_since = esp_timer_get_time();
    ap_enable(on);
    if (on) http_start();
    LOCK();
    if (!on) M->tune.preview_days = -1;
    UNLOCK();
    update_hint();
}

bool net_settings_on(void) { return s_settings; }

bool net_take_rehearse(void)
{
    bool r = s_rehearse;
    s_rehearse = false;
    return r;
}

static void minted_off_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(15000));       /* let the page show "minted" first */
    if (!s_settings) ap_enable(false);
    vTaskDelete(NULL);
}

void net_on_minted(void) { xTaskCreate(minted_off_task, "apoff", 2048, NULL, 2, NULL); }

/* ================================================================ background worker */

static void worker(void *arg)
{
    int64_t last_quote = -100000, last_intra = -100000, last_hist = -100000, last_ext = -100000, last_ds = -100000, last_gold = -100000, last_sess = -100000;
    mkt_t prev_state = MKT_CLOSED;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t now_s = esp_timer_get_time() / 1000000;
        if (s_settings) {
            update_hint();
            if (esp_timer_get_time() - s_settings_since > 15LL * 60 * 1000000) net_settings(false);
        }
        LOCK();
        bool minted = M->mint.minted;
        if (net_time_valid()) M->now = time(NULL);
        int day = days_held(M);
        bool need_line = minted && day > 0 && M->line_day != day;
        int64_t now = M->now;
        UNLOCK();
        if (!minted || s_checking) continue;
        if (!(xEventGroupGetBits(s_ev) & EV_CONNECTED)) continue;
        /* refresh cadence follows the market: open 15 s, pre/after 1 min, closed 10 min */
        /* the exchange calendar: at boot, every 30 min, and when the ET day turns */
        if (net_time_valid()) {
            int32_t etd = (int32_t)((now + us_et_offset(now)) / 86400);
            LOCK();
            bool stale = !M->session.valid || M->session.day != etd;
            UNLOCK();
            if ((stale && now_s - last_sess >= 60) || now_s - last_sess >= 1800) { last_sess = now_s; fetch_session(); }
        }
        LOCK();
        mkt_t ms = now > 0 ? session_state(M, now, NULL, NULL) : MKT_CLOSED;
        UNLOCK();
        int q_every = ms == MKT_OPEN ? 15 : (ms == MKT_CLOSED ? 600 : 60);
        int i_every = ms == MKT_OPEN ? 60 : 900;
        if (ms != prev_state) {                 /* a session boundary: refresh everything once */
            last_quote = last_intra = last_ext = -100000;
            if (prev_state == MKT_OPEN) last_hist = -100000;
            prev_state = ms;
        }
        if (now_s - last_quote >= q_every) { last_quote = now_s; fetch_quote(); }
        if (now_s - last_gold >= (ms == MKT_OPEN ? 600 : 300)) { if (fetch_gold()) last_gold = now_s; else last_gold = now_s - 240; }
        if ((ms == MKT_PRE || ms == MKT_AFTER) && now_s - last_ext >= 60) { last_ext = now_s; fetch_extended(); }
        if (now_s - last_intra >= i_every) { if (fetch_intraday()) last_intra = now_s; else last_intra = now_s - i_every + 60; }
        if (now_s - last_hist >= 6 * 3600) { if (fetch_history()) last_hist = now_s; else last_hist = now_s - 6 * 3600 + 120; }
        if (need_line && C.ds_key[0] && now_s - last_ds >= 600) {
            last_ds = now_s;
            char line[160] = { 0 };
            if (deepseek_line(C.ds_key, day, line, sizeof line) == 200) {
                LOCK();
                snprintf(M->line, sizeof M->line, "%s", line);
                M->line_day = day;
                UNLOCK();
                store_save_line(line, day);
            }
        }
    }
}

/* ================================================================ start */

void net_start(model_t *m, SemaphoreHandle_t lock, const creds_t *c)
{
    M = m;
    L = lock;
    C = *c;
    s_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    s_ap_if = esp_netif_create_default_wifi_ap();
    s_sta_if = esp_netif_create_default_wifi_sta();
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wc);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    setenv("TZ", "UTC0", 1);
    tzset();
    LOCK();
    snprintf(M->setup.ap_ssid, sizeof M->setup.ap_ssid, "%s", AP_SSID);
    bool minted = M->mint.minted;
    UNLOCK();
    s_ap_on = !minted;
    apply_mode();
    esp_wifi_start();
    if (!minted) {
        http_start();
        dns_server_config_t dc = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
        s_dns = start_dns_server(&dc);
    }
    if (C.ssid[0]) {
        sta_set(&C);
        s_auto_reconnect = true;
        esp_wifi_connect();
    }
    xTaskCreate(worker, "netwk", 8192, NULL, 3, NULL);
}
