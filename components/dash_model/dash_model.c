#include "dash_model.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"

/* Copy at most n-1 bytes without splitting a UTF-8 sequence. */
static void copy_utf8(char *dst, size_t n, const char *src)
{
    if (!n) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            --len; /* back off continuation bytes */
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(item) ? item->valuedouble : fallback;
}

/* number -> int16, DASH_NONE16 when absent or null */
static int16_t json_i16(const cJSON *obj, const char *key)
{
    double v = json_num(obj, key, (double)DASH_NONE16);
    if (v == (double)DASH_NONE16) {
        return DASH_NONE16;
    }
    return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}

static uint16_t json_u16(const cJSON *obj, const char *key)
{
    double v = json_num(obj, key, 0);
    return (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
}

static bool json_bool(const cJSON *obj, const char *key, bool fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

dash_run_state_t dash_run_state(const char *status, const char *conclusion)
{
    if (!status) {
        return DASH_RUN_UNKNOWN;
    }
    if (strcmp(status, "completed") != 0) {
        if (strcmp(status, "in_progress") == 0) {
            return DASH_RUN_RUNNING;
        }
        return DASH_RUN_QUEUED; /* queued, waiting, pending, requested */
    }
    if (!conclusion) {
        return DASH_RUN_UNKNOWN;
    }
    if (strcmp(conclusion, "success") == 0) {
        return DASH_RUN_SUCCESS;
    }
    if (strcmp(conclusion, "failure") == 0 || strcmp(conclusion, "timed_out") == 0 ||
        strcmp(conclusion, "startup_failure") == 0 || strcmp(conclusion, "action_required") == 0) {
        return DASH_RUN_FAILURE;
    }
    if (strcmp(conclusion, "cancelled") == 0) {
        return DASH_RUN_CANCELLED;
    }
    return DASH_RUN_SKIPPED; /* skipped, neutral, stale */
}

static dash_mi_state_t mi_state(const char *s)
{
    static const char *const names[] = {
        [DASH_MI_ST_OTHER] = "other", [DASH_MI_ST_IDLE] = "idle",   [DASH_MI_ST_WASH] = "wash",
        [DASH_MI_ST_RINSE] = "rinse", [DASH_MI_ST_SPIN] = "spin",   [DASH_MI_ST_DRY] = "dry",
        [DASH_MI_ST_DONE] = "done",   [DASH_MI_ST_PAUSE] = "pause", [DASH_MI_ST_FAULT] = "fault",
        [DASH_MI_ST_OFF] = "off",     [DASH_MI_ST_HEAT] = "heat",   [DASH_MI_ST_POUR] = "pour",
        [DASH_MI_ST_CLEAN] = "clean",
    };
    for (size_t i = 0; s && i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(s, names[i])) {
            return (dash_mi_state_t)i;
        }
    }
    return DASH_MI_ST_OTHER;
}

static int mi_kind(const char *kind)
{
    static const char *const names[] = {
        [DASH_MI_LAMP] = "lamp",           [DASH_MI_SWITCH] = "switch", [DASH_MI_WASHER] = "washer",
        [DASH_MI_DISPENSER] = "dispenser", [DASH_MI_SCALE] = "scale",
    };
    for (size_t i = 0; kind && i < sizeof(names) / sizeof(names[0]); ++i) {
        if (!strcmp(kind, names[i])) {
            return (int)i;
        }
    }
    return -1;
}

/* "mihome": {"ok": true, "devices": [{"id","kind","name_zh","name_en","online",
 *             lamp/switch: "on","bri"   washer: "state","eta","pct"
 *             dispenser: "state","temp","tds","filters":[a,b]
 *             scale: "at","d_last","d_30","hist":[30 x 0.1 kg or null]}]}
 * -> st (absent object = Mi Home not configured; unknown kinds are skipped) */
static void parse_mihome(dash_state_t *st, const cJSON *mi)
{
    st->have_mi = cJSON_IsObject(mi);
    st->mi_ok = json_bool(mi, "ok", false);
    st->mi_count = 0;
    for (int i = 0; i < DASH_WEIGHT_DAYS; ++i) {
        st->weight_hist[i] = DASH_NONE16;
    }
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(mi, "devices"))
    {
        if (st->mi_count >= DASH_MAX_MI || !cJSON_IsObject(d)) {
            break;
        }
        int kind = mi_kind(json_str(d, "kind"));
        if (kind < 0 || !json_str(d, "id")) {
            continue;
        }
        dash_mi_dev_t *m = &st->mi[st->mi_count++];
        memset(m, 0, sizeof(*m));
        copy_utf8(m->id, sizeof(m->id), json_str(d, "id"));
        copy_utf8(m->name_zh, sizeof(m->name_zh), json_str(d, "name_zh"));
        copy_utf8(m->name_en, sizeof(m->name_en), json_str(d, "name_en"));
        m->kind = (uint8_t)kind;
        m->state = (uint8_t)mi_state(json_str(d, "state"));
        m->online = json_bool(d, "online", false);
        m->on = json_bool(d, "on", false);
        double bri = json_num(d, "bri", -1), pct = json_num(d, "pct", -1);
        m->bri = (int8_t)(bri < 0 ? -1 : bri > 100 ? 100 : bri);
        m->pct = (int8_t)(pct < 0 ? -1 : pct > 100 ? 100 : pct);
        m->eta = (int64_t)json_num(d, "eta", 0);
        m->temp_c = json_i16(d, "temp");
        m->tds = json_i16(d, "tds");
        const cJSON *filters = cJSON_GetObjectItemCaseSensitive(d, "filters");
        for (int i = 0; i < 2; ++i) {
            const cJSON *f = cJSON_GetArrayItem(filters, i);
            double v = cJSON_IsNumber(f) ? f->valuedouble : -1;
            m->filter_pct[i] = (int8_t)(v < 0 ? -1 : v > 100 ? 100 : v);
        }
        m->weigh_at = (int64_t)json_num(d, "at", 0);
        m->d_last = json_i16(d, "d_last");
        m->d_30 = json_i16(d, "d_30");
        if (kind == DASH_MI_SCALE) {
            const cJSON *hist = cJSON_GetObjectItemCaseSensitive(d, "hist");
            int n = cJSON_GetArraySize(hist);
            int skip = n > DASH_WEIGHT_DAYS ? n - DASH_WEIGHT_DAYS : 0;
            int off = n < DASH_WEIGHT_DAYS ? DASH_WEIGHT_DAYS - n : 0; /* right-aligned: today is last */
            for (int i = skip; i < n; ++i) {
                const cJSON *h = cJSON_GetArrayItem(hist, i);
                double v = cJSON_IsNumber(h) ? h->valuedouble : (double)DASH_NONE16;
                st->weight_hist[off + i - skip] =
                    v == (double)DASH_NONE16 ? DASH_NONE16 : (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
            }
        }
    }
}

/* "days": [{"d":"11-14","wd":6,"ci_ok","ci_fail","wash","down","commits"}], oldest first */
static void parse_days(dash_state_t *st, const cJSON *days)
{
    st->day_count = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, days)
    {
        if (st->day_count >= DASH_DAYS || !cJSON_IsObject(d)) {
            break;
        }
        dash_day_t *x = &st->days[st->day_count++];
        memset(x, 0, sizeof(*x));
        copy_utf8(x->date, sizeof(x->date), json_str(d, "d"));
        x->wday = (uint8_t)(json_u16(d, "wd") % 7);
        x->ci_ok = json_u16(d, "ci_ok");
        x->ci_fail = json_u16(d, "ci_fail");
        x->wash = json_u16(d, "wash");
        x->down = json_u16(d, "down");
        x->commits = json_u16(d, "commits");
    }
}

void dash_state_init(dash_state_t *st)
{
    memset(st, 0, sizeof(*st));
    st->net = DASH_NET_BOOTING;
    dash_health_init(&st->health);
    for (int i = 0; i < DASH_WEIGHT_DAYS; ++i) {
        st->weight_hist[i] = DASH_NONE16;
    }
}

bool dash_model_parse(dash_state_t *st, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root) || json_num(root, "schema", 0) != 1) {
        cJSON_Delete(root);
        return false;
    }

    /* Use the same allocator as the JSON tree (PSRAM on the board). */
    dash_state_t *next = cJSON_malloc(sizeof(*next));
    if (!next) {
        cJSON_Delete(root);
        return false;
    }
    *next = *st;
    next->generated_at = (int64_t)json_num(root, "generated_at", 0);

    const cJSON *gh = cJSON_GetObjectItemCaseSensitive(root, "github");
    next->run_count = 0;
    next->repo[0] = '\0';
    next->github_fetched_at = 0;
    if (cJSON_IsObject(gh)) {
        copy_utf8(next->repo, sizeof(next->repo), json_str(gh, "repo"));
        next->github_fetched_at = (int64_t)json_num(gh, "fetched_at", 0);
        const cJSON *run;
        cJSON_ArrayForEach(run, cJSON_GetObjectItemCaseSensitive(gh, "runs"))
        {
            if (next->run_count >= DASH_MAX_RUNS || !cJSON_IsObject(run)) {
                break;
            }
            dash_run_t *r = &next->runs[next->run_count++];
            memset(r, 0, sizeof(*r));
            double id = json_num(run, "id", 0);
            if (id >= 1 && id <= 9007199254740991.0 && id == (int64_t)id) r->id = (int64_t)id;
            copy_utf8(r->workflow, sizeof(r->workflow), json_str(run, "workflow"));
            copy_utf8(r->branch, sizeof(r->branch), json_str(run, "branch"));
            r->run_number = (int)json_num(run, "run_number", 0);
            double attempt = json_num(run, "attempt", 1);
            r->attempt = attempt >= 1 && attempt <= INT_MAX && attempt == (int)attempt ? (int)attempt : 0;
            r->state = dash_run_state(json_str(run, "status"), json_str(run, "conclusion"));
            r->duration_s = (int)json_num(run, "duration_s", 0);
            r->updated_at = (int64_t)json_num(run, "updated_at", 0);
        }
    }

    next->server_count = 0;
    const cJSON *srv;
    cJSON_ArrayForEach(srv, cJSON_GetObjectItemCaseSensitive(root, "servers"))
    {
        if (next->server_count >= DASH_MAX_SERVERS || !cJSON_IsObject(srv)) {
            break;
        }
        dash_server_t *s = &next->servers[next->server_count++];
        memset(s, 0, sizeof(*s));
        copy_utf8(s->name, sizeof(s->name), json_str(srv, "name"));
        s->reported_at = (int64_t)json_num(srv, "reported_at", 0);
        s->ever_reported = s->reported_at > 0;
        s->online = json_bool(srv, "online", false);
        s->cpu_pct = (float)json_num(srv, "cpu_pct", 0);
        s->mem_pct = (float)json_num(srv, "mem_pct", 0);
        s->disk_pct = (float)json_num(srv, "disk_pct", 0);
        s->load1 = (float)json_num(srv, "load1", 0);
        s->uptime_s = (int64_t)json_num(srv, "uptime_s", 0);
        const cJSON *hv;
        cJSON_ArrayForEach(hv, cJSON_GetObjectItemCaseSensitive(srv, "cpu_hist"))
        {
            if (s->cpu_hist_len >= DASH_CPU_HIST) {
                break;
            }
            int v = cJSON_IsNumber(hv) ? hv->valueint : 0;
            s->cpu_hist[s->cpu_hist_len++] = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
        }
        const cJSON *svc;
        cJSON_ArrayForEach(svc, cJSON_GetObjectItemCaseSensitive(srv, "services"))
        {
            if (s->service_count >= DASH_MAX_SERVICES || !cJSON_IsObject(svc)) {
                break;
            }
            dash_service_t *v = &s->services[s->service_count++];
            copy_utf8(v->name, sizeof(v->name), json_str(svc, "name"));
            v->ok = json_bool(svc, "ok", false);
        }
    }

    next->event_count = 0;
    const cJSON *evt;
    cJSON_ArrayForEach(evt, cJSON_GetObjectItemCaseSensitive(root, "events"))
    {
        if (next->event_count >= DASH_MAX_EVENTS || !cJSON_IsObject(evt)) {
            break;
        }
        dash_event_t *e = &next->events[next->event_count++];
        e->id = (int64_t)json_num(evt, "id", 0);
        const char *kind = json_str(evt, "kind");
        e->kind = !kind                                ? DASH_EVK_OTHER
                  : strcmp(kind, "run_failed") == 0    ? DASH_EVK_RUN_FAILED
                  : strcmp(kind, "run_succeeded") == 0 ? DASH_EVK_RUN_SUCCEEDED
                  : strcmp(kind, "server_down") == 0   ? DASH_EVK_SERVER_DOWN
                  : strcmp(kind, "server_up") == 0     ? DASH_EVK_SERVER_UP
                  : strcmp(kind, "run_started") == 0   ? DASH_EVK_RUN_STARTED
                  : strcmp(kind, "mi_done") == 0       ? DASH_EVK_MI_DONE
                                                       : DASH_EVK_OTHER;
        e->level = e->kind == DASH_EVK_RUN_FAILED || e->kind == DASH_EVK_SERVER_DOWN
                       ? DASH_EVT_ERROR
                   : e->kind == DASH_EVK_RUN_SUCCEEDED || e->kind == DASH_EVK_SERVER_UP ||
                           e->kind == DASH_EVK_MI_DONE
                       ? DASH_EVT_SUCCESS
                       : DASH_EVT_INFO;
        copy_utf8(e->subject, sizeof(e->subject), json_str(evt, "subject"));
    }

    next->have_contrib = false;
    const cJSON *contrib = cJSON_GetObjectItemCaseSensitive(gh, "contrib");
    const char *levels = json_str(contrib, "levels");
    if (levels) {
        size_t n = strlen(levels);
        memset(next->contrib, 0, sizeof(next->contrib));
        /* keep the newest DASH_CONTRIB_DAYS days, right-aligned */
        size_t skip = n > DASH_CONTRIB_DAYS ? n - DASH_CONTRIB_DAYS : 0;
        size_t off = n < DASH_CONTRIB_DAYS ? DASH_CONTRIB_DAYS - n : 0;
        for (size_t i = skip; i < n; ++i) {
            char c = levels[i];
            next->contrib[off + i - skip] = (uint8_t)(c >= '0' && c <= '4' ? c - '0' : 0);
        }
        copy_utf8(next->contrib_user, sizeof(next->contrib_user), json_str(contrib, "user"));
        next->contrib_week = (int)json_num(contrib, "week", 0);
        next->contrib_today = (int)json_num(contrib, "today", 0);
        next->have_contrib = true;
    }

    parse_mihome(next, cJSON_GetObjectItemCaseSensitive(root, "mihome"));
    parse_days(next, cJSON_GetObjectItemCaseSensitive(root, "days"));
    const cJSON *health = cJSON_GetObjectItemCaseSensitive(root, "health");
    if (!health) dash_health_init(&next->health); /* old hub, or a fixture without health */
    else if (!dash_health_read(&next->health, health)) next->health.error = true;

    cJSON_Delete(root);
    next->have_hub = true;
    *st = *next;
    cJSON_free(next);
    return true;
}

bool dash_model_parse_mihome(dash_state_t *st, const char *json, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *mi = cJSON_GetObjectItemCaseSensitive(root, "mihome");
    bool ok = cJSON_IsObject(mi);
    if (ok) {
        parse_mihome(st, mi);
    }
    cJSON_Delete(root);
    return ok;
}

const dash_mi_dev_t *dash_mi_find(const dash_state_t *st, const char *id)
{
    for (int i = 0; i < st->mi_count; ++i) {
        if (!strcmp(st->mi[i].id, id)) {
            return &st->mi[i];
        }
    }
    return NULL;
}

bool dash_mi_running(const dash_mi_dev_t *d)
{
    if (d->kind != DASH_MI_WASHER || !d->online) {
        return false;
    }
    return (d->state >= DASH_MI_ST_WASH && d->state <= DASH_MI_ST_DRY) || d->state == DASH_MI_ST_PAUSE;
}

dash_freshness_t dash_freshness(const dash_state_t *st, int64_t now)
{
    if (st->fetch_failures >= DASH_OFFLINE_AFTER_FAILS || st->net == DASH_NET_DOWN) {
        return st->have_hub || st->net == DASH_NET_DOWN ? DASH_FRESH_OFFLINE : DASH_FRESH_NONE;
    }
    if (!st->have_hub) {
        return DASH_FRESH_NONE;
    }
    if (now - st->generated_at > DASH_STALE_AFTER_S) {
        return DASH_FRESH_STALE;
    }
    return DASH_FRESH_OK;
}

dash_run_state_t dash_latest_run(const dash_state_t *st)
{
    return st->have_hub && st->run_count > 0 ? st->runs[0].state : DASH_RUN_UNKNOWN;
}

int dash_servers_online(const dash_state_t *st)
{
    int n = 0;
    for (int i = 0; i < st->server_count; ++i) {
        n += st->servers[i].online;
    }
    return n;
}

int dash_servers_known(const dash_state_t *st)
{
    return st->server_count;
}

void dash_msg_push(dash_state_t *st, dash_msg_src_t src, dash_msg_kind_t kind, const char *project,
                   const char *utf8, int64_t ts)
{
    memmove(&st->muse[1], &st->muse[0], sizeof(st->muse[0]) * (DASH_MAX_MUSE - 1));
    st->muse[0].ts = ts;
    st->muse[0].src = (uint8_t)src;
    st->muse[0].kind = (uint8_t)kind;
    copy_utf8(st->muse[0].project, sizeof(st->muse[0].project), project ? project : "");
    copy_utf8(st->muse[0].text, sizeof(st->muse[0].text), utf8);
    if (st->muse_count < DASH_MAX_MUSE) {
        st->muse_count++;
    }
    st->muse_seq++;
}

void dash_muse_push(dash_state_t *st, const char *utf8, int64_t ts)
{
    dash_msg_push(st, DASH_SRC_MUSE, DASH_MSG_INFO, "", utf8, ts);
}

bool dash_agent_parse(const char *json, size_t len, dash_muse_msg_t *out)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root) || json_num(root, "v", 0) < 1) {
        cJSON_Delete(root);
        return false;
    }
    memset(out, 0, sizeof(*out));
    const char *s = json_str(root, "s");
    out->src = !s                        ? DASH_SRC_OTHER
               : !strcmp(s, "claude")    ? DASH_SRC_CLAUDE
               : !strcmp(s, "codex")     ? DASH_SRC_CODEX
                                         : DASH_SRC_OTHER;
    const char *k = json_str(root, "k");
    out->kind = !k                         ? DASH_MSG_INFO
                : !strcmp(k, "done")       ? DASH_MSG_DONE
                : !strcmp(k, "attention")  ? DASH_MSG_ATTENTION
                : !strcmp(k, "error")      ? DASH_MSG_ERROR
                                           : DASH_MSG_INFO;
    copy_utf8(out->project, sizeof(out->project), json_str(root, "p"));
    copy_utf8(out->text, sizeof(out->text), json_str(root, "m"));
    cJSON_Delete(root);
    return true;
}

int dash_events_take_new(dash_event_cursor_t *cursor, const dash_state_t *st,
                         dash_event_t *out, int max_out)
{
    if (!st->have_hub) {
        return 0;
    }
    int64_t max_id = cursor->last_id;
    for (int i = 0; i < st->event_count; ++i) {
        if (st->events[i].id > max_id) {
            max_id = st->events[i].id;
        }
    }
    if (!cursor->have_baseline) {
        cursor->have_baseline = true;
        cursor->last_id = max_id;
        return 0;
    }
    int n = 0;
    /* collect new events in ascending id order */
    int64_t floor_id = cursor->last_id;
    while (n < max_out) {
        const dash_event_t *best = NULL;
        for (int i = 0; i < st->event_count; ++i) {
            const dash_event_t *e = &st->events[i];
            if (e->id > floor_id && (!best || e->id < best->id)) {
                best = e;
            }
        }
        if (!best) {
            break;
        }
        out[n++] = *best;
        floor_id = best->id;
    }
    cursor->last_id = max_id;
    return n;
}

dash_led_pattern_t dash_led_pattern(const dash_state_t *st, const dash_led_timers_t *t,
                                    int64_t now_s, int64_t now_ms)
{
    if (st->night) {
        return DASH_LED_OFF; /* dorm at night: never light up */
    }
    if (now_ms < t->muse_until_ms) {
        return DASH_LED_MUSE_FLASH;
    }
    if (now_ms < t->agent_until_ms) {
        return t->agent_kind == DASH_MSG_ERROR       ? DASH_LED_AGENT_ERROR
               : t->agent_kind == DASH_MSG_ATTENTION ? DASH_LED_AGENT_ATTN
                                                     : DASH_LED_AGENT_DONE;
    }
    dash_freshness_t fresh = dash_freshness(st, now_s);
    if (fresh == DASH_FRESH_OFFLINE) {
        return DASH_LED_OFFLINE; /* data can't be trusted; don't show stale red/green */
    }
    if (st->have_hub) {
        dash_run_state_t latest = dash_latest_run(st);
        bool server_down = false;
        for (int i = 0; i < st->server_count; ++i) {
            const dash_server_t *s = &st->servers[i];
            server_down |= s->ever_reported && !s->online;
        }
        if (latest == DASH_RUN_FAILURE || server_down) {
            return DASH_LED_RED;
        }
        if (latest == DASH_RUN_RUNNING || latest == DASH_RUN_QUEUED) {
            return DASH_LED_YELLOW;
        }
    }
    if (now_ms < t->success_until_ms) {
        return DASH_LED_GREEN_FLASH;
    }
    if (st->pomo_phase == DASH_POMO_FOCUS) {
        return DASH_LED_POMO_FOCUS;
    }
    if (st->pomo_phase == DASH_POMO_BREAK) {
        return DASH_LED_POMO_BREAK;
    }
    return DASH_LED_OFF;
}

void dash_led_color(dash_led_pattern_t p, const dash_led_timers_t *t, int64_t now_ms,
                    uint8_t *r, uint8_t *g, uint8_t *b)
{
    dash_led_color_ex(p, t, now_ms, 0, r, g, b);
}

void dash_led_color_ex(dash_led_pattern_t p, const dash_led_timers_t *t, int64_t now_ms,
                       uint8_t pomo_percent, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 0;
    switch (p) {
    case DASH_LED_POMO_FOCUS: /* orange (50,25,0) -> red (55,0,0) */
        *r = (uint8_t)(50 + pomo_percent / 20);
        *g = (uint8_t)(25 - pomo_percent / 4);
        break;
    case DASH_LED_AGENT_DONE:
        if ((now_ms / 400) % 2 == 0) {
            *g = 60;
        }
        break;
    case DASH_LED_AGENT_ATTN:
        if ((now_ms / 220) % 2 == 0) {
            *r = 55;
            *g = 38;
        }
        break;
    case DASH_LED_AGENT_ERROR:
        if ((now_ms / 180) % 2 == 0) {
            *r = 60;
        }
        break;
    case DASH_LED_POMO_BREAK: { /* green, slow breathing */
        int phase = (int)(now_ms % 3000);
        int level = phase < 1500 ? phase : 3000 - phase;
        *g = (uint8_t)(8 + level * 22 / 1500);
        break;
    }
    case DASH_LED_MUSE_FLASH: {
        int64_t elapsed = now_ms - (t->muse_until_ms - 4800);
        bool on = elapsed >= 1800 || ((elapsed / 150) % 2) == 0;
        if (on) {
            *r = *g = *b = 60;
        }
        break;
    }
    case DASH_LED_RED:
        *r = 60;
        break;
    case DASH_LED_YELLOW:
        *r = 50;
        *g = 32;
        break;
    case DASH_LED_GREEN_FLASH:
        if ((now_ms / 400) % 2 == 0) {
            *g = 60;
        }
        break;
    case DASH_LED_OFFLINE:
        if (now_ms % 2000 < 250) {
            *b = 30;
        }
        break;
    case DASH_LED_OFF:
    default:
        break;
    }
}

static void local_tm(int64_t utc, struct tm *out)
{
    time_t t = (time_t)(utc + DASH_TZ_OFFSET_S);
    gmtime_r(&t, out);
}

void dash_fmt_hhmm(int64_t utc, char *buf, size_t n)
{
    struct tm tm;
    local_tm(utc, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

void dash_fmt_hhmmss(int64_t utc, char *buf, size_t n)
{
    struct tm tm;
    local_tm(utc, &tm);
    snprintf(buf, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

void dash_fmt_date(int64_t utc, dash_lang_t lang, char *buf, size_t n)
{
    static const char *const wd_en[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const mon_en[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    static const char *const wd_zh[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    struct tm tm;
    local_tm(utc, &tm);
    if (lang == DASH_LANG_ZH) {
        snprintf(buf, n, "%d月%d日 %s", tm.tm_mon + 1, tm.tm_mday, wd_zh[tm.tm_wday]);
    } else {
        snprintf(buf, n, "%s, %s %d", wd_en[tm.tm_wday], mon_en[tm.tm_mon], tm.tm_mday);
    }
}

void dash_fmt_duration(int64_t s, char *buf, size_t n)
{
    if (s < 0) {
        s = 0;
    }
    if (s < 60) {
        snprintf(buf, n, "%ds", (int)s);
    } else if (s < 3600) {
        snprintf(buf, n, "%dm %02ds", (int)(s / 60), (int)(s % 60));
    } else {
        snprintf(buf, n, "%dh %02dm", (int)(s / 3600), (int)(s % 3600 / 60));
    }
}

void dash_fmt_ago(int64_t s, dash_lang_t lang, char *buf, size_t n)
{
    if (s < 0) {
        s = 0;
    }
    bool zh = lang == DASH_LANG_ZH;
    if (s < 60) {
        snprintf(buf, n, zh ? "%d秒前" : "%ds ago", (int)s);
    } else if (s < 3600) {
        snprintf(buf, n, zh ? "%d分钟前" : "%dm ago", (int)(s / 60));
    } else if (s < 86400) {
        snprintf(buf, n, zh ? "%d小时前" : "%dh ago", (int)(s / 3600));
    } else {
        snprintf(buf, n, zh ? "%d天前" : "%dd ago", (int)(s / 86400));
    }
}

void dash_fmt_uptime(int64_t s, dash_lang_t lang, char *buf, size_t n)
{
    bool zh = lang == DASH_LANG_ZH;
    int days = (int)(s / 86400), hours = (int)(s % 86400 / 3600), mins = (int)(s % 3600 / 60);
    if (days > 0) {
        snprintf(buf, n, zh ? "%d天%d小时" : "%dd %dh", days, hours);
    } else {
        snprintf(buf, n, zh ? "%d小时%d分" : "%dh %dm", hours, mins);
    }
}
