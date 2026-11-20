/* Clock-driven free-time behavior. Does not change the daily schedule. */
#pragma once
#include <stdint.h>

typedef enum {
    DASH_IDLE_SIT = 0,
    DASH_IDLE_WINDOW,
    DASH_IDLE_LIE,
    DASH_IDLE_GROOM,
    DASH_IDLE_YAWN,
    DASH_IDLE_YARN,
    DASH_IDLE_COUNT
} dash_idle_kind_t;

typedef struct {
    dash_idle_kind_t kind, base;
    int64_t started_at, base_started_at; /* UTC seconds */
    uint16_t elapsed_s, duration_s, base_duration_s;
} dash_idle_state_t;

/* Pure/reproducible UTC+8 timeline, called only for DASH_ACT_FREE. The UI
 * supplies a quiet SIT fallback until its clock is valid. */
dash_idle_state_t dash_idle_at(int64_t utc);
