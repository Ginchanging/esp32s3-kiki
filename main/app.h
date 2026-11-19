/* Shared firmware state and small helpers. Only the UI task touches LVGL. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dash_model.h"
#include "dash_actions.h"
#include "dash_sit.h"
#include "esp_err.h"

/* --- shared dashboard state (guarded by a mutex) --- */
void app_state_init(void);
dash_state_t *app_lock(void); /* returns the state; call app_unlock() soon after */
void app_unlock(bool changed);
bool app_take_dirty(void);

/* LED timers live next to the state (same lock). */
dash_led_timers_t *app_led_timers_locked(void);

/* A plain-text Muse or iPhone alert arrived. */
void app_text_alert_arrived(dash_msg_src_t src, const char *utf8, int64_t ts);
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

void app_request_calendar(int32_t day, uint16_t offset);
/* Always returns the latest query; true means an immediate request is pending. */
bool app_take_calendar_request(int32_t *day, uint16_t *offset, uint32_t *seq);
bool app_calendar_current_locked(uint32_t seq); /* state mutex must be held */

typedef struct {
    uint32_t seq;
    bool ok;
    dash_action_page_t page;
} app_action_reply_t;

uint32_t app_request_action(const dash_action_query_t *query); /* NULL cancels */
bool app_take_action_request(dash_action_query_t *query, uint32_t *seq);
void app_finish_action_request(uint32_t seq, const char *json, size_t len);
bool app_take_action_reply(app_action_reply_t *reply);

/* Rotary knob events (dash_knob_ev_t) from the knob task or the KNOB serial command;
 * the UI task takes them. */
void app_knob_push(int ev, int steps);
bool app_knob_take(int *ev, int *steps, uint32_t *at_ms);
void app_knob_set_reverse(bool rev); /* NVS dash/knob_rev, loaded by the knob task */

/* "CAT ..." test command for Kiki, applied by the UI task */
void app_cat_request(const char *args);
bool app_take_cat_request(char *out, size_t n);

/* Wi-Fi modem-sleep (NVS dash/wifi_ps, default off: its current pulses made the 3V3 backlight flicker) */
void app_wifi_ps_apply(void); /* reads NVS and applies; also called on every new connection */
bool app_wifi_ps_on(void);

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
/* The fetch task owns all HTTP requests, including OTA streaming. */
int app_hub_json(const char *url, const char *token, const char *post, char *out, int cap, int *len);
void app_ota_init(void);
void app_ota_poll(const char *token, char *scratch, int capacity);
void app_ota_hub_result(int status, bool parsed);
void app_ota_ui_startup(const char *stage); /* pass a string literal */
void app_ota_ui_tick(void);
void app_ota_ui_presented(uint8_t number);
void app_ota_print_status(const char *prefix);
void muse_task(void *arg);
void led_task(void *arg);
void knob_task(void *arg);
void sensor_task(void *arg);
void app_sensor_print_status(const char *prefix);
void app_led_print_status(const char *prefix);
void sit_task(void *arg);
esp_err_t app_sit_set_enabled(bool enabled);
esp_err_t app_sit_set_range(unsigned cm);
void app_sit_get_status(dash_sit_status_t *status);
void app_sit_print_status(const char *prefix);
void app_sit_request_demo(void);
bool app_sit_take_reminder(bool *demo);

/* Hub endpoint; the certificate is pinned (main/certs/hub.pem). */
#define DASH_HUB_BASE "https://8.138.138.222:8443"
#define DASH_HUB_STATUS_URL DASH_HUB_BASE "/api/device/status"
#define DASH_HUB_FLIGHTS_URL DASH_HUB_BASE "/api/device/flights"
#define DASH_HUB_MI_URL DASH_HUB_BASE "/api/device/mi" /* Mi Home control (POST) */
#define DASH_HUB_CALENDAR_URL DASH_HUB_BASE "/api/device/calendar"
#define DASH_HUB_ACTIONS_URL DASH_HUB_BASE "/api/device/actions"
#define DASH_HUB_CN "esp-dash.local"
#define DASH_FETCH_PERIOD_MS 30000
#define DASH_FLIGHT_PERIOD_MS 10000

#define MUSE_MQTT_URI "mqtts://bemfa.com:9503" /* TLS 1.2; 9501 would send the UID in clear */
#define MUSE_TOPIC "musealert"
#define AGENT_TOPIC "agentalert" /* github.com/Ginchanging/agent-bemfa-notify */
#define IPHONE_TOPIC "iPhone"

#define LED_DEFAULT_GPIO 48
/* KY-016 common cathode: photo order B / G / R / -, with - connected to GND. */
#define RGB_GPIO_R 4
#define RGB_GPIO_G 5
#define RGB_GPIO_B 6

/* Confirmed wiring 2026-11-16: separate I2C bus; touch stays on GPIO15/16. */
#define SENSOR_GPIO_SDA 13
#define SENSOR_GPIO_SCL 14

/* 2.8" ILI9341V screen (SPI2) + FT6336G touch (I2C), on the 3V3/5Vin header; see README */
#define LCD_GPIO_SCK 12
#define LCD_GPIO_MOSI 11
#define LCD_GPIO_CS 10
#define LCD_GPIO_DC 9
#define LCD_GPIO_RST 8
#define LCD_GPIO_BL 7
#define TP_GPIO_SDA 15
#define TP_GPIO_SCL 16
#define TP_GPIO_INT 17
#define TP_GPIO_RST 18
#define LCD_BUF_LINES 32 /* two DMA buffers = 30 KiB; keep headroom for network allocations */
#define LCD_DEFAULT_MHZ 40
#define LCD_MIN_MHZ 5
#define LCD_MAX_MHZ 80

/* NVS dash/lcd_flags: per-module fixes, toggled with ./esp.sh lcd <name> */
#define LCD_F_INV (1u << 0) /* colour inversion (IPS panels) */
#define LCD_F_RGB (1u << 1) /* RGB instead of BGR (red and blue swapped) */
#define LCD_F_MX (1u << 2)  /* mirror the picture left-right */
#define LCD_F_MY (1u << 3)  /* mirror the picture top-bottom */
#define LCD_F_TMX (1u << 4) /* mirror touch x */
#define LCD_F_TMY (1u << 5) /* mirror touch y */
#define LCD_F_TXY (1u << 6) /* swap touch x and y */
#define LCD_FLAG_NAMES                                                                  \
    {"inv", LCD_F_INV}, {"rgb", LCD_F_RGB}, {"mx", LCD_F_MX}, {"my", LCD_F_MY},         \
        {"tmx", LCD_F_TMX}, {"tmy", LCD_F_TMY}, {"txy", LCD_F_TXY}

/* Screen driver (app_lcd.c). UI task only, except app_lcd_request_reload(). */
bool app_lcd_init(void);                                         /* false = stay headless */
bool app_lcd_draw(int x1, int y1, int x2, int y2, uint16_t *px); /* byte-swaps px; true = DMA queued */
void app_lcd_wait(void);                                         /* until the queued DMA is done */
bool app_lcd_touch(int *x, int *y);                              /* true while a finger is down */
void app_lcd_backlight_on(void);                                 /* after the first frame */
void app_lcd_request_reload(void);                               /* settings changed (LCD command) */
bool app_lcd_poll(void);                                         /* applies a reload; true = redraw */
void app_lcd_print_status(const char *prefix);

/* EC11 rotary knob (right header, next to GND) */
#define KNOB_GPIO_A 1   /* CLK */
#define KNOB_GPIO_B 2   /* DT */
#define KNOB_GPIO_SW 42 /* push button, active low */
