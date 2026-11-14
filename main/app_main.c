/*
 * ESP32-S3 desk dashboard.
 *   campus task : campus Wi-Fi (PEAP) + "@sysu" serial commands (components/campus_net)
 *   ui task     : LVGL dashboard (components/dash_ui), headless until the screen arrives
 *   fetch task  : hub JSON over HTTPS (GitHub Actions + 3 servers)
 *   muse task   : Muse alerts over Bemfa MQTT
 *   led task    : status LED
 *   knob task   : EC11 rotary knob (pages, lamp, pomodoro)
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "campus_net.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"

static void on_net_status(const campus_net_status_t *s)
{
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
    app_state_init();
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
}
