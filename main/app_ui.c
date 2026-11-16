/*
 * UI task: owns LVGL. Frames go to the ILI9341 (app_lcd.c); if the SPI bus
 * cannot be set up the display stays "headless" and drops them. Either way,
 * in mirror mode the flush callback also streams them to scripts/mirror.py:
 *
 *   @F <x1> <y1> <x2> <y2> <fnv1a-hex> <base64(RLE(RGB565 LE))>\n
 *
 * RLE: control byte c; c & 0x80 -> repeat next pixel (c & 0x7F) + 1 times,
 * otherwise c + 1 literal pixels follow. A frame line that another task's
 * printf corrupted fails the checksum; the viewer then asks for MIRROR full.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "campus_net.h"
#include "dash_ui.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#if !defined(DASH_LV_CONF_USED)
#error "LVGL is not using third_party/lv_conf.h"
#endif

#define MIRROR_ROWS 8 /* rows per serial line */
#define MIRROR_PX (DASH_UI_W * MIRROR_ROWS)

static uint32_t flushes;
static uint32_t flush_px;
static uint32_t mirror_bytes;

static uint8_t rle_buf[MIRROR_PX * 2 + MIRROR_PX / 128 + 8];
static char line_buf[64 + ((sizeof(rle_buf) + 2) / 3) * 4 + 2];

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static size_t rle_encode(const uint16_t *px, size_t n, uint8_t *out)
{
    size_t o = 0, i = 0;
    while (i < n) {
        size_t run = 1;
        while (i + run < n && run < 128 && px[i + run] == px[i]) {
            run++;
        }
        if (run >= 2) {
            out[o++] = (uint8_t)(0x80 | (run - 1));
            out[o++] = (uint8_t)(px[i] & 0xFF);
            out[o++] = (uint8_t)(px[i] >> 8);
            i += run;
            continue;
        }
        size_t lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && px[i + lit] == px[i + lit + 1])) {
            lit++;
        }
        out[o++] = (uint8_t)(lit - 1);
        for (size_t k = 0; k < lit; ++k) {
            out[o++] = (uint8_t)(px[i + k] & 0xFF);
            out[o++] = (uint8_t)(px[i + k] >> 8);
        }
        i += lit;
    }
    return o;
}

static size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= in[i + 2];
        }
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    return o;
}

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Write in small chunks with a timeout; false means nobody is reading. */
static bool usb_write_all(const char *p, size_t n)
{
    while (n) {
        size_t chunk = n > 512 ? 512 : n;
        if (usb_serial_jtag_write_bytes(p, chunk, pdMS_TO_TICKS(300)) <= 0) {
            return false;
        }
        p += chunk;
        n -= chunk;
        mirror_bytes += chunk;
    }
    return true;
}

static void mirror_send(const lv_area_t *area, const uint16_t *px)
{
    if (!usb_serial_jtag_is_connected()) {
        app_mirror_set_active(false);
        return;
    }
    int32_t w = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; y += MIRROR_ROWS) {
        int32_t y2 = y + MIRROR_ROWS - 1 > area->y2 ? area->y2 : y + MIRROR_ROWS - 1;
        size_t n = (size_t)w * (size_t)(y2 - y + 1);
        size_t rl = rle_encode(px + (size_t)(y - area->y1) * w, n, rle_buf);
        int h = snprintf(line_buf, 64, "@F %d %d %d %d %08x ", (int)area->x1, (int)y,
                         (int)area->x2, (int)y2, (unsigned)fnv1a(rle_buf, rl));
        size_t len = (size_t)h + b64_encode(rle_buf, rl, line_buf + h);
        line_buf[len++] = '\n';
        if (!usb_write_all(line_buf, len)) {
            app_mirror_set_active(false); /* the Mac stopped reading */
            printf("dash: mirror stopped (host not reading)\n");
            return;
        }
    }
}

static void headless_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    flushes++;
    flush_px += (uint32_t)lv_area_get_size(area);
    if (app_mirror_active()) {
        mirror_send(area, (const uint16_t *)px);
    }
    lv_display_flush_ready(disp);
}

static void lcd_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    flushes++;
    flush_px += (uint32_t)lv_area_get_size(area);
    if (app_mirror_active()) {
        mirror_send(area, (const uint16_t *)px); /* before app_lcd_draw byte-swaps it */
    }
    if (!app_lcd_draw(area->x1, area->y1, area->x2, area->y2, (uint16_t *)px)) {
        lv_display_flush_ready(disp); /* nothing queued, so LVGL won't wait */
    }
}

static void lcd_flush_wait(lv_display_t *disp)
{
    (void)disp;
    app_lcd_wait();
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    static bool ft_last; /* the last press came from the real screen */
    static int fx, fy;
    int x, y;
    bool pressed;
    /* mirror window first: one queued event per read, so LVGL sees every press/move/release */
    app_touch_get(&x, &y, &pressed);
    if (pressed) {
        ft_last = false;
    } else if (app_lcd_touch(&fx, &fy)) { /* then the FT6336 */
        ft_last = true;
        pressed = true;
        x = fx;
        y = fy;
    } else if (ft_last) { /* release where the finger was */
        x = fx;
        y = fy;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static lv_display_t *display_create(void)
{
    lv_display_t *disp = lv_display_create(DASH_UI_W, DASH_UI_H);
    size_t bytes = DASH_UI_W * LCD_BUF_LINES * 2;
    void *b1 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *b2 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    if (app_lcd_init()) {
        lv_display_set_flush_cb(disp, lcd_flush);
        lv_display_set_flush_wait_cb(disp, lcd_flush_wait);
    } else {
        lv_display_set_flush_cb(disp, headless_flush);
    }
    /* Touch: the FT6336 plus clicks from the mirror window. */
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);
    lv_indev_set_display(touch, disp);
    return disp;
}

static void save_lang(dash_lang_t lang)
{
    app_nvs_set_u32("dash", "lang", (uint32_t)lang);
}

static void save_pomo(int count, int minutes, int yday)
{
    app_nvs_set_u32("dash", "pomo_n", (uint32_t)count);
    app_nvs_set_u32("dash", "pomo_min", (uint32_t)minutes);
    app_nvs_set_u32("dash", "pomo_day", (uint32_t)yday);
    dash_pomo_day_t week[DASH_DAYS]; /* for the weekly report */
    dash_ui_pomo_week(week);
    app_nvs_set_blob("dash", "pomo_wk", week, sizeof(week));
}

/* Kiki's diary: 0 = none pending, 1 = daily, 2 = weekly (CAT diary [week]) */
static int diary_forced;
static uint32_t diary_day; /* dash_local_day() of the last diary (NVS dash/diary_day) */

static void write_diary(const dash_state_t *st, int64_t now, bool weekly)
{
    static dash_muse_msg_t m; /* ~300 B, keep it off the task stack */
    dash_ui_diary_msg(st, now, weekly, &m);
    dash_state_t *s = app_lock();
    dash_msg_push(s, (dash_msg_src_t)m.src, (dash_msg_kind_t)m.kind, m.project, m.text, now);
    app_unlock(true); /* the next loop shows it like an agent push, without the LED */
    printf("dash: diary %s bytes=%u\n", weekly ? "week" : "day", (unsigned)strlen(m.text));
}

/* "CAT ..." from the serial port: force Kiki into a state for testing */
static void apply_cat_request(const char *a)
{
    int n;
    char name[40];
    if (sscanf(a, "act %d", &n) == 1) {
        dash_ui_debug_activity(n);
    } else if (sscanf(a, "react %d", &n) == 1) {
        dash_ui_debug_react(n);
    } else if (sscanf(a, "look %39[^\n]", name) == 1) {
        dash_ui_debug_look(name);
    } else if (!strcmp(a, "night on")) {
        dash_ui_debug_night(1);
    } else if (!strcmp(a, "night off")) {
        dash_ui_debug_night(0);
    } else if (sscanf(a, "time %d", &n) == 1) {
        dash_ui_debug_time_offset((int64_t)n * 3600);
    } else if (!strcmp(a, "pomo")) {
        dash_ui_pomo_toggle();
    } else if (!strcmp(a, "flight")) {
        dash_ui_debug_flyover();
    } else if (!strcmp(a, "diary")) {
        diary_forced = 1;
    } else if (!strcmp(a, "diary week")) {
        diary_forced = 2;
    } else if (!strcmp(a, "auto")) {
        dash_ui_debug_activity(-1);
        dash_ui_debug_night(-1);
        dash_ui_debug_look("auto");
        dash_ui_debug_time_offset(0);
    }
}

void ui_task(void *arg)
{
    /* settings live in campus_nvs, which the campus task mounts during start-up */
    for (int i = 0; i < 50 && !campus_net_storage_ready(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    uint32_t lang = DASH_LANG_ZH;
    app_nvs_get_u32("dash", "lang", &lang);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *disp = display_create();
    uint32_t pn = 0, pm = 0, pd = 0;
    app_nvs_get_u32("dash", "pomo_n", &pn);
    app_nvs_get_u32("dash", "pomo_min", &pm);
    app_nvs_get_u32("dash", "pomo_day", &pd);
    dash_ui_pomo_restore((int)pn, (int)pm, (int)pd);
    dash_pomo_day_t week[DASH_DAYS];
    size_t week_len = sizeof(week);
    if (app_nvs_get_blob("dash", "pomo_wk", week, &week_len) == ESP_OK && week_len == sizeof(week)) {
        dash_ui_pomo_week_restore(week);
    }
    app_nvs_get_u32("dash", "diary_day", &diary_day);
    dash_ui_set_pomo_cb(save_pomo);
    dash_ui_set_mi_cb(app_request_mi); /* taps on the 米家 page; the fetch task sends them */
    dash_ui_set_calendar_cb(app_request_calendar);
    dash_ui_create(disp, lang == DASH_LANG_EN ? DASH_LANG_EN : DASH_LANG_ZH);
    dash_ui_set_lang_cb(save_lang);
    lv_refr_now(disp);      /* draw the first frame while the backlight is still off */
    app_lcd_backlight_on(); /* so the panel's power-on noise never shows */
    printf("dash: ui ready lang=%s heap=%u\n", lang == DASH_LANG_EN ? "en" : "zh",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* This private UI snapshot never feeds DMA. Keep the long-lived copy in
     * PSRAM so pages and language rebuilds leave internal RAM for LVGL/Wi-Fi. */
    dash_state_t *local = heap_caps_malloc(sizeof(*local), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!local) local = heap_caps_malloc(sizeof(*local), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!local) {
        printf("dash: cannot allocate UI snapshot\n");
        vTaskDelete(NULL);
        return;
    }
    static dash_event_cursor_t cursor;
    uint32_t muse_seen = 0;
    int64_t next_update = 0, next_stats = 30000;
    uint32_t busy_max = 0, busy_total = 0;

    for (;;) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        bool mirror_on;
        if (app_mirror_take_request(&mirror_on)) {
            app_mirror_set_active(mirror_on);
            if (mirror_on) { /* send one complete frame */
                lv_obj_invalidate(lv_screen_active());
                lv_obj_invalidate(lv_layer_top());
            }
        }
        if (app_lcd_poll()) { /* LCD settings changed: redraw with the new orientation/colours */
            lv_obj_invalidate(lv_screen_active());
            lv_obj_invalidate(lv_layer_top());
        }
        char cat_req[64];
        if (app_take_cat_request(cat_req, sizeof(cat_req))) {
            apply_cat_request(cat_req);
        }
        int knob_ev, knob_steps;
        while (app_knob_take(&knob_ev, &knob_steps)) {
            dash_ui_knob((dash_knob_ev_t)knob_ev, knob_steps);
        }
        dash_lang_t req;
        if (app_take_lang_request(&req)) {
            dash_ui_set_lang(req);
            save_lang(req);
            next_update = 0;
        }
        if (app_take_dirty() || now_ms >= next_update) {
            dash_state_t *st = app_lock();
            *local = *st;
            app_unlock(false);
            int64_t now = time(NULL);
            dash_ui_update(local, now);
            if (local->muse_seq != muse_seen && local->muse_count) {
                if (local->muse[0].src == DASH_SRC_MUSE) {
                    dash_ui_show_muse(local->muse[0].text, local->muse[0].ts, 6000);
                } else {
                    dash_ui_show_agent(&local->muse[0], local->muse[0].src == DASH_SRC_KIKI ? 15000 : 6000);
                }
            }
            muse_seen = local->muse_seq;
            dash_event_t fresh[DASH_MAX_EVENTS];
            int n = dash_events_take_new(&cursor, local, fresh, DASH_MAX_EVENTS);
            const dash_event_t *last = NULL;
            for (int i = 0; i < n; ++i) {
                static dash_muse_msg_t m; /* ~300 B, keep it off the task stack */
                if (dash_ui_mi_event_msg(local, &fresh[i], &m)) {
                    app_agent_arrived(&m, now); /* mailbox + Kiki + LED, like an agent push */
                } else {
                    last = &fresh[i];
                }
                printf("dash: event kind=%d subject=%s\n", (int)fresh[i].kind, fresh[i].subject);
            }
            if (last) {
                dash_ui_on_event(last);
                if (last->level == DASH_EVT_SUCCESS) {
                    app_lock();
                    app_led_timers_locked()->success_until_ms = now_ms + 5000;
                    app_unlock(false);
                }
            }
            next_update = now_ms + 1000;

            /* Kiki's diary at 23:00 (weekly report on Sundays), once the hub has answered */
            dash_diary_t due = local->clock_valid ? dash_diary_due(now, (int32_t)diary_day) : DASH_DIARY_NONE;
            bool hub_ready = local->have_hub || local->fetch_failures >= DASH_OFFLINE_AFTER_FAILS;
            if (diary_forced || (due != DASH_DIARY_NONE && hub_ready)) {
                write_diary(local, now, diary_forced ? diary_forced == 2 : due == DASH_DIARY_WEEK);
                if (!diary_forced) {
                    diary_day = (uint32_t)dash_local_day(now);
                    app_nvs_set_u32("dash", "diary_day", diary_day);
                }
                diary_forced = 0;
            }

            /* let the LED task follow night mode and the pomodoro */
            bool night;
            uint8_t phase, pct;
            dash_ui_get_status(&night, &phase, &pct);
            dash_state_t *s2 = app_lock();
            s2->night = night;
            s2->pomo_phase = phase;
            s2->pomo_percent = pct;
            app_unlock(false);
        }

        int64_t t0 = esp_timer_get_time();
        uint32_t wait = lv_timer_handler();
        app_ota_ui_tick();
        static uint8_t ota_shown;
        uint8_t ota_number = dash_ui_ota_countdown_visible();
        if (ota_number != ota_shown) {
            lv_refr_now(disp);
            ota_shown = ota_number;
            app_ota_ui_presented(ota_number);
        }
        uint32_t busy = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        busy_total += busy;
        if (busy > busy_max) {
            busy_max = busy;
        }
        if (now_ms >= next_stats) {
            printf("dash: ui flushes=%u px=%u busy_max_ms=%u busy_total_ms=%u mirror_kb=%u "
                   "heap=%u min_heap=%u\n",
                   (unsigned)flushes, (unsigned)flush_px, (unsigned)busy_max,
                   (unsigned)busy_total, (unsigned)(mirror_bytes / 1024),
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            flushes = flush_px = busy_max = busy_total = mirror_bytes = 0;
            next_stats = now_ms + 60000;
        }
        if (wait < 10) { /* FreeRTOS tick is 10 ms; shorter delays would round to 0 */
            wait = 10;
        } else if (wait > 30) {
            wait = 30;
        }
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}
