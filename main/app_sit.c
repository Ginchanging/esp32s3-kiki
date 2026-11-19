/* Seat radar: UART1 RX only. USB-Serial/JTAG remains the command console. */
#include <stdio.h>

#include "app.h"
#include "campus_net.h"
#include "dash_sit.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RADAR_UART UART_NUM_1
#define RADAR_GPIO_RX GPIO_NUM_21

/* Radar state is local to the device, under the application state mutex. */
static struct {
    dash_sit_t seat;
    bool enabled, settings_loaded, demo_pending;
    unsigned range_cm;
    int baud;
    esp_err_t uart_error;
} sit = {.enabled = true, .range_cm = DASH_SIT_RANGE_CM};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static void load_settings_locked(void)
{
    if (sit.settings_loaded || !campus_net_storage_ready()) return;
    uint32_t value;
    if (app_nvs_get_u32("dash", "sit_en", &value) == ESP_OK && value <= 1) sit.enabled = value != 0;
    if (app_nvs_get_u32("dash", "sit_range", &value) == ESP_OK && value && value <= DASH_LD2402_MAX_CM) {
        sit.range_cm = value;
    }
    sit.settings_loaded = true;
}

esp_err_t app_sit_set_enabled(bool enabled)
{
    esp_err_t err = app_nvs_set_u32("dash", "sit_en", enabled);
    if (err != ESP_OK) return err;
    app_lock();
    load_settings_locked();
    if (sit.enabled != enabled) dash_sit_reset(&sit.seat);
    sit.enabled = enabled;
    app_unlock(false);
    return ESP_OK;
}

esp_err_t app_sit_set_range(unsigned cm)
{
    if (!cm || cm > DASH_LD2402_MAX_CM) return ESP_ERR_INVALID_ARG;
    esp_err_t err = app_nvs_set_u32("dash", "sit_range", cm);
    if (err != ESP_OK) return err;
    app_lock();
    load_settings_locked();
    if (sit.range_cm != cm) dash_sit_reset(&sit.seat);
    sit.range_cm = cm;
    app_unlock(false);
    return ESP_OK;
}

void app_sit_request_demo(void)
{
    app_lock();
    sit.demo_pending = true;
    app_unlock(true);
}

bool app_sit_take_reminder(bool *demo)
{
    app_lock();
    *demo = sit.demo_pending;
    bool take = *demo || (sit.enabled && dash_sit_take(&sit.seat, monotonic_ms()));
    sit.demo_pending = false;
    app_unlock(false);
    return take;
}

static dash_sit_status_t status_locked(uint64_t now)
{
    load_settings_locked();
    dash_sit_poll(&sit.seat, now);
    dash_sit_link_t link = sit.uart_error != ESP_OK ? DASH_SIT_LINK_ERROR :
                          dash_sit_fresh(&sit.seat, now) ? DASH_SIT_LINK_LIVE :
                          sit.seat.have_report ? DASH_SIT_LINK_STALE : DASH_SIT_LINK_WAITING;
    bool live = link == DASH_SIT_LINK_LIVE;
    return (dash_sit_status_t){
        .elapsed_s = live && sit.enabled ? (uint32_t)(dash_sit_elapsed(&sit.seat) / 1000) : 0,
        .distance_cm = live ? (uint16_t)sit.seat.distance_cm : 0,
        .range_cm = (uint16_t)sit.range_cm,
        .phase = live ? sit.seat.phase : DASH_SIT_IDLE,
        .link = link,
        .enabled = sit.enabled,
        .reminded = live && sit.seat.reminded,
    };
}

void app_sit_get_status(dash_sit_status_t *status)
{
    app_lock();
    *status = status_locked(monotonic_ms());
    app_unlock(false);
}

void app_sit_print_status(const char *prefix)
{
    static const char *const phases[] = {"idle", "entering", "present", "away"};
    static const char *const links[] = {"waiting", "live", "stale", "error"};
    app_lock();
    uint64_t now = monotonic_ms();
    dash_sit_status_t status = status_locked(now);
    int age = sit.seat.have_report ? (int)((now - sit.seat.last_ms) / 1000) : -1;
    int baud = sit.baud;
    esp_err_t error = sit.uart_error;
    app_unlock(false);

    printf("%ssit enabled=%d link=%s rx=%d baud=%d distance_cm=%d age=%d range_cm=%u phase=%s "
           "elapsed=%u reminded=%d err=%s\n",
           prefix, status.enabled, links[status.link], RADAR_GPIO_RX, baud,
           status.link == DASH_SIT_LINK_LIVE ? (int)status.distance_cm : -1, age,
           (unsigned)status.range_cm, phases[status.phase], (unsigned)status.elapsed_s,
           status.reminded, esp_err_to_name(error));
}

static esp_err_t start_uart(void)
{
    const uart_config_t cfg = {
        .baud_rate = DASH_LD2402_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(RADAR_UART, &cfg);
    if (err == ESP_OK) err = uart_driver_install(RADAR_UART, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) return err;
    err = uart_set_pin(RADAR_UART, UART_PIN_NO_CHANGE, RADAR_GPIO_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err == ESP_OK) err = gpio_pullup_en(RADAR_GPIO_RX); /* idle high with the sensor unplugged */
    if (err != ESP_OK) uart_driver_delete(RADAR_UART);
    return err;
}

static esp_err_t receive_reports(void)
{
    dash_ld2402_t rx;
    dash_ld2402_start(&rx, monotonic_ms());
    uint8_t buf[128];
    for (;;) {
        int n = uart_read_bytes(RADAR_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n < 0) return ESP_FAIL;
        uint64_t now = monotonic_ms();
        if (dash_ld2402_poll(&rx, now)) {
            int baud = dash_ld2402_baud(&rx);
            esp_err_t err = uart_set_baudrate(RADAR_UART, baud);
            if (err == ESP_OK) err = uart_flush_input(RADAR_UART);
            if (err != ESP_OK) return err;
            app_lock();
            sit.baud = baud;
            dash_sit_poll(&sit.seat, now);
            app_unlock(false);
            continue;
        }
        for (int i = 0; i < n; ++i) {
            int cm = dash_ld2402_feed(&rx, buf[i], now);
            if (cm < 0) continue;
            app_lock();
            load_settings_locked();
            dash_sit_report(&sit.seat, now, (unsigned)cm, sit.enabled ? sit.range_cm : 0);
            app_unlock(false);
        }
    }
}

void sit_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_err_t err = start_uart();
        if (err == ESP_OK) {
            app_lock();
            sit.baud = DASH_LD2402_BAUD;
            sit.uart_error = ESP_OK;
            app_unlock(false);
            err = receive_reports();
            uart_driver_delete(RADAR_UART);
        }
        app_lock();
        sit.uart_error = err;
        sit.baud = 0;
        dash_sit_reset(&sit.seat);
        app_unlock(false);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
