/*
 * EC11 rotary knob: CLK -> GPIO1, DT -> GPIO2, SW -> GPIO42, + -> 3V3 (not 5 V), GND -> GND.
 *
 * Rotation is decoded in hardware by the PCNT peripheral (x4 quadrature with a glitch
 * filter, as in ESP-IDF's rotary_encoder example); the push button is polled every 10 ms.
 * Events go to the UI task through app_knob_push(), the same queue the KNOB serial
 * command feeds, so everything can be tested before the knob is wired up.
 *
 * Nothing connected: the internal pull-ups keep all three lines high, so no events.
 */
#include <stdio.h>

#include "app.h"
#include "dash_ui.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define KNOB_COUNTS_PER_DETENT 4 /* EC11: one full quadrature cycle per click */
#define KNOB_LONG_MS 600
#define KNOB_POLL_MS 10
#define KNOB_LIMIT 1000

static volatile bool reversed;

void app_knob_set_reverse(bool rev)
{
    reversed = rev;
}

static pcnt_unit_handle_t knob_pcnt_init(void)
{
    pcnt_unit_config_t ucfg = {.low_limit = -KNOB_LIMIT, .high_limit = KNOB_LIMIT};
    pcnt_unit_handle_t unit = NULL;
    if (pcnt_new_unit(&ucfg, &unit) != ESP_OK) {
        return NULL;
    }
    pcnt_glitch_filter_config_t fcfg = {.max_glitch_ns = 1000};
    pcnt_chan_config_t a_cfg = {.edge_gpio_num = KNOB_GPIO_A, .level_gpio_num = KNOB_GPIO_B};
    pcnt_chan_config_t b_cfg = {.edge_gpio_num = KNOB_GPIO_B, .level_gpio_num = KNOB_GPIO_A};
    pcnt_channel_handle_t a = NULL, b = NULL;
    bool ok = pcnt_unit_set_glitch_filter(unit, &fcfg) == ESP_OK &&
              pcnt_new_channel(unit, &a_cfg, &a) == ESP_OK && pcnt_new_channel(unit, &b_cfg, &b) == ESP_OK &&
              pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                           PCNT_CHANNEL_EDGE_ACTION_INCREASE) == ESP_OK &&
              pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                            PCNT_CHANNEL_LEVEL_ACTION_INVERSE) == ESP_OK &&
              pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                           PCNT_CHANNEL_EDGE_ACTION_DECREASE) == ESP_OK &&
              pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                            PCNT_CHANNEL_LEVEL_ACTION_INVERSE) == ESP_OK;
    /* most EC11 boards have 10k pull-ups already; the internal ones cover bare encoders */
    ok = ok && gpio_pullup_en(KNOB_GPIO_A) == ESP_OK && gpio_pullup_en(KNOB_GPIO_B) == ESP_OK &&
         pcnt_unit_enable(unit) == ESP_OK && pcnt_unit_clear_count(unit) == ESP_OK &&
         pcnt_unit_start(unit) == ESP_OK;
    return ok ? unit : NULL;
}

void knob_task(void *arg)
{
    uint32_t rev = 0;
    if (app_nvs_get_u32("dash", "knob_rev", &rev) == ESP_OK) {
        reversed = rev != 0;
    }
    pcnt_unit_handle_t unit = knob_pcnt_init();
    gpio_config_t sw = {
        .pin_bit_mask = 1ULL << KNOB_GPIO_SW,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    if (!unit || gpio_config(&sw) != ESP_OK) {
        printf("dash: knob init failed\n");
        vTaskDelete(NULL);
        return;
    }
    printf("dash: knob ready (CLK %d, DT %d, SW %d)\n", KNOB_GPIO_A, KNOB_GPIO_B, KNOB_GPIO_SW);

    int prev = 0, acc = 0;
    bool raw_last = false, pressed = false, turned = false, long_sent = false;
    int stable = 0;
    int64_t press_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(KNOB_POLL_MS));
        int64_t now = esp_timer_get_time() / 1000;

        /* button: active low, 20 ms debounce */
        bool raw = gpio_get_level(KNOB_GPIO_SW) == 0;
        stable = raw == raw_last ? stable + 1 : 0;
        raw_last = raw;
        if (stable == 2 && raw != pressed) {
            pressed = raw;
            if (pressed) {
                press_ms = now;
                turned = long_sent = false;
            } else if (turned) {
                app_knob_push(DASH_KNOB_RELEASE, 0); /* end of a brightness change */
            } else if (!long_sent) {
                app_knob_push(DASH_KNOB_CLICK, 0);
            }
        }
        if (pressed && !turned && !long_sent && now - press_ms >= KNOB_LONG_MS) {
            long_sent = true;
            app_knob_push(DASH_KNOB_LONG, 0);
        }

        /* rotation */
        int count = 0;
        if (pcnt_unit_get_count(unit, &count) != ESP_OK) {
            continue;
        }
        acc += count - prev;
        prev = count;
        if (count > KNOB_LIMIT / 2 || count < -KNOB_LIMIT / 2) { /* stay far from the limits */
            pcnt_unit_clear_count(unit);
            prev = 0;
        }
        int detents = acc / KNOB_COUNTS_PER_DETENT;
        if (detents) {
            acc -= detents * KNOB_COUNTS_PER_DETENT;
            if (reversed) {
                detents = -detents;
            }
            if (pressed) {
                turned = true;
            }
            app_knob_push(pressed ? DASH_KNOB_HOLD_TURN : DASH_KNOB_TURN, detents);
        }
    }
}
