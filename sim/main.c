/*
 * macOS simulator for the ESP32-S3 desk dashboard.
 *
 * Interactive:  dash_sim [fixtures/ok.json] [--lang zh|en]
 *   mouse = touch (tap Kiki = meow, long-press Kiki = pomodoro), drag to swipe pages
 *   1..9  load the Nth fixture in the same folder      <- / ->  previous / next page (wraps)
 *   s / f / x  CI passed / failed / started            d / u   server down / back up
 *   m     Muse alert                                    p       pomodoro start/pause
 *   t     routine clock +1 h      c  next activity      0       automatic activity/time
 *   n     night on/off            l  中文 / English     r       reload the fixture
 *   a / b / e  AI agent push: done / needs you / error (agent-bemfa-notify)
 *   w     Mi Home: the washer finishes            v     flyover demo (labelled)
 *   y / Y Kiki's diary / weekly report (normally written at 23:00, weekly on Sundays)
 *   [ / ] knob: turn (pages)    { / }  knob: press + turn (lamp brightness)
 *   k / K knob: click (lamp on/off) / long press (pomodoro)
 *   q     quit
 *   A second small window shows the virtual status LED.
 *
 * Headless screenshot (no window):
 *   dash_sim --shot out.bmp --fixture f.json [--page N] [--lang zh|en] [--now EPOCH]
 *            [--muse "text"] [--event] [--scale 2] [--act N] [--night] [--react N]
 *            [--look name] [--ms N]   (--ms: let animations run N ms before the shot)
 *            [--mi-done]              (Mi Home washer-done message + popup)
 *            [--diary day|week]       (Kiki's diary + popup)
 *            [--knob click|long|turn:N|hold:N]   (rotary knob event, e.g. the brightness toast)
 *
 * Fixtures are hub JSON (/api/device/status) plus an optional "_sim" object
 * describing device-side state (Wi-Fi, MQTT, sensor, Muse history, now).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "dash_model.h"
#include "dash_ui.h"
#include "lvgl.h"

#if DASH_LV_SIM
#include LV_SDL_INCLUDE_PATH
#endif

static dash_state_t state;
static dash_event_cursor_t cursor;
static dash_led_timers_t led_timers;
static int64_t fixture_now;      /* "now" from the fixture, 0 = real clock */
static int64_t fixture_loaded_ms; /* wall ms when the fixture was loaded */
static cJSON *calendar_fixture;
static int64_t calendar_shot_now;
static bool calendar_update_queued;
static const char *ota_demo;

static void apply_ota_demo(void)
{
    if (!ota_demo) return;
    state.ota.total = 100;
    state.ota.received = 42;
    if (!strncmp(ota_demo, "countdown:", 10)) {
        state.ota.phase = DASH_OTA_COUNTDOWN;
        state.ota.countdown = (uint8_t)atoi(ota_demo + 10);
    } else if (!strcmp(ota_demo, "success")) state.ota.phase = DASH_OTA_SUCCEEDED;
    else if (!strcmp(ota_demo, "failed")) state.ota.phase = DASH_OTA_FAILED;
    else if (!strcmp(ota_demo, "rollback")) state.ota.phase = DASH_OTA_ROLLED_BACK;
    else if (!strcmp(ota_demo, "validating")) state.ota.phase = DASH_OTA_VALIDATING;
    else if (!strcmp(ota_demo, "verifying")) state.ota.phase = DASH_OTA_VERIFYING;
    else state.ota.phase = DASH_OTA_DOWNLOADING;
}

static char fixture_paths[16][512];
static int fixture_count;
static int fixture_index = -1;

/* "_sim": {"pomo_week": [[days before now, count, minutes], ...]} -> the UI's pomodoro ring */
static void apply_pomo_week(const cJSON *sim, int64_t now)
{
    dash_pomo_day_t week[DASH_DAYS];
    memset(week, 0, sizeof(week));
    const cJSON *e;
    int32_t today = dash_local_day(now);
    int today_n = 0, today_min = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(sim, "pomo_week"))
    {
        int back = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(e, 0));
        int n = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(e, 1));
        int m = (int)cJSON_GetNumberValue(cJSON_GetArrayItem(e, 2));
        dash_pomo_week_put(week, today - back, n, m);
        if (back == 0) {
            today_n = n, today_min = m;
        }
    }
    dash_ui_pomo_week_restore(week);
    /* the UI's own day key for "today" (see local_yday in dash_ui.c) */
    time_t t = (time_t)(now + DASH_TZ_OFFSET_S);
    struct tm tm;
    gmtime_r(&t, &tm);
    dash_ui_pomo_restore(today_n, today_min, tm.tm_year * 400 + tm.tm_yday);
}

static int64_t wall_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t sim_now(void)
{
    if (fixture_now) {
        return fixture_now + (wall_ms() - fixture_loaded_ms) / 1000;
    }
    return (int64_t)time(NULL);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = '\0';
        *len = (size_t)n;
    }
    return buf;
}

/* Device-side state that the hub does not send. */
static void apply_sim_section(const char *json, size_t len)
{
    state.net = DASH_NET_ONLINE;
    snprintf(state.ip, sizeof(state.ip), "10.12.34.56");
    state.rssi = -58;
    state.clock_valid = true;
    state.mqtt_configured = true;
    state.mqtt_connected = true;
    state.sensor_valid = false;
    state.fetch_failures = 0;
    state.muse_count = 0;
    fixture_now = 0;

    cJSON *root = cJSON_ParseWithLength(json, len);
    const cJSON *sim = cJSON_GetObjectItemCaseSensitive(root, "_sim");
    if (cJSON_IsObject(sim)) {
        const cJSON *v;
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "now")) && cJSON_IsNumber(v)) {
            fixture_now = (int64_t)v->valuedouble;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "net")) && cJSON_IsString(v)) {
            state.net = strcmp(v->valuestring, "online") == 0       ? DASH_NET_ONLINE
                        : strcmp(v->valuestring, "connecting") == 0 ? DASH_NET_CONNECTING
                        : strcmp(v->valuestring, "down") == 0       ? DASH_NET_DOWN
                                                                    : DASH_NET_BOOTING;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "fetch_failures")) && cJSON_IsNumber(v)) {
            state.fetch_failures = v->valueint;
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "mqtt_configured")) && cJSON_IsBool(v)) {
            state.mqtt_configured = cJSON_IsTrue(v);
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "mqtt_connected")) && cJSON_IsBool(v)) {
            state.mqtt_connected = cJSON_IsTrue(v);
        }
        if ((v = cJSON_GetObjectItemCaseSensitive(sim, "clock_valid")) && cJSON_IsBool(v)) {
            state.clock_valid = cJSON_IsTrue(v);
        }
        const cJSON *sensor = cJSON_GetObjectItemCaseSensitive(sim, "sensor");
        if (cJSON_IsObject(sensor)) {
            state.sensor_valid = true;
            state.temp_c = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(sensor, "t"));
            state.humidity = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(sensor, "h"));
        }
        apply_pomo_week(sim, fixture_now ? fixture_now : (int64_t)time(NULL));
        const cJSON *muse = cJSON_GetObjectItemCaseSensitive(sim, "muse");
        int n = cJSON_GetArraySize(muse);
        for (int i = n - 1; i >= 0; --i) { /* listed newest first; push oldest first */
            const cJSON *m = cJSON_GetArrayItem(muse, i);
            const cJSON *text = cJSON_GetObjectItem(m, "text");
            if (cJSON_IsString(text)) {
                dash_muse_push(&state, text->valuestring,
                               (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(m, "ts")));
            }
        }
    }
    cJSON_Delete(root);
}

static bool load_fixture(const char *path)
{
    size_t len = 0;
    char *json = read_file(path, &len);
    if (!json) {
        fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    dash_state_t fresh;
    dash_state_init(&fresh);
    state = fresh;
    if (strstr(json, "\"schema\"")) {
        if (!dash_model_parse(&state, json, len)) {
            fprintf(stderr, "parse failed: %s\n", path);
        }
    }
    apply_sim_section(json, len);
    dash_flights_parse(&state.flights, json, len);
    cJSON_Delete(calendar_fixture);
    cJSON *calendar_root = cJSON_ParseWithLength(json, len);
    calendar_fixture = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(calendar_root, "calendar"), true);
    cJSON_Delete(calendar_root);
    dash_calendar_parse(&state.calendar, json, len);
    free(json);
    fixture_loaded_ms = wall_ms();
    printf("fixture: %s  runs=%d servers=%d events=%d muse=%d\n", path, state.run_count,
           state.server_count, state.event_count, state.muse_count);
    return true;
}

static void calendar_update_async(void *unused)
{
    calendar_update_queued = false;
    dash_ui_update(&state, calendar_shot_now ? calendar_shot_now : sim_now());
}

static void calendar_number(cJSON *root, const char *name, double value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(root, name);
    cJSON_AddNumberToObject(root, name, value);
}

static void calendar_request(int32_t day, uint16_t offset)
{
    int64_t now = calendar_shot_now ? calendar_shot_now : sim_now();
    int32_t today = dash_local_day(now);
    cJSON *reply = calendar_fixture ? cJSON_Duplicate(calendar_fixture, true) : cJSON_CreateObject();
    if (!calendar_fixture) {
        cJSON_AddNumberToObject(reply, "schema", 1);
        cJSON_AddBoolToObject(reply, "configured", false);
        cJSON_AddBoolToObject(reply, "ok", false);
        cJSON_AddNumberToObject(reply, "fetched_at", 0);
    }
    const cJSON *all = cJSON_GetObjectItemCaseSensitive(reply, "all_events");
    if (!all) all = cJSON_GetObjectItemCaseSensitive(calendar_fixture, "events");
    cJSON *events = cJSON_CreateArray(), *counts = cJSON_CreateArray();
    int total = 0;
    for (int i = 0; i < 7; ++i) {
        int n = 0;
        int64_t lo = (int64_t)(today+i)*86400-DASH_TZ_OFFSET_S;
        const cJSON *event;
        cJSON_ArrayForEach(event, all) {
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(event, "start");
            const cJSON *b = cJSON_GetObjectItemCaseSensitive(event, "end");
            if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b) || a->valuedouble >= lo+86400 || b->valuedouble <= lo) continue;
            ++n;
            if (today+i == day) {
                if (total >= offset && total < offset+DASH_CALENDAR_BATCH)
                    cJSON_AddItemToArray(events, cJSON_Duplicate(event, true));
                ++total;
            }
        }
        cJSON_AddItemToArray(counts, cJSON_CreateNumber(n));
    }
    cJSON_DeleteItemFromObjectCaseSensitive(reply, "events");
    cJSON_DeleteItemFromObjectCaseSensitive(reply, "all_events");
    cJSON_DeleteItemFromObjectCaseSensitive(reply, "counts");
    cJSON_AddItemToObject(reply, "events", events);
    cJSON_AddItemToObject(reply, "counts", counts);
    calendar_number(reply, "day", day);
    calendar_number(reply, "week_start", today);
    calendar_number(reply, "offset", offset);
    calendar_number(reply, "total", total);
    char *json = cJSON_PrintUnformatted(reply);
    if (json) {
        dash_calendar_parse(&state.calendar, json, strlen(json));
        cJSON_free(json);
    }
    cJSON_Delete(reply);
    printf("calendar query day=%ld offset=%u total=%d\n", (long)day, (unsigned)offset, total);
    if (!calendar_update_queued) {
        calendar_update_queued = true;
        lv_async_call(calendar_update_async, NULL);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan_fixtures(const char *any_path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", any_path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) && fixture_count < 16) {
        size_t n = strlen(e->d_name);
        if (n > 5 && strcmp(e->d_name + n - 5, ".json") == 0) {
            snprintf(fixture_paths[fixture_count++], 512, "%s/%s", dir, e->d_name);
        }
    }
    closedir(d);
    qsort(fixture_paths, (size_t)fixture_count, sizeof(fixture_paths[0]), cmp_str);
    for (int i = 0; i < fixture_count; ++i) {
        if (strcmp(fixture_paths[i], any_path) == 0) {
            fixture_index = i;
        }
    }
}

static const char *const muse_samples[] = {
    "该去吃午饭了，下午两点还有课。",
    "提醒：明天上午九点交实验报告。",
    "服务器磁盘快满了，记得清理日志。",
    "今晚十点前把论文初稿发给老师。",
};

/* Mi Home control: pretend the hub answered after 700 ms. */
static char mi_req_dev[16];
static int mi_req_on, mi_req_bri;

static void mi_apply(lv_timer_t *t)
{
    for (int i = 0; i < state.mi_count; ++i) {
        dash_mi_dev_t *d = &state.mi[i];
        if (!strcmp(d->id, mi_req_dev)) {
            if (mi_req_on >= 0) {
                d->on = mi_req_on != 0;
            }
            if (mi_req_bri > 0) {
                d->bri = (int8_t)mi_req_bri;
            }
        }
    }
    dash_ui_update(&state, sim_now());
}

static void mi_request(const char *dev, int on, int bri)
{
    snprintf(mi_req_dev, sizeof(mi_req_dev), "%s", dev);
    mi_req_on = on, mi_req_bri = bri;
    lv_timer_t *t = lv_timer_create(mi_apply, 700, NULL);
    lv_timer_set_repeat_count(t, 1);
    printf("mi: %s on=%d bri=%d\n", dev, on, bri);
}

/* ------------------------------------------------------------- headless shot */

static uint16_t shot_fb[DASH_UI_W * DASH_UI_H];
static uint32_t fake_ms;

static uint32_t fake_tick(void)
{
    return fake_ms;
}

static void shot_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    lv_display_flush_ready(disp);
}

/* scripted finger for --swipe tests */
static int fake_x, fake_y;
static bool fake_pressed;

static void fake_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    data->point.x = fake_x;
    data->point.y = fake_y;
    data->state = fake_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void run_for(int ms)
{
    for (int t = 0; t < ms; t += 10) {
        fake_ms += 10;
        lv_timer_handler();
    }
}

/* drag from (x0,y0) to (x1,y1) in 12 steps of 30 ms, then let the snap animation finish */
static void swipe(int x0, int y0, int x1, int y1)
{
    fake_x = x0, fake_y = y0, fake_pressed = true;
    run_for(60);
    for (int i = 1; i <= 12; ++i) {
        fake_x = x0 + (x1 - x0) * i / 12;
        fake_y = y0 + (y1 - y0) * i / 12;
        run_for(30);
    }
    fake_pressed = false;
    run_for(800);
}

/* n quick taps at (x, y): 60 ms down, 120 ms up */
static void taps(int x, int y, int n)
{
    fake_x = x, fake_y = y;
    for (int i = 0; i < n; ++i) {
        fake_pressed = true;
        run_for(60);
        fake_pressed = false;
        run_for(120);
    }
    run_for(800);
}

static bool write_bmp(const char *path, int scale)
{
    int w = DASH_UI_W * scale, h = DASH_UI_H * scale;
    int row = (w * 3 + 3) & ~3;
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    uint32_t size = 54u + (uint32_t)(row * h);
    uint8_t hdr[54] = {'B', 'M'};
    #define PUT32(o, v) do { hdr[o] = (uint8_t)(v); hdr[o + 1] = (uint8_t)((v) >> 8); \
        hdr[o + 2] = (uint8_t)((v) >> 16); hdr[o + 3] = (uint8_t)((v) >> 24); } while (0)
    PUT32(2, size);
    PUT32(10, 54);
    PUT32(14, 40);
    PUT32(18, (uint32_t)w);
    PUT32(22, (uint32_t)h);
    hdr[26] = 1;
    hdr[28] = 24;
    PUT32(34, (uint32_t)(row * h));
    fwrite(hdr, 1, sizeof(hdr), f);
    uint8_t *line = calloc(1, (size_t)row);
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            uint16_t p = shot_fb[(y / scale) * DASH_UI_W + (x / scale)];
            uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
            line[x * 3 + 0] = b;
            line[x * 3 + 1] = g;
            line[x * 3 + 2] = r;
        }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    fclose(f);
    return true;
}

static int run_shot(const char *out, const char *fixture, int page, dash_lang_t lang,
                    int64_t now_arg, const char *muse, bool event, int scale, int act, bool night,
                    int react, const char *look, int run_ms, int agent, const char *swipes, bool mi_done,
                    int diary, const char *knob, bool flyover)
{
    lv_init();
    lv_tick_set_cb(fake_tick);
    lv_display_t *disp = lv_display_create(DASH_UI_W, DASH_UI_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, shot_fb, NULL, sizeof(shot_fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, shot_flush);

    dash_state_init(&state);
    if (fixture && !load_fixture(fixture)) {
        return 1;
    }
    apply_ota_demo();
    int64_t now = now_arg ? now_arg : fixture_now ? fixture_now : (int64_t)time(NULL);
    calendar_shot_now = now;
    dash_ui_set_mi_cb(mi_request);
    dash_ui_set_calendar_cb(calendar_request);
    dash_ui_create(disp, lang);
    dash_ui_set_page(page, false);
    if (muse) {
        dash_muse_push(&state, muse, now);
    }
    dash_ui_update(&state, now);
    if (act >= 0) {
        dash_ui_debug_activity(act);
    }
    if (night) {
        dash_ui_debug_night(1);
    }
    if (look) {
        dash_ui_debug_look(look);
    }
    if (muse) {
        dash_ui_show_muse(muse, now, 0);
    } else if (event && state.event_count) {
        dash_ui_on_event(&state.events[state.event_count - 1]);
    }
    if (react > 0) {
        dash_ui_debug_react(react);
    }
    if (agent > 0) {
        static const char *const texts[] = {"", "Kiki 重设计完成，已烧录到板子", "需要你批准运行 idf.py flash",
                                            "rate_limit: 429 Too Many Requests"};
        dash_msg_push(&state, agent == 2 ? DASH_SRC_CODEX : DASH_SRC_CLAUDE, (dash_msg_kind_t)agent, "ESPdev",
                      texts[agent], now);
        dash_ui_update(&state, now);
        dash_ui_show_agent(&state.muse[0], 0);
    }
    if (mi_done) {
        dash_event_t ev = {.id = 1, .kind = DASH_EVK_MI_DONE, .level = DASH_EVT_SUCCESS};
        snprintf(ev.subject, sizeof(ev.subject), "washer");
        dash_muse_msg_t m;
        if (dash_ui_mi_event_msg(&state, &ev, &m)) {
            dash_msg_push(&state, (dash_msg_src_t)m.src, (dash_msg_kind_t)m.kind, m.project, m.text, now);
            dash_ui_update(&state, now);
            dash_ui_show_agent(&state.muse[0], 0);
        }
    }
    if (diary) {
        dash_muse_msg_t m;
        dash_ui_diary_msg(&state, now, diary == 2, &m);
        dash_msg_push(&state, (dash_msg_src_t)m.src, (dash_msg_kind_t)m.kind, m.project, m.text, now);
        dash_ui_update(&state, now);
        dash_ui_show_agent(&state.muse[0], 0);
        printf("diary (%zu bytes): %s\n", strlen(m.text), m.text);
    }
    if (knob) {
        int n = 1;
        const char *colon = strchr(knob, ':');
        if (colon) {
            n = atoi(colon + 1);
        }
        dash_knob_ev_t ev = !strncmp(knob, "click", 5)  ? DASH_KNOB_CLICK
                            : !strncmp(knob, "long", 4) ? DASH_KNOB_LONG
                            : !strncmp(knob, "hold", 4) ? DASH_KNOB_HOLD_TURN
                                                        : DASH_KNOB_TURN;
        dash_ui_knob(ev, n);
    }
    if (flyover) dash_ui_debug_flyover();
    if (swipes) { /* "wait:6500;tap:x,y,n;..." also tests timed, actionable notifications */
        lv_indev_t *finger = lv_indev_create();
        lv_indev_set_type(finger, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(finger, fake_touch_read);
        lv_indev_set_display(finger, disp);
        dash_ui_attach_indev(finger);
        run_for(200);
        const char *q = swipes;
        while (*q) {
            int x0, y0, x1, y1, n, used = 0;
            int before = dash_ui_get_page();
            if (sscanf(q, "wait:%d%n", &n, &used) == 1) {
                run_for(n);
                printf("wait %d ms page=%d\n", n, dash_ui_get_page());
            } else if (!strncmp(q, "muse", 4)) {
                dash_ui_show_muse("Muse priority test", now, 6000);
                used = 4;
            } else if (!strncmp(q, "lang", 4)) {
                dash_ui_set_lang(dash_ui_get_lang() == DASH_LANG_ZH ? DASH_LANG_EN : DASH_LANG_ZH);
                used = 4;
            } else if (sscanf(q, "tap:%d,%d,%d%n", &x0, &y0, &n, &used) == 3) {
                taps(x0, y0, n);
                printf("tap x%d at %d,%d  page %d -> %d\n", n, x0, y0, before, dash_ui_get_page());
            } else if (sscanf(q, "%d,%d,%d,%d%n", &x0, &y0, &x1, &y1, &used) == 4) {
                swipe(x0, y0, x1, y1);
                printf("swipe %d,%d->%d,%d  page %d -> %d\n", x0, y0, x1, y1, before, dash_ui_get_page());
            } else {
                break;
            }
            q += used;
            if (*q == ';') {
                ++q;
            }
        }
    }
    for (int t = 0; t < run_ms + 150; t += 50) { /* let animations and layout settle */
        fake_ms += 50;
        lv_timer_handler();
    }
    lv_refr_now(disp);
    if (!write_bmp(out, scale)) {
        fprintf(stderr, "cannot write %s\n", out);
        return 1;
    }
    printf("wrote %s\n", out);
    return 0;
}

/* ------------------------------------------------------------- interactive */

#if DASH_LV_SIM
static lv_obj_t *led_circle;
static lv_obj_t *led_label;

static void new_events_to_popups(void)
{
    dash_event_t fresh[DASH_MAX_EVENTS];
    int n = dash_events_take_new(&cursor, &state, fresh, DASH_MAX_EVENTS);
    const dash_event_t *last = NULL;
    for (int i = 0; i < n; ++i) {
        dash_muse_msg_t m;
        if (dash_ui_mi_event_msg(&state, &fresh[i], &m)) { /* Mi Home: into the mailbox, like an agent push */
            dash_msg_push(&state, (dash_msg_src_t)m.src, (dash_msg_kind_t)m.kind, m.project, m.text, sim_now());
            led_timers.agent_until_ms = wall_ms() + 5000;
            led_timers.agent_kind = m.kind;
            dash_ui_update(&state, sim_now());
            dash_ui_show_agent(&state.muse[0], 6000);
        } else {
            last = &fresh[i];
        }
    }
    if (last) {
        dash_ui_on_event(last);
        if (last->level == DASH_EVT_SUCCESS) {
            led_timers.success_until_ms = wall_ms() + 5000;
        }
    }
}



static void select_fixture(int i)
{
    if (i < 0 || i >= fixture_count) {
        return;
    }
    fixture_index = i;
    load_fixture(fixture_paths[i]);
    new_events_to_popups();
    dash_ui_update(&state, sim_now());
}

static int64_t next_event_id = 1;

static void inject_event(dash_event_kind_t kind, const char *subject)
{
    if (state.event_count >= DASH_MAX_EVENTS) {
        memmove(&state.events[0], &state.events[1], sizeof(state.events[0]) * (DASH_MAX_EVENTS - 1));
        state.event_count--;
    }
    dash_event_t *ev = &state.events[state.event_count++];
    memset(ev, 0, sizeof(*ev));
    ev->id = (int64_t)wall_ms() * 1000 + next_event_id++;
    ev->kind = kind;
    ev->level = kind == DASH_EVK_RUN_FAILED || kind == DASH_EVK_SERVER_DOWN ? DASH_EVT_ERROR
                : kind == DASH_EVK_RUN_SUCCEEDED || kind == DASH_EVK_SERVER_UP || kind == DASH_EVK_MI_DONE
                    ? DASH_EVT_SUCCESS
                                                                               : DASH_EVT_INFO;
    snprintf(ev->subject, sizeof(ev->subject), "%s", subject);
    new_events_to_popups();
    dash_ui_update(&state, sim_now());
}

static void inject_agent(dash_msg_kind_t kind)
{
    static const char *const texts[] = {"", "Kiki 重设计完成，已烧录到板子", "需要你批准运行 idf.py flash",
                                        "rate_limit: 429 Too Many Requests"};
    int64_t now = sim_now();
    dash_msg_push(&state, kind == DASH_MSG_ATTENTION ? DASH_SRC_CODEX : DASH_SRC_CLAUDE, kind, "ESPdev",
                  texts[kind], now);
    led_timers.agent_until_ms = wall_ms() + 5000;
    led_timers.agent_kind = (uint8_t)kind;
    dash_ui_update(&state, now);
    dash_ui_show_agent(&state.muse[0], 6000);
}

static void write_diary(bool weekly)
{
    int64_t now = sim_now();
    dash_muse_msg_t m;
    dash_ui_diary_msg(&state, now, weekly, &m);
    dash_msg_push(&state, (dash_msg_src_t)m.src, (dash_msg_kind_t)m.kind, m.project, m.text, now);
    dash_ui_update(&state, now);
    dash_ui_show_agent(&state.muse[0], 12000);
    printf("diary: %s\n", m.text);
}

static int sim_activity = -1;
static int64_t sim_offset;
static int sim_night = -1;

static void key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    const char *wf = state.run_count ? state.runs[0].workflow : "ISBFweb";
    char subj[96];
    snprintf(subj, sizeof(subj), "%s #%d", wf, state.run_count ? state.runs[0].run_number + 1 : 1);
    switch (k) { /* knob + diary keys are case sensitive */
    case '[': dash_ui_knob(DASH_KNOB_TURN, -1); return;
    case ']': dash_ui_knob(DASH_KNOB_TURN, 1); return;
    case '{': dash_ui_knob(DASH_KNOB_HOLD_TURN, -1); return;
    case '}': dash_ui_knob(DASH_KNOB_HOLD_TURN, 1); return;
    case 'k': dash_ui_knob(DASH_KNOB_CLICK, 0); return;
    case 'K': dash_ui_knob(DASH_KNOB_LONG, 0); return;
    case 'y': write_diary(false); return;
    case 'Y': write_diary(true); return;
    default: break;
    }
    if (k == 'a' || k == 'A') {
        inject_agent(DASH_MSG_DONE);
        return;
    } else if (k == 'b' || k == 'B') {
        inject_agent(DASH_MSG_ATTENTION);
        return;
    } else if (k == 'e' || k == 'E') {
        inject_agent(DASH_MSG_ERROR);
        return;
    } else if (k == 'w' || k == 'W') {
        for (int i = 0; i < state.mi_count; ++i) {
            if (state.mi[i].kind == DASH_MI_WASHER) {
                state.mi[i].state = DASH_MI_ST_DONE;
                state.mi[i].eta = 0;
                inject_event(DASH_EVK_MI_DONE, state.mi[i].id);
                return;
            }
        }
        return;
    } else if (k == 's' || k == 'S') {
        if (state.run_count) state.runs[0].state = DASH_RUN_SUCCESS;
        inject_event(DASH_EVK_RUN_SUCCEEDED, subj);
        return;
    } else if (k == 'f' || k == 'F') {
        if (state.run_count) state.runs[0].state = DASH_RUN_FAILURE;
        inject_event(DASH_EVK_RUN_FAILED, subj);
        return;
    } else if (k == 'x' || k == 'X') {
        if (state.run_count) state.runs[0].state = DASH_RUN_RUNNING;
        inject_event(DASH_EVK_RUN_STARTED, subj);
        return;
    } else if (k == 'd' || k == 'D') {
        if (state.server_count > 1) { state.servers[1].online = false; state.servers[1].ever_reported = true; }
        inject_event(DASH_EVK_SERVER_DOWN, state.server_count > 1 ? state.servers[1].name : "server");
        return;
    } else if (k == 'u' || k == 'U') {
        if (state.server_count > 1) state.servers[1].online = true;
        inject_event(DASH_EVK_SERVER_UP, state.server_count > 1 ? state.servers[1].name : "server");
        return;
    } else if (k == 'p' || k == 'P') {
        dash_ui_pomo_toggle();
        dash_ui_update(&state, sim_now());
        return;
    } else if (k == 't' || k == 'T') {
        sim_offset += 3600;
        dash_ui_debug_time_offset(sim_offset);
        return;
    } else if (k == 'c' || k == 'C') {
        sim_activity = (sim_activity + 1) % DASH_ACT_COUNT;
        dash_ui_debug_activity(sim_activity);
        return;
    } else if (k == '0') {
        sim_activity = -1, sim_offset = 0, sim_night = -1;
        dash_ui_debug_activity(-1);
        dash_ui_debug_time_offset(0);
        dash_ui_debug_night(-1);
        return;
    } else if (k == 'n' || k == 'N') {
        sim_night = sim_night == 1 ? 0 : 1;
        dash_ui_debug_night(sim_night);
        return;
    }
    if (k == 'v' || k == 'V') {
        dash_ui_debug_flyover();
    } else if (k >= '1' && k <= '9') {
        select_fixture((int)(k - '1'));
    } else if (k == LV_KEY_LEFT) {
        dash_ui_step_page(-1);
    } else if (k == LV_KEY_RIGHT) {
        dash_ui_step_page(1);
    } else if (k == 'm' || k == 'M') {
        static int next;
        const char *text = muse_samples[next++ % 4];
        int64_t now = sim_now();
        dash_muse_push(&state, text, now);
        dash_ui_update(&state, now);
        dash_ui_show_muse(text, now, 6000);
        led_timers.muse_until_ms = wall_ms() + 4800;
    } else if (k == 'l' || k == 'L') {
        dash_ui_set_lang(dash_ui_get_lang() == DASH_LANG_ZH ? DASH_LANG_EN : DASH_LANG_ZH);
    } else if (k == 'r' || k == 'R') {
        select_fixture(fixture_index);
    } else if (k == 'q' || k == 'Q') {
        exit(0);
    }
}

static const char *led_name(dash_led_pattern_t p)
{
    switch (p) {
    case DASH_LED_MUSE_FLASH:
        return "MUSE";
    case DASH_LED_RED:
        return "RED";
    case DASH_LED_YELLOW:
        return "YELLOW";
    case DASH_LED_GREEN_FLASH:
        return "GREEN";
    case DASH_LED_OFFLINE:
        return "OFFLINE";
    case DASH_LED_POMO_FOCUS:
        return "FOCUS";
    case DASH_LED_POMO_BREAK:
        return "BREAK";
    case DASH_LED_AGENT_DONE:
        return "AGENT OK";
    case DASH_LED_AGENT_ATTN:
        return "AGENT ?";
    case DASH_LED_AGENT_ERROR:
        return "AGENT !";
    default:
        return "OFF";
    }
}

static void tick_led(void)
{
    int64_t ms = wall_ms();
    dash_ui_get_status(&state.night, &state.pomo_phase, &state.pomo_percent);
    dash_led_pattern_t p = dash_led_pattern(&state, &led_timers, sim_now(), ms);
    uint8_t r, g, b;
    dash_led_color_ex(p, &led_timers, ms, state.pomo_percent, &r, &g, &b);
    /* scale the dim LED values up so they are visible on a monitor */
    lv_obj_set_style_bg_color(led_circle,
                              lv_color_make((uint8_t)(r * 4 > 255 ? 255 : r * 4),
                                            (uint8_t)(g * 4 > 255 ? 255 : g * 4),
                                            (uint8_t)(b * 4 > 255 ? 255 : b * 4)),
                              0);
    lv_label_set_text(led_label, led_name(p));
}

static uint32_t sdl_tick(void)
{
    return SDL_GetTicks();
}

static int run_interactive(const char *fixture, dash_lang_t lang)
{
    lv_init();
    lv_tick_set_cb(sdl_tick);

    lv_display_t *led_disp = lv_sdl_window_create(90, 90);
    lv_sdl_window_set_title(led_disp, "LED");
    lv_obj_t *led_scr = lv_display_get_screen_active(led_disp);
    lv_obj_set_style_bg_color(led_scr, lv_color_hex(0x0D1117), 0);
    led_circle = lv_obj_create(led_scr);
    lv_obj_remove_style_all(led_circle);
    lv_obj_set_size(led_circle, 46, 46);
    lv_obj_set_style_radius(led_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(led_circle, LV_OPA_COVER, 0);
    lv_obj_align(led_circle, LV_ALIGN_TOP_MID, 0, 8);
    led_label = lv_label_create(led_scr);
    lv_obj_set_style_text_color(led_label, lv_color_hex(0xE6EDF3), 0);
    lv_obj_set_style_text_font(led_label, &lv_font_montserrat_12, 0);
    lv_obj_align(led_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_display_t *disp = lv_sdl_window_create(DASH_UI_W, DASH_UI_H);
    lv_sdl_window_set_zoom(disp, 2);
    lv_sdl_window_set_title(disp, "Kiki (s/f/x CI, d/u server, m Muse, p pomo, t/c/n time, l lang)");
    lv_display_set_default(disp);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_display(mouse, disp);
    lv_indev_t *kb = lv_sdl_keyboard_create();
    lv_indev_set_display(kb, disp);

    dash_state_init(&state);
    if (fixture) {
        scan_fixtures(fixture);
        load_fixture(fixture);
        dash_events_take_new(&cursor, &state, NULL, 0); /* baseline, like the device */
    }
    dash_ui_set_mi_cb(mi_request);
    dash_ui_set_calendar_cb(calendar_request);
    dash_ui_create(disp, lang);
    dash_ui_update(&state, sim_now());

    /* keyboard: an invisible focused object on the top layer receives LV_EVENT_KEY */
    lv_group_t *g = lv_group_create();
    lv_obj_t *sink = lv_obj_create(lv_layer_sys());
    lv_obj_set_size(sink, 1, 1);
    lv_obj_set_style_opa(sink, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(sink, key_cb, LV_EVENT_KEY, NULL);
    lv_group_add_obj(g, sink);
    lv_indev_set_group(kb, g);
    lv_group_focus_obj(sink);

    int64_t next_update = 0;
    for (;;) {
        uint32_t wait = lv_timer_handler();
        int64_t ms = wall_ms();
        if (ms >= next_update) {
            dash_ui_update(&state, sim_now());
            next_update = ms + 1000;
        }
        tick_led();
        usleep((wait > 20 ? 20 : wait) * 1000);
    }
    return 0;
}
#endif

int main(int argc, char **argv)
{
    const char *shot = NULL, *fixture = NULL, *muse = NULL;
    int page = 0, scale = 2;
    int64_t now = 0;
    bool event = false, night = false, flyover = false;
    int act = -1, react = 0, run_ms = 0, agent = 0;
    bool mi_done = false;
    int diary = 0;
    const char *look = NULL, *swipes = NULL, *knob = NULL;
    dash_lang_t lang = DASH_LANG_ZH;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--shot") && v) {
            shot = v, ++i;
        } else if (!strcmp(a, "--fixture") && v) {
            fixture = v, ++i;
        } else if (!strcmp(a, "--page") && v) {
            page = atoi(v), ++i;
        } else if (!strcmp(a, "--lang") && v) {
            lang = strcmp(v, "en") == 0 ? DASH_LANG_EN : DASH_LANG_ZH, ++i;
        } else if (!strcmp(a, "--now") && v) {
            now = atoll(v), ++i;
        } else if (!strcmp(a, "--muse") && v) {
            muse = v, ++i;
        } else if (!strcmp(a, "--scale") && v) {
            scale = atoi(v) < 1 ? 1 : atoi(v), ++i;
        } else if (!strcmp(a, "--event")) {
            event = true;
        } else if (!strcmp(a, "--night")) {
            night = true;
        } else if (!strcmp(a, "--flyover")) {
            flyover = true;
        } else if (!strcmp(a, "--act") && v) {
            act = atoi(v), ++i;
        } else if (!strcmp(a, "--react") && v) {
            react = atoi(v), ++i;
        } else if (!strcmp(a, "--look") && v) {
            look = v, ++i;
        } else if (!strcmp(a, "--agent") && v) {
            agent = !strcmp(v, "done") ? 1 : !strcmp(v, "attention") ? 2 : !strcmp(v, "error") ? 3 : 0, ++i;
        } else if (!strcmp(a, "--mi-done")) {
            mi_done = true;
        } else if (!strcmp(a, "--diary") && v) {
            diary = !strcmp(v, "week") ? 2 : 1, ++i;
        } else if (!strcmp(a, "--knob") && v) {
            knob = v, ++i;
        } else if (!strcmp(a, "--swipe") && v) {
            swipes = v, ++i;
        } else if (!strcmp(a, "--ms") && v) {
            run_ms = atoi(v), ++i;
        } else if (!strcmp(a, "--ota") && v) {
            ota_demo = v, ++i;
        } else if (a[0] != '-') {
            fixture = a;
        } else {
            fprintf(stderr, "unknown option %s\n", a);
            return 2;
        }
    }
    if (shot) {
        return run_shot(shot, fixture, page, lang, now, muse, event, scale, act, night, react, look, run_ms, agent, swipes, mi_done,
                        diary, knob, flyover);
    }
#if DASH_LV_SIM
    return run_interactive(fixture, lang);
#else
    fprintf(stderr, "built without SDL\n");
    return 2;
#endif
}
