/*
 * Status LED. For now: the board's onboard WS2812 (GPIO48 on DevKitC-1 v1.0,
 * GPIO38 on v1.1; set with "LED <gpio>"). Later: the KY-016 on GPIO4/5/6.
 * Colour rules are shared with the simulator (dash_led_pattern/dash_led_color).
 */
#include <stdio.h>
#include <time.h>

#include "app.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;

static void led_deinit(void)
{
    if (chan) {
        rmt_disable(chan);
        rmt_del_channel(chan);
        chan = NULL;
    }
    if (enc) {
        rmt_del_encoder(enc);
        enc = NULL;
    }
}

static bool led_init(int gpio)
{
    led_deinit();
    if (gpio <= 0) {
        return false;
    }
    rmt_tx_channel_config_t cfg = {
        .gpio_num = gpio,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1 us per tick */
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    rmt_bytes_encoder_config_t bytes = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9}, /* 0.3 / 0.9 us */
        .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3}, /* 0.9 / 0.3 us */
        .flags.msb_first = 1,
    };
    if (rmt_new_tx_channel(&cfg, &chan) != ESP_OK || rmt_new_bytes_encoder(&bytes, &enc) != ESP_OK ||
        rmt_enable(chan) != ESP_OK) {
        printf("dash: led init failed gpio=%d\n", gpio);
        led_deinit();
        return false;
    }
    return true;
}

static void led_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!chan) {
        return;
    }
    uint8_t grb[3] = {g, r, b};
    rmt_transmit_config_t tx = {.loop_count = 0};
    if (rmt_transmit(chan, enc, grb, sizeof(grb), &tx) == ESP_OK) {
        rmt_tx_wait_all_done(chan, 20);
    }
}

void led_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000)); /* campus_nvs is mounted by then */
    uint32_t gpio = LED_DEFAULT_GPIO;
    app_nvs_get_u32("dash", "led_gpio", &gpio);
    led_init((int)gpio);
    printf("dash: led gpio=%u\n", (unsigned)gpio);

    static dash_state_t local;
    uint32_t last_rgb = 0xFFFFFFFF;
    uint32_t mirrored_rgb = 0xFFFFFFFF; /* last colour sent to the mirror window */
    for (;;) {
        if (app_take_led_changed()) {
            led_write(0, 0, 0);
            gpio = 0;
            app_nvs_get_u32("dash", "led_gpio", &gpio);
            led_init((int)gpio);
            last_rgb = 0xFFFFFFFF;
            printf("dash: led gpio=%u\n", (unsigned)gpio);
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        dash_led_timers_t timers;
        dash_state_t *st = app_lock();
        local = *st;
        timers = *app_led_timers_locked();
        app_unlock(false);
        dash_led_pattern_t p = dash_led_pattern(&local, &timers, time(NULL), now_ms);
        uint8_t r, g, b;
        dash_led_color_ex(p, &timers, now_ms, local.pomo_percent, &r, &g, &b);
        uint32_t rgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
        if (rgb != last_rgb) {
            led_write(r, g, b);
            last_rgb = rgb;
        }
        if (!app_mirror_active()) {
            mirrored_rgb = 0xFFFFFFFF;
        } else if (rgb != mirrored_rgb) {
            printf("@L %u %u %u\n", r, g, b); /* virtual LED in scripts/mirror.py */
            mirrored_rgb = rgb;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
