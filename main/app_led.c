/*
 * Status LEDs: onboard WS2812 (GPIO48 on DevKitC-1 v1.0, GPIO38 on v1.1;
 * set with "LED <gpio>") and KY-016 common-cathode RGB on GPIO4/5/6.
 * Colour rules are shared with the simulator (dash_led_pattern/dash_led_color).
 */
#include <stdio.h>
#include <time.h>

#include "app.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/rmt_tx.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static rmt_channel_handle_t chan;
static rmt_encoder_handle_t enc;

/* Backlight owns timer 0 / channel 0 in app_lcd.c. Never change those here. */
#define RGB_TIMER LEDC_TIMER_1
static const int rgb_pins[] = {RGB_GPIO_R, RGB_GPIO_G, RGB_GPIO_B};
static const ledc_channel_t rgb_channels[] = {LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3};
static bool rgb_ready; /* diagnostics below are guarded by the state mutex */
static uint32_t rgb_duty[3];

static void rgb_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = RGB_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK, /* same clock selection as the LCD backlight */
    };
    esp_err_t err = ledc_timer_config(&timer);
    unsigned configured = 0;
    if (err == ESP_OK) {
        for (unsigned i = 0; i < 3; ++i) {
            ledc_channel_config_t channel = {
                .gpio_num = rgb_pins[i],
                .speed_mode = LEDC_LOW_SPEED_MODE,
                .channel = rgb_channels[i],
                .timer_sel = RGB_TIMER,
                .duty = 0, /* common cathode: low is off, no startup flash */
            };
            err = ledc_channel_config(&channel);
            if (err != ESP_OK) break;
            ++configured;
        }
    }
    if (err != ESP_OK) {
        for (unsigned i = 0; i < configured; ++i) {
            ledc_stop(LEDC_LOW_SPEED_MODE, rgb_channels[i], 0);
        }
        for (unsigned i = 0; i < 3; ++i) {
            gpio_reset_pin(rgb_pins[i]);
            gpio_set_level(rgb_pins[i], 0);
            gpio_set_direction(rgb_pins[i], GPIO_MODE_OUTPUT);
        }
        printf("dash: KY-016 init failed err=%s\n", esp_err_to_name(err));
        return;
    }
    app_lock();
    rgb_ready = true;
    app_unlock(false);
    printf("dash: KY-016 ready R=%d G=%d B=%d pwm_hz=5000\n",
           RGB_GPIO_R, RGB_GPIO_G, RGB_GPIO_B);
}

static void rgb_write(uint8_t r, uint8_t g, uint8_t b)
{
    if (!rgb_ready) return;
    const uint8_t values[] = {r, g, b};
    uint32_t duties[3];
    for (unsigned i = 0; i < 3; ++i) {
        /* Shared colour values already keep brightness low (max 60/255).
         * Retain that limit rather than scaling notifications to full power. */
        ledc_set_duty(LEDC_LOW_SPEED_MODE, rgb_channels[i], values[i]);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, rgb_channels[i]);
        duties[i] = ledc_get_duty(LEDC_LOW_SPEED_MODE, rgb_channels[i]);
    }
    app_lock();
    for (unsigned i = 0; i < 3; ++i) rgb_duty[i] = duties[i];
    app_unlock(false);
}

void app_led_print_status(const char *prefix)
{
    app_lock();
    bool ready = rgb_ready;
    uint32_t r = rgb_duty[0], g = rgb_duty[1], b = rgb_duty[2];
    app_unlock(false);
    printf("%srgb ready=%d r=%d g=%d b=%d duty_r=%u duty_g=%u duty_b=%u\n", prefix,
           ready, RGB_GPIO_R, RGB_GPIO_G, RGB_GPIO_B, (unsigned)r, (unsigned)g, (unsigned)b);
}

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
    rgb_init();
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
            rgb_write(r, g, b);
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
