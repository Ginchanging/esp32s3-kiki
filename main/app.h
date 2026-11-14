/* Shared firmware state and small helpers. Only the UI task touches LVGL. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dash_model.h"
#include "esp_err.h"

/* --- shared dashboard state (guarded by a mutex) --- */
void app_state_init(void);
dash_state_t *app_lock(void); /* returns the state; call app_unlock() soon after */
void app_unlock(bool changed);
bool app_take_dirty(void);

/* LED timers live next to the state (same lock). */
dash_led_timers_t *app_led_timers_locked(void);

/* A Muse alert arrived (from MQTT or the MUSE serial command). */
void app_muse_arrived(const char *utf8, int64_t ts);
/* An AI agent push (agentalert topic or the AGENT serial command). */
void app_agent_arrived(const dash_muse_msg_t *m, int64_t ts);

/* UI language request from the serial command; consumed by the UI task. */
void app_request_lang(dash_lang_t lang);
bool app_take_lang_request(dash_lang_t *out);

/* Demo mode: the fetch task pauses while built-in fixture data is shown. */
bool app_demo_load(const char *name); /* "ok", "gh_failed", "gh_running", "server_down" */
void app_demo_off(void);
bool app_demo_active(void);

/* Wake-ups for tasks after the matching serial command changed settings. */
void app_notify_token_changed(void);
bool app_take_token_changed(void);
void app_notify_bemfa_changed(void);
bool app_take_bemfa_changed(void);
void app_notify_led_changed(void);
bool app_take_led_changed(void);

/* --- live mirror: the board streams its rendered frames over USB serial and
 *     takes touches back from scripts/mirror.py on the Mac --- */
void app_mirror_request(bool on);  /* from the MIRROR command */
bool app_mirror_take_request(bool *on);
void app_mirror_set_active(bool on);
bool app_mirror_active(void);

/* monotonic ms of the last good hub fetch (0 = never); immune to clock corrections */
void app_fetch_ok_now(void);
int64_t app_fetch_ok_age_ms(void);
void app_touch_set(int x, int y, bool pressed);
bool app_touch_get(int *x, int *y, bool *pressed); /* true if more are queued */

/* Mi Home control (tap on the 米家 page or the MI serial command), sent by the fetch task.
 * on = 0/1 or -1 to keep; bri = 1..100 or -1 to keep. Requests older than 15 s are dropped. */
void app_request_mi(const char *dev, int on, int bri);
bool app_take_mi_request(char *dev, size_t n, int *on, int *bri);

/* Rotary knob events (dash_knob_ev_t) from the knob task or the KNOB serial command;
 * the UI task takes them. */
void app_knob_push(int ev, int steps);
bool app_knob_take(int *ev, int *steps);
void app_knob_set_reverse(bool rev); /* NVS dash/knob_rev, loaded by the knob task */

/* "CAT ..." test command for Kiki, applied by the UI task */
void app_cat_request(const char *args);
bool app_take_cat_request(char *out, size_t n);

/* --- NVS in the "campus_nvs" partition (new namespaces only) --- */
esp_err_t app_nvs_get_str(const char *ns, const char *key, char *out, size_t n);
esp_err_t app_nvs_set_str(const char *ns, const char *key, const char *value);
esp_err_t app_nvs_erase(const char *ns, const char *key);
esp_err_t app_nvs_get_u32(const char *ns, const char *key, uint32_t *out);
esp_err_t app_nvs_set_u32(const char *ns, const char *key, uint32_t value);
esp_err_t app_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len);
esp_err_t app_nvs_set_blob(const char *ns, const char *key, const void *data, size_t len);

/* --- modules --- */
bool app_cmd_handle(char *line); /* extra "@sysu" serial commands */
void ui_task(void *arg);
void fetch_task(void *arg);
void muse_task(void *arg);
void led_task(void *arg);
void knob_task(void *arg);

/* Hub endpoint; the certificate is pinned (main/certs/hub.pem). */
#define DASH_HUB_BASE "https://8.138.138.222:8443"
#define DASH_HUB_STATUS_URL DASH_HUB_BASE "/api/device/status"
#define DASH_HUB_MI_URL DASH_HUB_BASE "/api/device/mi" /* Mi Home control (POST) */
#define DASH_HUB_CN "esp-dash.local"
#define DASH_FETCH_PERIOD_MS 30000

#define MUSE_MQTT_URI "mqtts://bemfa.com:9503" /* TLS 1.2; 9501 would send the UID in clear */
#define MUSE_TOPIC "musealert"
#define AGENT_TOPIC "agentalert" /* github.com/Ginchanging/agent-bemfa-notify */

#define LED_DEFAULT_GPIO 48

/* EC11 rotary knob (right header, next to GND) */
#define KNOB_GPIO_A 1   /* CLK */
#define KNOB_GPIO_B 2   /* DT */
#define KNOB_GPIO_SW 42 /* push button, active low */
