/* Data shared between the platform layer (network, storage, sensors) and the scene. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define IPO_PRICE 83.00
#define DEFAULT_TOTAL_SHARES 13086672591.0

typedef struct {
    double price;           /* USD per share */
    int64_t buy_ts;         /* UTC unix seconds */
    int shares;
    char inscription[33];   /* optional, ASCII */
} purchase_t;

typedef struct {
    bool minted;
    purchase_t p;
    int64_t mint_ts;        /* when the shake happened (0 if clock unknown) */
    uint32_t serial;
} mint_rec_t;

typedef struct {
    bool valid;
    double price, prev_close, open, high, low, chg_pct, volume;
    double total_shares;
    double ext_price, ext_pct;  /* pre-market / after-hours, 0 if none */
    double gold;                /* spot gold, USD per troy ounce, 0 if unknown */
    char when[24];          /* exchange time, e.g. "2026-11-19 16:00:01" (ET) */
    int64_t fetched_ts;
} quote_t;

/* The front's living inscription: the bar is "weighed" at each close. */
typedef struct {
    bool valid;
    double price;               /* per share, USD */
    double grams;               /* whole holding in grams of gold, 0 if gold was unknown */
    int32_t close_day;          /* ET day index of the close it shows; 0 = the purchase itself */
} assay_t;

#define TROY_OZ_G 31.1034768

/* Today's minute prices (regular session) and longer daily/weekly closes. */
#define INTRA_MAX 400
#define SERIES_MAX 300

typedef struct {
    int n;
    int date;                   /* yyyymmdd (ET) */
    int16_t min[INTRA_MAX];     /* minutes since 09:30 ET */
    float px[INTRA_MAX];
    uint32_t rev;               /* bumps on every update */
} intraday_t;

typedef struct {
    int n;
    int32_t day[SERIES_MAX];    /* days since 1970-01-01 */
    float close[SERIES_MAX];
    uint32_t rev;
} series_t;

/* Pre-mint setup progress, driven by the captive portal. */
typedef enum { SETUP_WAITING = 0, SETUP_CHECKING, SETUP_FAILED, SETUP_READY } setup_stage_t;
typedef enum { CHK_PENDING = 0, CHK_RUNNING, CHK_OK, CHK_FAIL } chk_t;

typedef struct {
    setup_stage_t stage;
    chk_t chk[3];           /* wifi, market data, deepseek */
    char error[48];
    char ap_ssid[33];
    purchase_t pending;     /* what will be engraved */
} setup_t;

typedef struct {
    float shake_g;          /* gesture threshold, high-passed |a| in g */
    int   shake_cooldown_ms;
    float heat_floor;       /* g of shake energy that produces no heat */
    float heat_gain;        /* heat per second per g above floor */
    float heat_decay;       /* heat lost per second */
    int   flip_ms;
    float flip_bounce;      /* 0..0.3 overshoot */
    float mint_speed;       /* 1 = designed timing */
    int   burin_ms;         /* per character when engraving the daily line */
    int   longpress_ms;
    int   zoom_ms;
    int   brightness;       /* 5..100 */
    int   led;              /* 0..100 */
    int   preview_days;     /* -1 = real age; otherwise preview patina (not saved) */
    float tilt_gain;        /* how far the light swings per g of tilt */
    float shadow_px;        /* cast-shadow length */
    float silk;             /* guilloche shimmer strength 0..1 */
    float desk_drift;       /* 0 = shimmer only when moved; >0 = slow drift on the desk */
    int   tilt_flip_x, tilt_flip_y, tilt_swap;   /* IMU axis fixes */
} tune_t;

static inline tune_t tune_defaults(void)
{
    tune_t t = {
        .shake_g = 1.1f, .shake_cooldown_ms = 550,
        .heat_floor = 0.25f, .heat_gain = 0.85f, .heat_decay = 0.45f,
        .flip_ms = 420, .flip_bounce = 0.10f,
        .mint_speed = 1.0f, .burin_ms = 38,
        .longpress_ms = 700, .zoom_ms = 7500,
        .brightness = 85, .led = 50, .preview_days = -1,
        .tilt_gain = 1.0f, .shadow_px = 1.5f, .silk = 0.5f, .desk_drift = 0,
        .tilt_flip_x = 0, .tilt_flip_y = 0, .tilt_swap = 0,
    };
    return t;
}

typedef struct {
    mint_rec_t mint;
    setup_t setup;
    quote_t quote;
    assay_t assay;
    intraday_t intra;
    series_t daily;             /* daily closes, or weekly once the history outgrows SERIES_MAX */
    char line[160];         /* today's DeepSeek line */
    int  line_day;          /* day number (held) the line belongs to, 0 = none */
    int  line_seen_day;     /* last day whose line finished engraving on screen */
    int64_t now;            /* UTC unix seconds, 0 if unknown */
    int  patina_day;        /* last day the patina was shown (persisted) */
    bool settings_mode;
    char settings_hint[64];
    tune_t tune;
} model_t;

/* Beijing time is the user's local time. */
#define LOCAL_UTC_OFFSET (8 * 3600)

static inline int64_t local_day_index(int64_t utc) { return (utc + LOCAL_UTC_OFFSET) / 86400; }

/* Day 1 = the day of purchase (local). */
static inline int days_held(const model_t *m)
{
    if (!m->mint.minted || m->now <= 0) return 0;
    int64_t d = local_day_index(m->now) - local_day_index(m->mint.p.buy_ts) + 1;
    return d < 1 ? 1 : (int)d;
}
