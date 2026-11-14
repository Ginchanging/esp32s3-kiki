/* Kiki's default routine. Times are minutes after local (UTC+8) midnight. */
#include <time.h>

#include "dash_model.h"

typedef struct {
    int start_min;
    dash_activity_t act;
} slot_t;

#define M(h, m) ((h) * 60 + (m))

static const slot_t weekday[] = {
    {M(0, 0), DASH_ACT_SLEEP},     {M(7, 0), DASH_ACT_EXERCISE}, {M(7, 40), DASH_ACT_BREAKFAST},
    {M(8, 30), DASH_ACT_CLASS},    {M(11, 50), DASH_ACT_LUNCH},  {M(13, 0), DASH_ACT_NAP},
    {M(14, 0), DASH_ACT_CODE},     {M(17, 40), DASH_ACT_DINNER}, {M(18, 40), DASH_ACT_FREE},
    {M(22, 50), DASH_ACT_WASH},    {M(23, 30), DASH_ACT_SLEEP},
};

static const slot_t weekend[] = {
    {M(0, 0), DASH_ACT_SLEEP},     {M(9, 0), DASH_ACT_EXERCISE}, {M(9, 30), DASH_ACT_BREAKFAST},
    {M(10, 10), DASH_ACT_FREE},    {M(11, 50), DASH_ACT_LUNCH},  {M(13, 0), DASH_ACT_NAP},
    {M(14, 0), DASH_ACT_SUNBATHE}, {M(16, 0), DASH_ACT_CODE},    {M(17, 40), DASH_ACT_DINNER},
    {M(18, 40), DASH_ACT_FREE},    {M(22, 50), DASH_ACT_WASH},   {M(23, 30), DASH_ACT_SLEEP},
};

static void local_tm(int64_t utc, struct tm *out)
{
    time_t t = (time_t)(utc + DASH_TZ_OFFSET_S);
    gmtime_r(&t, out);
}

dash_activity_t dash_schedule_at(int64_t utc, dash_activity_t *next, int *next_min)
{
    struct tm tm;
    local_tm(utc, &tm);
    bool we = tm.tm_wday == 0 || tm.tm_wday == 6;
    const slot_t *tab = we ? weekend : weekday;
    int n = we ? (int)(sizeof(weekend) / sizeof(*weekend)) : (int)(sizeof(weekday) / sizeof(*weekday));
    int now_min = tm.tm_hour * 60 + tm.tm_min;
    int cur = 0;
    for (int i = 0; i < n; ++i) {
        if (tab[i].start_min <= now_min) {
            cur = i;
        }
    }
    if (next || next_min) {
        int ni = cur + 1 < n ? cur + 1 : 1; /* after the last slot: tomorrow's first wake-up slot */
        if (next) {
            *next = tab[ni].act;
        }
        if (next_min) {
            *next_min = tab[ni].start_min;
        }
    }
    return tab[cur].act;
}

bool dash_schedule_is_night(int64_t utc)
{
    return dash_schedule_at(utc, NULL, NULL) == DASH_ACT_SLEEP;
}

int32_t dash_local_day(int64_t utc)
{
    int64_t t = utc + DASH_TZ_OFFSET_S;
    return (int32_t)(t >= 0 ? t / 86400 : (t - 86399) / 86400);
}

dash_diary_t dash_diary_due(int64_t utc, int32_t last_written_day)
{
    if (utc < 1700000000) { /* clock not set yet */
        return DASH_DIARY_NONE;
    }
    int32_t day = dash_local_day(utc);
    int64_t sec = utc + DASH_TZ_OFFSET_S - (int64_t)day * 86400;
    if (sec < 23 * 3600 || day == last_written_day) {
        return DASH_DIARY_NONE;
    }
    return (day + 4) % 7 == 0 ? DASH_DIARY_WEEK : DASH_DIARY_DAY; /* 1970-01-01 was a Thursday */
}

void dash_pomo_week_put(dash_pomo_day_t week[DASH_DAYS], int32_t day, int count, int minutes)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < DASH_DAYS; ++i) {
        if (week[i].day == day) {
            slot = i;
            break;
        }
        if (week[i].day < week[oldest].day) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }
    week[slot].day = day;
    week[slot].count = (uint16_t)(count < 0 ? 0 : count > 999 ? 999 : count);
    week[slot].minutes = (uint16_t)(minutes < 0 ? 0 : minutes > 9999 ? 9999 : minutes);
}
