/* SHT3x (SHT30/31/35): VCC -> 3V3, GND -> GND, SDA -> 13, SCL -> 14.
 * Own task and own I2C controller: sensor waits/errors never block touch or LVGL.
 * Single-shot high repeatability, no clock stretching, heater left off. */
#include <stdio.h>

#include "app.h"
#include "sht3x.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SENSOR_PERIOD_MS 5000
#define SENSOR_MAX_FAILURES 3
#define SENSOR_TIMEOUT_MS 50

/* Diagnostics share the state mutex; I2C handles belong only to sensor_task. */
static struct {
    uint8_t address;
    unsigned failures;
    int64_t last_ok_ms;
    esp_err_t error;
} diag;

static esp_err_t sensor_attach(i2c_master_bus_handle_t bus, i2c_master_dev_handle_t *dev,
                               uint8_t *address)
{
    const uint8_t addresses[] = {0x44, 0x45};
    esp_err_t last = ESP_ERR_NOT_FOUND;
    for (unsigned i = 0; i < sizeof(addresses); ++i) {
        esp_err_t err = i2c_master_probe(bus, addresses[i], SENSOR_TIMEOUT_MS);
        if (err != ESP_OK) {
            if (err != ESP_ERR_NOT_FOUND) last = err;
            continue;
        }
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addresses[i],
            .scl_speed_hz = 100000,
        };
        err = i2c_master_bus_add_device(bus, &cfg, dev);
        if (err != ESP_OK) return err;
        const uint8_t reset[] = {0x30, 0xa2};
        err = i2c_master_transmit(*dev, reset, sizeof(reset), SENSOR_TIMEOUT_MS);
        vTaskDelay(pdMS_TO_TICKS(20)); /* >=10 ms even with an unfavourable tick phase */
        if (err == ESP_OK) {
            *address = addresses[i];
            return ESP_OK;
        }
        i2c_master_bus_rm_device(*dev);
        *dev = NULL;
        last = err;
    }
    return last;
}

static esp_err_t sensor_read(i2c_master_dev_handle_t dev, float *temp_c, float *humidity)
{
    const uint8_t measure[] = {0x24, 0x00};
    esp_err_t err = i2c_master_transmit(dev, measure, sizeof(measure), SENSOR_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    /* Max high-repeatability conversion is 15.5 ms; allow tick phase as well. */
    vTaskDelay(pdMS_TO_TICKS(30));
    uint8_t frame[6];
    err = i2c_master_receive(dev, frame, sizeof(frame), SENSOR_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    return sht3x_decode(frame, temp_c, humidity) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

void sensor_task(void *arg)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_dev_handle_t dev = NULL;
    uint8_t address = 0;
    unsigned failures = 0;
    bool online = false;
    /* Let the UI allocate its touch controller first; auto-selection also makes
     * the two buses independent if UI startup is delayed or the panel is absent. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    for (;;) {
        esp_err_t err = ESP_OK;
        if (!bus) {
            i2c_master_bus_config_t cfg = {
                .i2c_port = -1,
                .sda_io_num = SENSOR_GPIO_SDA,
                .scl_io_num = SENSOR_GPIO_SCL,
                .clk_source = I2C_CLK_SRC_DEFAULT,
                .glitch_ignore_cnt = 7,
                .flags.enable_internal_pullup = 1,
            };
            err = i2c_new_master_bus(&cfg, &bus);
        }
        if (err == ESP_OK && !dev) err = sensor_attach(bus, &dev, &address);
        float temp_c = 0, humidity = 0;
        if (err == ESP_OK) err = sensor_read(dev, &temp_c, &humidity);
        if (err == ESP_OK) {
            failures = 0;
            if (!online) {
                printf("dash: SHT3x ready addr=0x%02x SDA=%d SCL=%d\n",
                       address, SENSOR_GPIO_SDA, SENSOR_GPIO_SCL);
            }
            online = true;
        } else {
            if (failures < UINT32_MAX) ++failures;
            if (failures == 1 || failures % 12 == 0) {
                printf("dash: SHT3x read failed err=%s failures=%u (SDA=%d SCL=%d)\n",
                       esp_err_to_name(err), failures, SENSOR_GPIO_SDA, SENSOR_GPIO_SCL);
            }
        }
        dash_state_t *st = app_lock();
        bool changed = false;
        if (err == ESP_OK) {
            changed = !st->sensor_valid || st->temp_c != temp_c || st->humidity != humidity;
            st->sensor_valid = true;
            st->temp_c = temp_c;
            st->humidity = humidity;
            diag.last_ok_ms = esp_timer_get_time() / 1000;
        } else if (failures >= SENSOR_MAX_FAILURES) {
            changed = st->sensor_valid;
            st->sensor_valid = false; /* the room shows --, never an indefinitely stale value */
        }
        diag.address = address;
        diag.failures = failures;
        diag.error = err;
        app_unlock(changed);

        if (err != ESP_OK && failures >= SENSOR_MAX_FAILURES) {
            online = false;
            if (dev) {
                i2c_master_bus_rm_device(dev);
                dev = NULL;
            }
            address = 0;
            if (bus) i2c_master_bus_reset(bus); /* only this sensor's bus, never the touch bus */
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}

void app_sensor_print_status(const char *prefix)
{
    dash_state_t *st = app_lock();
    bool valid = st->sensor_valid;
    float temp_c = st->temp_c, humidity = st->humidity;
    unsigned address = diag.address, failures = diag.failures;
    esp_err_t err = diag.error;
    int64_t age = diag.last_ok_ms ? (esp_timer_get_time() / 1000 - diag.last_ok_ms) / 1000 : -1;
    app_unlock(false);
    printf("%ssensor valid=%d addr=0x%02x sda=%d scl=%d temp_c=%.1f humidity=%.1f failures=%u age=%lld err=%s\n",
           prefix, valid, address, SENSOR_GPIO_SDA, SENSOR_GPIO_SCL, temp_c, humidity, failures,
           (long long)age, esp_err_to_name(err));
}
