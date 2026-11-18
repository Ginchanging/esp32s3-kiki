#include "dash_flights.h"
#include <math.h>
#include <string.h>
#include "cJSON.h"

static double number(const cJSON *o, const char *key, double fallback)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble : fallback;
}

static void string(const cJSON *o, const char *key, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    const char *s = cJSON_IsString(v) ? v->valuestring : "";
    size_t k = strlen(s);
    if (k >= n) {
        k = n - 1;
        while (k && ((unsigned char)s[k] & 0xc0) == 0x80) --k;
    }
    memcpy(out, s, k);
    out[k] = 0;
}

static bool parse_aircraft(const cJSON *o, dash_flight_t *f)
{
    if (!cJSON_IsObject(o)) return false;
    memset(f, 0, sizeof(*f));
    string(o, "hex", f->hex, sizeof(f->hex));
    if (strlen(f->hex) != 6 || strspn(f->hex, "0123456789abcdefABCDEF") != 6) return false;
    double d = number(o, "distance_m", -1), ts = number(o, "observed_at", 0);
    if (d < 0 || d > 1000000 || ts < 1 || ts > 4102444800.0) return false;
    f->distance_m = (int32_t)d;
    f->observed_at = (int64_t)ts;
    double a = number(o, "altitude_m", DASH_FLIGHT_UNKNOWN);
    f->altitude_m = a >= -2000 && a <= 60000 ? (int32_t)a : DASH_FLIGHT_UNKNOWN;
    double b = number(o, "bearing", -1), h = number(o, "heading", -1);
    f->bearing = b >= 0 && b < 360 ? (int16_t)b : -1;
    f->heading = h >= 0 && h < 360 ? (int16_t)h : -1;
    f->eligible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "eligible"));
#define COPY(k) string(o, #k, f->k, sizeof(f->k))
    COPY(callsign); COPY(registration); COPY(model); COPY(flight); COPY(airline);
    COPY(origin); COPY(destination); COPY(origin_city); COPY(destination_city);
#undef COPY
    return true;
}

bool dash_flights_parse(dash_flights_t *st, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "flights");
    if (!o) o = root;
    const cJSON *configured = cJSON_GetObjectItemCaseSensitive(o, "configured");
    if (!cJSON_IsObject(o) || number(o, "schema", 0) != 1 || !cJSON_IsBool(configured)) {
        cJSON_Delete(root);
        return false;
    }
    static dash_flights_t next; /* avoid a 3 KiB temporary on the firmware fetch stack */
    memset(&next, 0, sizeof(next));
    next.have = true;
    next.configured = cJSON_IsTrue(configured);
    next.ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ok"));
    double ts = number(o, "fetched_at", 0);
    if (ts < 0 || ts > 4102444800.0) { cJSON_Delete(root); return false; }
    next.fetched_at = (int64_t)ts;
    double radius = number(o, "radius_m", 20000), alert = number(o, "alert_radius_m", 3000);
    if (radius < 1000 || radius > 100000 || alert < 1000 || alert > radius) {
        cJSON_Delete(root); return false;
    }
    next.radius_m = (int32_t)radius;
    next.alert_radius_m = (int32_t)alert;
    string(o, "place", next.place, sizeof(next.place));
    const cJSON *v;
    if (next.configured) {
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(o, "aircraft")) {
            if (next.count == DASH_MAX_FLIGHTS) break;
            if (parse_aircraft(v, &next.aircraft[next.count])) ++next.count;
        }
        cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(o, "events")) {
            if (next.event_count == DASH_MAX_FLYOVERS) break;
            dash_flyover_t *e = &next.events[next.event_count];
            double id = number(v, "id", 0), at = number(v, "at", 0);
            if (id <= 0 || id > 9007199254740991.0 || at <= 0 || at > 4102444800.0 ||
                !parse_aircraft(cJSON_GetObjectItemCaseSensitive(v, "flight"), &e->flight)) continue;
            e->id = (int64_t)id;
            e->at = (int64_t)at;
            ++next.event_count;
        }
    }
    *st = next;
    cJSON_Delete(root);
    return true;
}

dash_flight_status_t dash_flights_status(const dash_flights_t *st, int64_t now)
{
    if (!st->have) return st->fetch_failures ? DASH_FLIGHT_ERROR : DASH_FLIGHT_WAIT;
    if (!st->configured) return DASH_FLIGHT_DISABLED;
    if (st->fetch_failures || (!st->ok && st->fetched_at)) return DASH_FLIGHT_ERROR;
    if (!st->fetched_at) return st->ok ? DASH_FLIGHT_WAIT : DASH_FLIGHT_ERROR;
    if (now - st->fetched_at > 30 || now - st->fetched_at < -5) return DASH_FLIGHT_STALE;
    return DASH_FLIGHT_LIVE;
}

int dash_flights_take_new(dash_flight_cursor_t *c, const dash_flights_t *st, int64_t now,
                          dash_flyover_t *out, int cap)
{
    if (!st->have || !st->configured || !st->fetched_at) return 0;
    int64_t max = c->last_id;
    for (int i = 0; i < st->event_count; ++i) if (st->events[i].id > max) max = st->events[i].id;
    int n = 0;
    if (c->baseline && dash_flights_status(st, now) == DASH_FLIGHT_LIVE) {
        for (int i = 0; i < st->event_count && n < cap; ++i) {
            const dash_flyover_t *e = &st->events[i];
            if (e->id > c->last_id && now-e->at >= -5 && now-e->at <= 60) out[n++] = *e;
        }
        /* Endpoint order is not trusted; replay oldest first. */
        for (int i = 1; i < n; ++i) {
            dash_flyover_t e = out[i];
            int j = i;
            while (j && out[j-1].id > e.id) { out[j] = out[j-1]; --j; }
            out[j] = e;
        }
    }
    c->baseline = true;
    c->last_id = max;
    return n;
}
