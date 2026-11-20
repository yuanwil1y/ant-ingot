/* Market session: the exchange's own calendar when we have it, a weekday rule otherwise. */
#pragma once
#include "model.h"
#include "ingot.h"

mkt_t session_state(const model_t *m, int64_t now, int64_t *next_open, int64_t *close_at);
