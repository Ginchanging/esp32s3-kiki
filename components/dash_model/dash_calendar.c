#include "dash_model.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>
#include <time.h>

static bool integer(const cJSON *o, const char *key, int64_t lo, int64_t hi, int64_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo ||
        v->valuedouble > hi || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (int64_t)v->valuedouble;
    return true;
}

static bool text(const cJSON *o, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || strlen(v->valuestring) >= size) return false;
    memcpy(out, v->valuestring, strlen(v->valuestring)+1);
    return true;
}

bool dash_calendar_parse(dash_calendar_t *out, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "calendar");
    if (!o) o = root;
    static dash_calendar_t next; /* firmware fetch stack stays small */
    memset(&next, 0, sizeof(next));
    int64_t n;
    const cJSON *configured = cJSON_GetObjectItemCaseSensitive(o, "configured");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(o, "ok");
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(o, "error");
    bool valid = cJSON_IsObject(o) && integer(o, "schema", 1, 1, &n) &&
        cJSON_IsBool(configured) && cJSON_IsBool(ok) && (!error || cJSON_IsBool(error));
    next.configured = cJSON_IsTrue(configured);
    next.ok = cJSON_IsTrue(ok);
    next.error = cJSON_IsTrue(error);
    if (!valid || !integer(o, "fetched_at", 0, 4102444800LL, &next.fetched_at) ||
        !integer(o, "week_start", 0, 47481, &n)) goto bad;
    next.week_start = (int32_t)n;
    if (!integer(o, "day", next.week_start, next.week_start+6, &n)) goto bad;
    next.day = (int32_t)n;
    if (!integer(o, "offset", 0, DASH_CALENDAR_MAX_EVENTS, &n) || n % DASH_CALENDAR_BATCH) goto bad;
    next.offset = (uint16_t)n;
    if (!integer(o, "total", 0, DASH_CALENDAR_MAX_EVENTS, &n)) goto bad;
    next.total = (uint16_t)n;
    const cJSON *counts = cJSON_GetObjectItemCaseSensitive(o, "counts");
    const cJSON *events = cJSON_GetObjectItemCaseSensitive(o, "events");
    if (!cJSON_IsArray(counts) || cJSON_GetArraySize(counts) != 7 || !cJSON_IsArray(events) ||
        cJSON_GetArraySize(events) > DASH_CALENDAR_BATCH) goto bad;
    for (int i = 0; i < 7; ++i) {
        const cJSON *v = cJSON_GetArrayItem(counts, i);
        if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > DASH_CALENDAR_MAX_EVENTS ||
            floor(v->valuedouble) != v->valuedouble) goto bad;
        next.counts[i] = (uint16_t)v->valuedouble;
    }
    const cJSON *v;
    cJSON_ArrayForEach(v, events) {
        dash_calendar_event_t *e = &next.events[next.count++];
        const cJSON *all_day = cJSON_GetObjectItemCaseSensitive(v, "all_day");
        if (!cJSON_IsObject(v) || !cJSON_IsBool(all_day) ||
            !integer(v, "start", 1, 4102444800LL, &e->start) ||
            !integer(v, "end", e->start+1, 4102444800LL, &e->end) ||
            !text(v, "title", e->title, sizeof(e->title)) ||
            !text(v, "calendar", e->calendar, sizeof(e->calendar))) goto bad;
        e->all_day = cJSON_IsTrue(all_day);
        int64_t lo = (int64_t)next.day*86400-DASH_TZ_OFFSET_S;
        if (e->start >= lo+86400 || e->end <= lo) goto bad;
        if (e->all_day && ((e->start+DASH_TZ_OFFSET_S)%86400 || (e->end+DASH_TZ_OFFSET_S)%86400)) goto bad;
    }
    if (next.count > next.total || (next.count && next.offset+next.count > next.total) ||
        next.counts[next.day-next.week_start] != next.total ||
        (!next.configured && (next.count || next.total || next.ok)) ||
        (next.ok && (!next.fetched_at || next.error))) goto bad;
    next.have = true;
    *out = next;
    cJSON_Delete(root);
    return true;
bad:
    cJSON_Delete(root);
    return false;
}

bool dash_calendar_fresh(const dash_calendar_t *st, int64_t now)
{
    return st->have && st->configured && st->ok && !st->fetch_failures && st->fetched_at > 0 &&
        now >= st->fetched_at-60 && now-st->fetched_at <= DASH_CALENDAR_STALE_S &&
        st->week_start == dash_local_day(now);
}

/* Generated from Hong Kong Observatory annual tables; see scripts/gen_holidays.py.
 * Each row: zero-based year days for Spring, Lantern, Dragon Boat, Qixi,
 * Double Ninth, Winter Solstice. Normal lunar months only. */
static const uint16_t holiday_days[][6] = {
#include "dash_holiday_dates.inc"
};

uint16_t dash_holidays_at(int64_t utc)
{
    time_t local = (time_t)(utc+DASH_TZ_OFFSET_S);
    struct tm tm;
    gmtime_r(&local, &tm);
    int year = tm.tm_year+1900, md = (tm.tm_mon+1)*100+tm.tm_mday;
    uint16_t flags = 0;
    if (year >= 2026 && year <= 2099) {
        const uint16_t *days = holiday_days[year-2026];
        if (tm.tm_yday >= days[0] && tm.tm_yday < days[0]+7) flags |= DASH_HOL_SPRING;
        for (int i = 1; i < 6; ++i) if (tm.tm_yday == days[i]) flags |= (uint16_t)(1u << i);
    }
    if (md >= 1001 && md <= 1007) flags |= DASH_HOL_NATIONAL;
    if (md == 1031) flags |= DASH_HOL_HALLOWEEN;
    if (tm.tm_mon == 10 && tm.tm_wday == 4 && tm.tm_mday >= 22 && tm.tm_mday <= 28)
        flags |= DASH_HOL_THANKSGIVING;
    if (md == 1225) flags |= DASH_HOL_CHRISTMAS;
    if (md == 807) flags |= DASH_HOL_BIRTHDAY;
    return flags;
}
