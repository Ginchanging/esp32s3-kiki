/* Actual rendered OTA overlay: digits, cat animation, language rebuild, lifetime. */
#include "dash_ui.h"
#include "dash_ota_ui.h"
#include "dash_flight_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t ticks;
static uint16_t framebuffer[240 * 320];
static uint32_t clock_ms(void) { return ticks; }
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(d);
}
static void run(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) {
        ticks += 20;
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}
static uint32_t region(int x, int y, int w, int h)
{
    uint32_t hash = 2166136261u;
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            hash = (hash ^ framebuffer[j * 240 + i]) * 16777619u;
    return hash;
}
int main(void)
{
    lv_init();
    lv_tick_set_cb(clock_ms);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, framebuffer, NULL, sizeof(framebuffer), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    dash_ui_create(d, DASH_LANG_ZH);
    dash_state_t state;
    dash_state_init(&state);
    state.ota = (dash_ota_status_t){DASH_OTA_DOWNLOADING, 42, 100, 0};
    dash_ui_open_tool(DASH_TOOL_CALENDAR);
    dash_ui_update(&state, 1794844800);
    run(100);
    assert(dash_ota_ui_active());
    uint32_t cat = region(56, 82, 128, 144);
    run(340);
    assert(cat != region(56, 82, 128, 144));
    uint32_t digits[3];
    for (int n = 3; n > 0; --n) {
        state.ota.phase = DASH_OTA_COUNTDOWN;
        state.ota.countdown = (uint8_t)n;
        dash_ui_update(&state, 1794844800);
        run(100);
        assert(dash_ui_ota_countdown_visible() == n);
        digits[n - 1] = region(80, 249, 80, 56);
    }
    assert(digits[0] != digits[1] && digits[1] != digits[2] && digits[0] != digits[2]);
    dash_ui_set_lang(DASH_LANG_EN);
    run(100);
    assert(dash_ui_ota_countdown_visible() == 1);
    assert(dash_ui_get_page() == DASH_PAGE_TOOLS && dash_ui_get_tool() == DASH_TOOL_CALENDAR);
    state.ota.phase = DASH_OTA_SUCCEEDED;
    dash_ui_update(&state, 1794844800);
    run(100);
    cat = region(56, 82, 128, 144);
    run(180);
    assert(cat != region(56, 82, 128, 144));
    run(8100);
    assert(!dash_ota_ui_active());
    dash_ui_update(&state, 1794844800);
    assert(!dash_ota_ui_active()); /* terminal state cannot recreate a dismissed overlay */
    assert(!dash_ui_alert_active()); /* a finished update releases the flight/notification gate */
    assert(dash_ui_get_page() == DASH_PAGE_TOOLS && dash_ui_get_tool() == DASH_TOOL_CALENDAR);
    state.ota.phase = DASH_OTA_FAILED;
    dash_ui_update(&state, 1794844800);
    run(100);
    assert(dash_ota_ui_active());
    state.ota.phase = DASH_OTA_IDLE;
    dash_ui_update(&state, 1794844800);
    assert(!dash_ota_ui_active());
    puts("OTA digits, animated Kiki, language rebuild and overlay lifetime passed");
    return 0;
}
