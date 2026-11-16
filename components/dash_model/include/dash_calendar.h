/* Independent calendar data and offline holiday dates. Pure C, no LVGL/IDF. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DASH_CALENDAR_BATCH 8
#define DASH_CALENDAR_DAYS 7
#define DASH_CALENDAR_MAX_EVENTS 4096
#define DASH_CALENDAR_STALE_S 1800

typedef struct {
    int64_t start, end; /* UTC seconds, end exclusive; dates use China midnight */
    bool all_day;
    char title[97], calendar[49]; /* UTF-8 byte limits, shared with the hub */
} dash_calendar_event_t;

typedef struct {
    bool have, configured, ok, error;
    int64_t fetched_at;
    int32_t week_start, day; /* days since 1970-01-01 in UTC+8 */
    uint16_t counts[DASH_CALENDAR_DAYS], offset, total;
    uint8_t count;
    unsigned fetch_failures;
    dash_calendar_event_t events[DASH_CALENDAR_BATCH];
} dash_calendar_t;

/* Accept endpoint JSON or a fixture's "calendar" object. Atomic on failure. */
bool dash_calendar_parse(dash_calendar_t *out, const char *json, size_t len);
bool dash_calendar_fresh(const dash_calendar_t *st, int64_t now);

typedef enum {
    DASH_HOL_SPRING = 1u << 0,
    DASH_HOL_LANTERN = 1u << 1,
    DASH_HOL_DRAGON = 1u << 2,
    DASH_HOL_QIXI = 1u << 3,
    DASH_HOL_CHONGYANG = 1u << 4,
    DASH_HOL_SOLSTICE = 1u << 5,
    DASH_HOL_NATIONAL = 1u << 6,
    DASH_HOL_HALLOWEEN = 1u << 7,
    DASH_HOL_THANKSGIVING = 1u << 8,
    DASH_HOL_CHRISTMAS = 1u << 9,
    DASH_HOL_BIRTHDAY = 1u << 10,
} dash_holiday_t;

/* Lunar/solstice coverage 2026..2099; Gregorian holidays work outside it too.
 * The caller must check clock_valid. Does not change the process timezone. */
uint16_t dash_holidays_at(int64_t utc);
