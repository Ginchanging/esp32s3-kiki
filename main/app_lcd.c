/*
 * The real screen: 2.8" ILI9341V over SPI, FT6336G capacitive touch over I2C,
 * PWM backlight. Pins are in app.h (LCD_GPIO_*, TP_GPIO_*), wiring in README.
 *
 * Only the UI task calls into this file. The one exception is
 * app_lcd_request_reload(), which the LCD serial command uses after it saved
 * new settings; the UI task applies them in app_lcd_poll().
 *
 * Settings (NVS "dash"), changed with ./esp.sh lcd …:
 *   lcd_flags  LCD_F_* bits: colour inversion, RGB order, mirrors, touch transform
 *   lcd_mhz    SPI clock in MHz (default LCD_DEFAULT_MHZ; lower it for long wires)
 *   lcd_bl     backlight percent (default 100)
 *
 * Flushes: LVGL renders RGB565 little-endian; the panel wants big-endian, so
 * app_lcd_draw() swaps the buffer in place (after the mirror has sent it) and
 * queues a DMA transfer. The DMA-done interrupt only gives a semaphore, which
 * keeps it safe while flash cache is off (NVS writes); LVGL waits on it in
 * the flush_wait callback (app_lcd_wait).
 */
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "dash_ui.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST SPI2_HOST
#define TP_ADDR 0x38 /* FT6336G */
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_RES_BITS 10
#define TOUCH_MAX_ERRORS 20 /* consecutive I2C errors before touch is switched off */

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t done_sem;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t tp;
static bool panel_ok, touch_ok, bl_ok, bl_ready;
static uint32_t flags, mhz = LCD_DEFAULT_MHZ, bl_pct = 100;
static uint8_t tp_chip;
static int touch_errors;
static bool touch_down;
static int raw_x = -1, raw_y = -1, last_x = -1, last_y = -1;
static volatile bool reload_req;

typedef struct {
    uint8_t cmd, len;
    uint8_t data[15];
} lcd_cmd_t;

/* ILI9341 power, VCOM and gamma setup (the widely used Adafruit / TFT_eSPI values) */
static const lcd_cmd_t ili9341_init[] = {
    {0xEF, 3, {0x03, 0x80, 0x02}},
    {0xCF, 3, {0x00, 0xC1, 0x30}},             /* power control B */
    {0xED, 4, {0x64, 0x03, 0x12, 0x81}},       /* power-on sequence */
    {0xE8, 3, {0x85, 0x00, 0x78}},             /* driver timing A */
    {0xCB, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}}, /* power control A */
    {0xF7, 1, {0x20}},                         /* pump ratio */
    {0xEA, 2, {0x00, 0x00}},                   /* driver timing B */
    {0xC0, 1, {0x23}},                         /* power control 1 */
    {0xC1, 1, {0x10}},                         /* power control 2 */
    {0xC5, 2, {0x3E, 0x28}},                   /* VCOM 1 */
    {0xC7, 1, {0x86}},                         /* VCOM 2 */
    {0x37, 1, {0x00}},                         /* vertical scroll start */
    {0x3A, 1, {0x55}},                         /* RGB565 */
    {0xB1, 2, {0x00, 0x18}},                   /* frame rate */
    {0xB6, 3, {0x08, 0x82, 0x27}},             /* display function */
    {0xF2, 1, {0x00}},                         /* 3-gamma off */
    {0x26, 1, {0x01}},                         /* gamma curve 1 */
    {0xE0, 15, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00}},
    {0xE1, 15, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F}},
};

static bool IRAM_ATTR color_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata,
                                 void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(done_sem, &woken);
    return woken == pdTRUE;
}

static void load_settings(void)
{
    flags = 0;
    mhz = LCD_DEFAULT_MHZ;
    bl_pct = 100;
    app_nvs_get_u32("dash", "lcd_flags", &flags);
    app_nvs_get_u32("dash", "lcd_mhz", &mhz);
    app_nvs_get_u32("dash", "lcd_bl", &bl_pct);
    if (mhz < LCD_MIN_MHZ || mhz > LCD_MAX_MHZ) {
        mhz = LCD_DEFAULT_MHZ;
    }
    if (bl_pct > 100) {
        bl_pct = 100;
    }
}

static esp_err_t io_create(void)
{
    esp_lcd_panel_io_spi_config_t cfg = {
        .cs_gpio_num = LCD_GPIO_CS,
        .dc_gpio_num = LCD_GPIO_DC,
        .spi_mode = 0,
        .pclk_hz = mhz * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &cfg, &io);
}

static void panel_apply_flags(void)
{
    uint8_t mad = 0x48; /* MX | BGR: upright portrait on the usual modules */
    if (flags & LCD_F_MX) {
        mad ^= 0x40;
    }
    if (flags & LCD_F_MY) {
        mad ^= 0x80;
    }
    if (flags & LCD_F_RGB) {
        mad &= (uint8_t)~0x08;
    }
    esp_lcd_panel_io_tx_param(io, 0x36, &mad, 1);
    esp_lcd_panel_io_tx_param(io, (flags & LCD_F_INV) ? 0x21 : 0x20, NULL, 0);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_RES_BITS,
        .timer_num = BL_TIMER,
        .freq_hz = 20000, /* above hearing range */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t c = {
        .gpio_num = LCD_GPIO_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0, /* dark until the first frame is out */
    };
    bl_ok = ledc_timer_config(&t) == ESP_OK && ledc_channel_config(&c) == ESP_OK;
}

static void backlight_set(uint32_t pct)
{
    if (!bl_ok) {
        return;
    }
    uint32_t duty = pct >= 100 ? (1u << BL_RES_BITS) : pct * ((1u << BL_RES_BITS) - 1) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static void touch_probe(void)
{
    if (!i2c_bus || touch_ok) {
        return;
    }
    if (i2c_master_probe(i2c_bus, TP_ADDR, 50) != ESP_OK) {
        printf("dash: touch FT6336 not found at 0x%02x (check SDA 15 / SCL 16 / RST 18)\n", TP_ADDR);
        return;
    }
    if (!tp) {
        i2c_device_config_t dc = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = TP_ADDR,
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(i2c_bus, &dc, &tp) != ESP_OK) {
            return;
        }
    }
    uint8_t reg = 0xA3; /* chip id: 0x64 = FT6336G/U */
    i2c_master_transmit_receive(tp, &reg, 1, &tp_chip, 1, 50);
    touch_ok = true;
    touch_errors = 0;
    printf("dash: touch ok chip=0x%02x\n", tp_chip);
}

bool app_lcd_init(void)
{
    load_settings();
    done_sem = xSemaphoreCreateBinary();
    backlight_init();

    /* reset the panel and the touch controller together */
    gpio_config_t rst = {
        .pin_bit_mask = (1ULL << LCD_GPIO_RST) | (1ULL << TP_GPIO_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst);
    gpio_set_level(LCD_GPIO_RST, 0);
    gpio_set_level(TP_GPIO_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_GPIO_RST, 1);
    gpio_set_level(TP_GPIO_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    spi_bus_config_t bus = {
        .sclk_io_num = LCD_GPIO_SCK,
        .mosi_io_num = LCD_GPIO_MOSI,
        .miso_io_num = -1, /* write-only; MISO may stay unconnected */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DASH_UI_W * LCD_BUF_LINES * 2 + 16,
    };
    if (!done_sem || spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK || io_create() != ESP_OK) {
        printf("dash: lcd init failed, staying headless\n");
        return false;
    }
    for (size_t i = 0; i < sizeof(ili9341_init) / sizeof(ili9341_init[0]); ++i) {
        esp_lcd_panel_io_tx_param(io, ili9341_init[i].cmd, ili9341_init[i].data, ili9341_init[i].len);
    }
    panel_apply_flags();
    esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0); /* sleep out */
    vTaskDelay(pdMS_TO_TICKS(120));
    esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0); /* display on */
    panel_ok = true;

    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << TP_GPIO_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&irq); /* not used for wake-ups yet, just kept from floating */
    i2c_master_bus_config_t bc = {
        .i2c_port = -1,
        .sda_io_num = TP_GPIO_SDA,
        .scl_io_num = TP_GPIO_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    if (i2c_new_master_bus(&bc, &i2c_bus) == ESP_OK) {
        touch_probe(); /* the FT6336 needs ~300 ms after reset; the panel init took that long */
    }
    printf("dash: lcd ok mhz=%u flags=0x%02x touch=%d\n", (unsigned)mhz, (unsigned)flags, touch_ok);
    return true;
}

bool app_lcd_draw(int x1, int y1, int x2, int y2, uint16_t *px)
{
    if (!panel_ok) {
        return false;
    }
    size_t n = (size_t)(x2 - x1 + 1) * (size_t)(y2 - y1 + 1);
    for (size_t i = 0; i < n; ++i) {
        px[i] = __builtin_bswap16(px[i]);
    }
    uint8_t ca[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    uint8_t ra[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
    esp_lcd_panel_io_tx_param(io, 0x2A, ca, 4);
    esp_lcd_panel_io_tx_param(io, 0x2B, ra, 4);
    return esp_lcd_panel_io_tx_color(io, 0x2C, px, n * 2) == ESP_OK;
}

void app_lcd_wait(void)
{
    if (xSemaphoreTake(done_sem, pdMS_TO_TICKS(500)) != pdTRUE) {
        printf("dash: lcd flush timeout\n");
    }
}

bool app_lcd_touch(int *x, int *y)
{
    if (!touch_ok) {
        return false;
    }
    uint8_t reg = 0x02, d[5];
    if (i2c_master_transmit_receive(tp, &reg, 1, d, sizeof(d), 20) != ESP_OK) {
        if (++touch_errors >= TOUCH_MAX_ERRORS) {
            touch_ok = false; /* ./esp.sh lcd (any setting) probes again */
            printf("dash: touch off after %d I2C errors\n", touch_errors);
        }
        return false;
    }
    touch_errors = 0;
    int count = d[0] & 0x0F;
    if (count < 1 || count > 2) {
        touch_down = false;
        return false;
    }
    int tx = ((d[1] & 0x0F) << 8) | d[2];
    int ty = ((d[3] & 0x0F) << 8) | d[4];
    raw_x = tx;
    raw_y = ty;
    if (flags & LCD_F_TXY) {
        int t = tx;
        tx = ty;
        ty = t;
    }
    if (flags & LCD_F_TMX) {
        tx = DASH_UI_W - 1 - tx;
    }
    if (flags & LCD_F_TMY) {
        ty = DASH_UI_H - 1 - ty;
    }
    tx = tx < 0 ? 0 : tx >= DASH_UI_W ? DASH_UI_W - 1 : tx;
    ty = ty < 0 ? 0 : ty >= DASH_UI_H ? DASH_UI_H - 1 : ty;
    last_x = tx;
    last_y = ty;
    if (!touch_down) { /* log presses, handy for checking the touch direction */
        printf("dash: touch raw=%d,%d at=%d,%d\n", raw_x, raw_y, tx, ty);
    }
    touch_down = true;
    *x = tx;
    *y = ty;
    return true;
}

void app_lcd_backlight_on(void)
{
    bl_ready = true;
    backlight_set(bl_pct);
}

void app_lcd_request_reload(void)
{
    reload_req = true;
}

bool app_lcd_poll(void)
{
    if (!reload_req) {
        return false;
    }
    reload_req = false;
    uint32_t old_mhz = mhz;
    load_settings();
    if (panel_ok && mhz != old_mhz) {
        esp_lcd_panel_io_del(io); /* waits for the transfer in flight */
        if (io_create() != ESP_OK) {
            panel_ok = false;
            printf("dash: lcd io re-create failed\n");
        }
    }
    if (panel_ok) {
        panel_apply_flags();
    }
    if (bl_ready) {
        backlight_set(bl_pct);
    }
    touch_probe();
    printf("dash: lcd applied mhz=%u flags=0x%02x bl=%u touch=%d\n", (unsigned)mhz, (unsigned)flags,
           (unsigned)bl_pct, touch_ok);
    return panel_ok; /* caller redraws the whole screen */
}

void app_lcd_print_status(const char *prefix)
{
    char names[48] = "";
    static const struct {
        const char *name;
        uint32_t bit;
    } opts[] = {LCD_FLAG_NAMES};
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); ++i) {
        if (flags & opts[i].bit) {
            size_t len = strlen(names);
            snprintf(names + len, sizeof(names) - len, "%s%s", len ? "," : "", opts[i].name);
        }
    }
    printf("%slcd panel=%d touch=%d chip=0x%02x mhz=%u bl=%u flags=%s last_raw=%d,%d last_at=%d,%d\n",
           prefix, panel_ok, touch_ok, tp_chip, (unsigned)mhz, (unsigned)bl_pct,
           names[0] ? names : "none", raw_x, raw_y, last_x, last_y);
}
