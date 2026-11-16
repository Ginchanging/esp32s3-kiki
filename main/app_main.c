/*
 * ESP32-S3 desk dashboard.
 *   campus task : campus Wi-Fi (PEAP) + "@sysu" serial commands (components/campus_net)
 *   ui task     : LVGL dashboard (components/dash_ui) on the ILI9341 (app_lcd.c)
 *   fetch task  : hub JSON over HTTPS (GitHub Actions + 3 servers)
 *   muse task   : Muse alerts over Bemfa MQTT
 *   led task    : status LED
 *   knob task   : EC11 rotary knob (pages, lamp, pomodoro)
 *   sensor task : SHT3x temperature/humidity on a separate I2C bus (GPIO13/14)
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "campus_net.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * Wi-Fi power save is off by default. In modem-sleep the radio wakes for every
 * beacon, and those current pulses dip the 3V3 rail. The screen runs on 3V3
 * (the board's 5Vin pin is input-only, see README), and its backlight LEDs
 * have little headroom there, so the dips showed up as flicker.
 * NVS dash/wifi_ps = 1 (./esp.sh wifips on) brings modem-sleep back.
 */
static volatile int wifi_ps = -1; /* -1 = not loaded yet */

static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void app_wifi_ps_apply(void)
{
    uint32_t v = 0;
    app_nvs_get_u32("dash", "wifi_ps", &v);
    wifi_ps = v ? 1 : 0;
    esp_wifi_set_ps(wifi_ps ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE); /* NOT_INIT before Wi-Fi starts: fine */
}

bool app_wifi_ps_on(void)
{
    return wifi_ps == 1;
}

static void on_net_status(const campus_net_status_t *s)
{
    static bool ps_applied;
    if (s->online && !ps_applied) { /* once per connection; campus_net restarts Wi-Fi on retries */
        app_wifi_ps_apply();
    }
    ps_applied = s->online;

    dash_state_t *st = app_lock();
    st->net = s->online       ? DASH_NET_ONLINE
              : s->connecting ? DASH_NET_CONNECTING
              : s->have_profile ? DASH_NET_DOWN
                                : DASH_NET_BOOTING;
    if (s->online) {
        esp_ip4_addr_t ip = {.addr = s->ip};
        snprintf(st->ip, sizeof(st->ip), IPSTR, IP2STR(&ip));
        if (s->rssi) {
            st->rssi = s->rssi;
        }
    } else {
        st->ip[0] = '\0';
    }
    st->clock_valid = s->clock_valid;
    app_unlock(true);
}

void app_main(void)
{
    /* Set once, before any JSON-using task starts. Small cJSON nodes would
     * otherwise occupy internal RAM during every status/calendar/TLS overlap.
     * heap_caps_free also handles the internal-RAM fallback safely. */
    cJSON_Hooks json_hooks = {.malloc_fn = json_malloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&json_hooks);
    app_state_init();
    app_ota_init();
    static const campus_net_hooks_t hooks = {
        .on_status = on_net_status,
        .on_command = app_cmd_handle,
    };
    campus_net_start(&hooks);
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(fetch_task, "fetch", 8192, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(muse_task, "muse", 4096, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(led_task, "led", 3072, NULL, 2, NULL, 1);
    xTaskCreatePinnedToCore(knob_task, "knob", 3072, NULL, 3, NULL, 1);
    xTaskCreatePinnedToCore(sensor_task, "sensor", 3072, NULL, 2, NULL, 1);
}
