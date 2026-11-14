#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "campus_net.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"

static SemaphoreHandle_t lock;
static dash_state_t state;
static dash_led_timers_t led_timers;
static volatile bool dirty = true;
static volatile bool lang_pending;
static volatile dash_lang_t lang_requested;
static volatile bool demo;
static volatile bool token_changed, bemfa_changed, led_changed;

typedef struct {
    int16_t ev, steps;
} knob_msg_t;
static QueueHandle_t knob_q; /* two producers (knob task, serial command), one consumer (UI) */

void app_state_init(void)
{
    lock = xSemaphoreCreateMutex();
    knob_q = xQueueCreate(16, sizeof(knob_msg_t));
    dash_state_init(&state);
}

void app_knob_push(int ev, int steps)
{
    knob_msg_t m = {.ev = (int16_t)ev, .steps = (int16_t)steps};
    if (knob_q) {
        xQueueSend(knob_q, &m, 0); /* full: drop */
    }
}

bool app_knob_take(int *ev, int *steps)
{
    knob_msg_t m;
    if (!knob_q || xQueueReceive(knob_q, &m, 0) != pdTRUE) {
        return false;
    }
    *ev = m.ev;
    *steps = m.steps;
    return true;
}

dash_state_t *app_lock(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    return &state;
}

void app_unlock(bool changed)
{
    if (changed) {
        dirty = true;
    }
    xSemaphoreGive(lock);
}

bool app_take_dirty(void)
{
    bool d = dirty;
    dirty = false;
    return d;
}

dash_led_timers_t *app_led_timers_locked(void)
{
    return &led_timers;
}

void app_muse_arrived(const char *utf8, int64_t ts)
{
    dash_state_t *st = app_lock();
    dash_muse_push(st, utf8, ts);
    led_timers.muse_until_ms = esp_timer_get_time() / 1000 + 4800;
    app_unlock(true);
}

void app_agent_arrived(const dash_muse_msg_t *m, int64_t ts)
{
    dash_state_t *st = app_lock();
    dash_msg_push(st, (dash_msg_src_t)m->src, (dash_msg_kind_t)m->kind, m->project, m->text, ts);
    led_timers.agent_until_ms = esp_timer_get_time() / 1000 + (m->kind == DASH_MSG_DONE ? 5000 : 6000);
    led_timers.agent_kind = m->kind;
    app_unlock(true);
}

void app_request_lang(dash_lang_t lang)
{
    lang_requested = lang;
    lang_pending = true;
}

bool app_take_lang_request(dash_lang_t *out)
{
    if (!lang_pending) {
        return false;
    }
    lang_pending = false;
    *out = lang_requested;
    return true;
}

/* --- demo fixtures (same files the simulator uses) --- */
extern const char fx_ok_start[] asm("_binary_1_ok_json_start");
extern const char fx_ok_end[] asm("_binary_1_ok_json_end");
extern const char fx_failed_start[] asm("_binary_2_gh_failed_json_start");
extern const char fx_failed_end[] asm("_binary_2_gh_failed_json_end");
extern const char fx_running_start[] asm("_binary_3_gh_running_json_start");
extern const char fx_running_end[] asm("_binary_3_gh_running_json_end");
extern const char fx_down_start[] asm("_binary_4_server_down_json_start");
extern const char fx_down_end[] asm("_binary_4_server_down_json_end");

bool app_demo_load(const char *name)
{
    const char *start = NULL, *end = NULL;
    if (!strcmp(name, "ok")) {
        start = fx_ok_start, end = fx_ok_end;
    } else if (!strcmp(name, "gh_failed")) {
        start = fx_failed_start, end = fx_failed_end;
    } else if (!strcmp(name, "gh_running")) {
        start = fx_running_start, end = fx_running_end;
    } else if (!strcmp(name, "server_down")) {
        start = fx_down_start, end = fx_down_end;
    }
    if (!start) {
        return false;
    }
    dash_state_t *st = app_lock();
    bool ok = dash_model_parse(st, start, (size_t)(end - start));
    if (ok) {
        int64_t now = time(NULL);
        st->generated_at = now; /* fixtures carry a fixed date; make them look fresh */
        st->fetch_failures = 0;
        for (int i = 0; i < st->server_count; ++i) {
            if (st->servers[i].ever_reported) {
                st->servers[i].reported_at = now - (st->servers[i].online ? 5 : 420);
            }
        }
        demo = true;
    }
    app_unlock(ok);
    return ok;
}

void app_demo_off(void)
{
    demo = false;
    dash_state_t *st = app_lock();
    st->have_hub = false;
    st->run_count = st->server_count = st->event_count = st->mi_count = 0;
    st->have_mi = false;
    app_unlock(true);
    app_notify_token_changed(); /* wake the fetch task */
}

bool app_demo_active(void)
{
    return demo;
}

static volatile bool mirror_req_pending, mirror_req_on, mirror_on;
/* Touch events queue up so a burst of serial TOUCH commands is replayed in order. */
#define TOUCH_Q 32
static struct {
    int16_t x, y;
    bool pressed;
} touch_q[TOUCH_Q];
static volatile unsigned touch_head, touch_tail;
static int touch_x, touch_y;
static bool touch_pressed;

void app_mirror_request(bool on)
{
    mirror_req_on = on;
    mirror_req_pending = true;
}

bool app_mirror_take_request(bool *on)
{
    if (!mirror_req_pending) {
        return false;
    }
    mirror_req_pending = false;
    *on = mirror_req_on;
    return true;
}

static volatile int64_t fetch_ok_ms;

void app_fetch_ok_now(void)
{
    fetch_ok_ms = esp_timer_get_time() / 1000;
}

int64_t app_fetch_ok_age_ms(void)
{
    return fetch_ok_ms ? esp_timer_get_time() / 1000 - fetch_ok_ms : -1;
}

void app_mirror_set_active(bool on)
{
    mirror_on = on;
}

bool app_mirror_active(void)
{
    return mirror_on;
}

void app_touch_set(int x, int y, bool pressed)
{
    unsigned next = (touch_head + 1) % TOUCH_Q;
    if (next == touch_tail) {
        return; /* full: drop */
    }
    touch_q[touch_head].x = (int16_t)x;
    touch_q[touch_head].y = (int16_t)y;
    touch_q[touch_head].pressed = pressed;
    touch_head = next;
}

bool app_touch_get(int *x, int *y, bool *pressed)
{
    if (touch_tail != touch_head) {
        touch_x = touch_q[touch_tail].x;
        touch_y = touch_q[touch_tail].y;
        touch_pressed = touch_q[touch_tail].pressed;
        touch_tail = (touch_tail + 1) % TOUCH_Q;
    }
    *x = touch_x;
    *y = touch_y;
    *pressed = touch_pressed;
    return touch_tail != touch_head; /* more queued */
}

static char mi_req_dev[16];
static int mi_req_on, mi_req_bri;
static int64_t mi_req_ms;
static volatile bool mi_req_pending;

void app_request_mi(const char *dev, int on, int bri)
{
    app_lock(); /* reuse the state mutex for the small request */
    snprintf(mi_req_dev, sizeof(mi_req_dev), "%s", dev);
    mi_req_on = on;
    mi_req_bri = bri;
    mi_req_ms = esp_timer_get_time() / 1000;
    mi_req_pending = true;
    app_unlock(false);
}

bool app_take_mi_request(char *dev, size_t n, int *on, int *bri)
{
    if (!mi_req_pending) {
        return false;
    }
    app_lock();
    mi_req_pending = false;
    bool fresh = esp_timer_get_time() / 1000 - mi_req_ms < 15000;
    snprintf(dev, n, "%s", mi_req_dev);
    *on = mi_req_on;
    *bri = mi_req_bri;
    app_unlock(false);
    return fresh;
}

static char cat_req[64];
static volatile bool cat_req_pending;

void app_cat_request(const char *args)
{
    app_lock(); /* reuse the state mutex for the small buffer */
    snprintf(cat_req, sizeof(cat_req), "%s", args);
    cat_req_pending = true;
    app_unlock(false);
}

bool app_take_cat_request(char *out, size_t n)
{
    if (!cat_req_pending) {
        return false;
    }
    app_lock();
    snprintf(out, n, "%s", cat_req);
    cat_req_pending = false;
    app_unlock(false);
    return true;
}

#define FLAG_FUNCS(name)                                                                           \
    void app_notify_##name##_changed(void) { name##_changed = true; }                             \
    bool app_take_##name##_changed(void)                                                           \
    {                                                                                              \
        bool v = name##_changed;                                                                   \
        name##_changed = false;                                                                    \
        return v;                                                                                  \
    }
FLAG_FUNCS(token)
FLAG_FUNCS(bemfa)
FLAG_FUNCS(led)

/* --- NVS --- */
static esp_err_t open_ns(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (!campus_net_storage_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    return nvs_open_from_partition(CAMPUS_NVS_PARTITION, ns, mode, h);
}

esp_err_t app_nvs_get_str(const char *ns, const char *key, char *out, size_t n)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_str(h, key, out, &n);
        nvs_close(h);
    }
    if (err != ESP_OK && n) {
        out[0] = '\0';
    }
    return err;
}

esp_err_t app_nvs_set_str(const char *ns, const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t app_nvs_erase(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t app_nvs_get_u32(const char *ns, const char *key, uint32_t *out)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u32(h, key, out);
        nvs_close(h);
    }
    return err;
}

esp_err_t app_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_blob(h, key, out, len);
        nvs_close(h);
    }
    return err;
}

esp_err_t app_nvs_set_blob(const char *ns, const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, key, data, len);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}

esp_err_t app_nvs_set_u32(const char *ns, const char *key, uint32_t value)
{
    nvs_handle_t h;
    esp_err_t err = open_ns(ns, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u32(h, key, value);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    return err;
}
