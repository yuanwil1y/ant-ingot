#include "market.h"
#include <stddef.h>

static int64_t et_day(int64_t utc) { return (utc + us_et_offset(utc)) / 86400; }

mkt_t session_state(const model_t *m, int64_t now, int64_t *next_open, int64_t *close_at)
{
    const session_t *s = &m->session;
    if (s->valid && s->day == et_day(now)) {
        if (!s->business) {
            if (close_at) *close_at = 0;
            if (next_open) *next_open = s->next_open;
            if (s->next_open > now) return MKT_CLOSED;
        } else if (s->open > 0 && et_day(s->open) == s->day) {
            if (close_at) *close_at = s->close;
            if (next_open) *next_open = now < s->open ? s->open : s->next_open;
            if (now < s->pre_open) return MKT_CLOSED;
            if (now < s->open) return MKT_PRE;
            if (now < s->close) return MKT_OPEN;
            if (now < s->post_close) return MKT_AFTER;
            if (!s->next_open && next_open) market_state(now, next_open, NULL);
            return MKT_CLOSED;
        }
    }
    return market_state(now, next_open, close_at);
}
