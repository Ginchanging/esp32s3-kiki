#include "dash_model.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

_Static_assert(sizeof(dash_health_t) <= 2048, "health model must stay small");

static bool integer(const cJSON *v, int64_t lo, int64_t hi, int64_t *out)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < lo ||
        v->valuedouble > hi || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (int64_t)v->valuedouble;
    return true;
}

static bool timestamp(const cJSON *o, const char *key, int64_t *out)
{
    return integer(cJSON_GetObjectItemCaseSensitive(o, key), 0, 4102444800LL, out);
}

static bool nullable(const cJSON *v, int32_t lo, int32_t hi, int32_t *out)
{
    if (cJSON_IsNull(v)) { *out = DASH_HEALTH_NONE; return true; }
    int64_t n;
    if (!integer(v, lo, hi, &n)) return false;
    *out = (int32_t)n;
    return true;
}

void dash_health_init(dash_health_t *st)
{
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < DASH_HEALTH_METRICS; ++i) st->values[i] = DASH_HEALTH_NONE;
    for (int i = 0; i < DASH_HEALTH_DAYS; ++i)
        st->activity[i][0] = st->activity[i][1] = DASH_HEALTH_NONE;
    for (int i = 0; i < DASH_HEALTH_WEIGHTS; ++i) st->weight_hist[i] = DASH_HEALTH_WEIGHT_NONE;
    st->weight_delta = st->weight_change = DASH_HEALTH_WEIGHT_NONE;
}

bool dash_health_read(dash_health_t *out, const cJSON *o)
{
    static const char *const keys[] = {"steps", "distance_m", "exercise_min", "active_kcal", "sleep_min",
                                      "core_min", "deep_min", "rem_min", "body_fat"};
    static const int32_t limits[] = {1000000, 2000000, 1440, 50000, 2880, 2880, 2880, 2880, 1000};
    dash_health_t next;
    dash_health_init(&next);
    int64_t n;
    const cJSON *configured = cJSON_GetObjectItemCaseSensitive(o, "configured");
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(o, "ok");
    if (!cJSON_IsObject(o) || !integer(cJSON_GetObjectItemCaseSensitive(o, "schema"), 1, 1, &n) ||
        !cJSON_IsBool(configured) || !cJSON_IsBool(ok)) return false;
    next.configured = cJSON_IsTrue(configured);
    next.ok = cJSON_IsTrue(ok);
    if (!next.configured) {
        if (next.ok) return false;
        next.have = true;
        *out = next;
        return true;
    }
    if (!timestamp(o, "fetched_at", &next.fetched_at) || !timestamp(o, "since", &next.since) ||
        !timestamp(o, "weight_at", &next.weight_at) ||
        !integer(cJSON_GetObjectItemCaseSensitive(o, "day"), 0, 47481, &n) ||
        (next.ok && !next.fetched_at)) return false;
    next.day = (int32_t)n;
    const cJSON *values = cJSON_GetObjectItemCaseSensitive(o, "values");
    const cJSON *at = cJSON_GetObjectItemCaseSensitive(o, "source_at");
    const cJSON *activity = cJSON_GetObjectItemCaseSensitive(o, "activity");
    const cJSON *weights = cJSON_GetObjectItemCaseSensitive(o, "weight_hist");
    if (!cJSON_IsObject(values) || !cJSON_IsArray(at) || cJSON_GetArraySize(at) != DASH_HEALTH_METRICS ||
        !cJSON_IsArray(activity) || cJSON_GetArraySize(activity) != DASH_HEALTH_DAYS ||
        !cJSON_IsArray(weights) || cJSON_GetArraySize(weights) != DASH_HEALTH_WEIGHTS) return false;
    for (int i = 0; i < DASH_HEALTH_METRICS; ++i) {
        if (!nullable(cJSON_GetObjectItemCaseSensitive(values, keys[i]), 0, limits[i], &next.values[i]) ||
            !integer(cJSON_GetArrayItem(at, i), 0, 4102444800LL, &next.source_at[i]) ||
            (next.values[i] != DASH_HEALTH_NONE && !next.source_at[i])) return false;
    }
    for (int i = 0; i < DASH_HEALTH_DAYS; ++i) {
        const cJSON *row = cJSON_GetArrayItem(activity, i);
        if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) != 2 ||
            !nullable(cJSON_GetArrayItem(row, 0), 0, 1000000, &next.activity[i][0]) ||
            !nullable(cJSON_GetArrayItem(row, 1), 0, 1440, &next.activity[i][1])) return false;
    }
    int first = -1, last = -1;
    int32_t v;
    for (int i = 0; i < DASH_HEALTH_WEIGHTS; ++i) {
        if (!nullable(cJSON_GetArrayItem(weights, i), -2980, 2980, &v)) return false;
        next.weight_hist[i] = v == DASH_HEALTH_NONE ? DASH_HEALTH_WEIGHT_NONE : (int16_t)v;
        if (v != DASH_HEALTH_NONE) {
            if (first < 0) { first = i; if (v) return false; }
            last = i;
        }
    }
    if (!integer(cJSON_GetObjectItemCaseSensitive(o, "weight_span"), 0, DASH_HEALTH_WEIGHTS, &n) ||
        n != (first < 0 ? 0 : last-first+1)) return false;
    next.weight_span = (uint8_t)n;
    if (!nullable(cJSON_GetObjectItemCaseSensitive(o, "weight_delta"), -2980, 2980, &v)) return false;
    next.weight_delta = v == DASH_HEALTH_NONE ? DASH_HEALTH_WEIGHT_NONE : (int16_t)v;
    if (!nullable(cJSON_GetObjectItemCaseSensitive(o, "weight_change"), -2980, 2980, &v)) return false;
    next.weight_change = v == DASH_HEALTH_NONE ? DASH_HEALTH_WEIGHT_NONE : (int16_t)v;
    if ((first == last && v != DASH_HEALTH_NONE) ||
        (first != last && (v == DASH_HEALTH_NONE || v != next.weight_hist[last]))) return false;
    next.have = true;
    *out = next;
    return true;
}

bool dash_health_parse(dash_health_t *out, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "health");
    bool ok = dash_health_read(out, o ? o : root);
    cJSON_Delete(root);
    return ok;
}

int32_t dash_health_value(const dash_health_t *st, dash_health_metric_t metric, int64_t now)
{
    if (metric < 0 || metric >= DASH_HEALTH_METRICS || !st->configured) return DASH_HEALTH_NONE;
    if (metric <= DASH_HEALTH_ENERGY &&
        (st->day != dash_local_day(now) || !st->source_at[metric] ||
         dash_local_day(st->source_at[metric]) != st->day)) return DASH_HEALTH_NONE;
    return st->values[metric];
}
