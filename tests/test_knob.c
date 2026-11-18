/* Exercise real shared LVGL interactions with a controllable monotonic clock. */
#include "dash_ui.h"
#include "dash_cat.h"
#include "dash_flight_ui.h"
#include "knob_button.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint32_t ticks = 10000;
static uint16_t pixels[240 * 320];
static dash_state_t state;
static const int64_t utc = 1794844800;
static int duration_saves, saved_duration, saved_count, saved_minutes;
static int mi_requests, mi_on, mi_bri;
static dash_action_query_t action_query;
static uint32_t action_seq;
static uint32_t clock_ms(void) { return ticks; }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{ (void)a; (void)px; lv_display_flush_ready(d); }
static void run(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 20) { ticks += 20; lv_timer_handler(); }
    lv_refr_now(NULL);
}
static void update(void) { dash_ui_update(&state, utc); }
static void duration_saved(int m) { ++duration_saves; saved_duration = m; }
static void totals_saved(int n, int m, int day)
{ (void)day; saved_count = n; saved_minutes = m; }
static void mi_request(const char *dev, int on, int bri)
{ assert(!strcmp(dev, "lamp")); ++mi_requests; mi_on = on; mi_bri = bri; }
static uint32_t action_request(const dash_action_query_t *query)
{
    action_query = query ? *query : (dash_action_query_t){0};
    return ++action_seq;
}
static lv_obj_t *find_label(lv_obj_t *o, const char *text)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), text)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *found = find_label(lv_obj_get_child(o, i), text);
        if (found) return found;
    }
    return NULL;
}
static bool text(const char *s) { return find_label(lv_screen_active(), s) != NULL; }
static uint8_t phase(uint8_t *percent)
{ bool night; uint8_t p; dash_ui_get_status(&night, &p, percent); return p; }
static uint32_t health_hash(void)
{
    lv_refr_now(NULL);
    uint32_t h = 2166136261u;
    for (int y = 56; y < 305; ++y)
        for (int x = 0; x < 240; ++x) h = (h ^ pixels[y*240+x]) * 16777619u;
    return h;
}
static void gesture(int dir, unsigned gap)
{
    for (int i = 0; i < 4; ++i) {
        dash_ui_knob(DASH_KNOB_TURN, i % 2 ? -dir : dir);
        if (i != 3) run(gap);
    }
}

int main(void)
{
    knob_button_t button = {0};
    assert(knob_button_update(&button, true, 0, 10) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, true, 0, 710) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, true, 1, 720) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, false, 0, 800) == KNOB_BUTTON_RELEASE);
    assert(knob_button_update(&button, true, 0, 1000) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, false, 0, 1100) == KNOB_BUTTON_CLICK);
    assert(knob_button_update(&button, true, 0, 2000) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, false, 0, 2600) == KNOB_BUTTON_LONG);
    assert(knob_button_update(&button, true, 0, UINT32_MAX-100) == KNOB_BUTTON_NONE);
    assert(knob_button_update(&button, false, 1, 600) == KNOB_BUTTON_RELEASE);

    lv_init(); lv_tick_set_cb(clock_ms);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    dash_ui_create(d, DASH_LANG_EN);
    dash_state_init(&state);
    state.clock_valid = true;
    dash_ui_set_pomo_duration_cb(duration_saved);
    dash_ui_set_pomo_cb(totals_saved);
    update();

    /* All pages remain reachable by holding, with wrap in either direction. */
    dash_ui_knob(DASH_KNOB_HOLD_TURN, -1); assert(dash_ui_get_page() == 8);
    dash_ui_knob(DASH_KNOB_HOLD_TURN, 10); assert(dash_ui_get_page() == 0);
    dash_ui_set_page(3, false);
    dash_ui_pomo_duration_restore(27); update(); assert(text("Focus: 25 min"));
    dash_ui_knob(DASH_KNOB_TURN, INT_MIN); assert(text("Focus: 15 min"));
    dash_ui_knob(DASH_KNOB_TURN, INT_MAX); assert(text("Focus: 60 min"));
    dash_ui_knob(DASH_KNOB_TURN, -9); assert(text("Focus: 15 min"));
    dash_ui_knob(DASH_KNOB_TURN, 1); run(500);
    dash_ui_knob(DASH_KNOB_TURN, 1); run(600); assert(duration_saves == 0);
    run(450); assert(duration_saves == 1 && saved_duration == 25);
    dash_ui_pomo_duration_restore(15); update();
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    uint8_t pct; assert(phase(&pct) == DASH_POMO_FOCUS && pct == 0);
    ticks += 300000; update(); assert(phase(&pct) == DASH_POMO_FOCUS && pct == 33);
    dash_ui_knob(DASH_KNOB_TURN, 9);
    assert(text("Next focus: 60 min") && phase(&pct) == DASH_POMO_FOCUS && pct == 33);
    dash_ui_knob(DASH_KNOB_CLICK, 0); assert(phase(&pct) == DASH_POMO_PAUSED);
    uint8_t paused = pct; ticks += 300000; update();
    assert(phase(&pct) == DASH_POMO_PAUSED && pct == paused);
    dash_ui_knob(DASH_KNOB_TURN, -9);
    dash_ui_set_lang(DASH_LANG_ZH); dash_ui_set_lang(DASH_LANG_EN);
    assert(text("Next focus: 15 min") && phase(&pct) == DASH_POMO_PAUSED && pct == paused);
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    ticks += 600000; update();
    assert(phase(&pct) == DASH_POMO_BREAK && saved_count == 1 && saved_minutes == 15);
    dash_ui_knob(DASH_KNOB_TURN, 9); assert(text("Next focus: 60 min"));
    ticks += 300001; update(); assert(phase(&pct) == DASH_POMO_IDLE);
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    ticks += 900000; update(); assert(phase(&pct) == DASH_POMO_FOCUS && pct == 25);
    dash_ui_set_page(1, false);
    dash_ui_knob(DASH_KNOB_LONG, 0); assert(phase(&pct) == DASH_POMO_PAUSED);
    dash_ui_knob(DASH_KNOB_LONG, 0); assert(phase(&pct) == DASH_POMO_FOCUS);

    /* Lamp bursts coalesce, and leaving commits before page navigation. */
    state.have_mi = state.mi_ok = true; state.mi_count = 1;
    state.mi[0] = (dash_mi_dev_t){.kind=DASH_MI_LAMP, .online=true, .on=true, .bri=50};
    snprintf(state.mi[0].id, sizeof(state.mi[0].id), "lamp");
    snprintf(state.mi[0].name_en, sizeof(state.mi[0].name_en), "Desk");
    dash_ui_set_mi_cb(mi_request); update(); dash_ui_set_page(5, false);
    dash_ui_knob(DASH_KNOB_TURN, 1); run(200);
    dash_ui_knob(DASH_KNOB_TURN, 2); run(200); assert(mi_requests == 0);
    run(300); assert(mi_requests == 1 && mi_on == 1 && mi_bri == 65);
    dash_ui_knob(DASH_KNOB_TURN, 1);
    dash_ui_knob(DASH_KNOB_HOLD_TURN, 1);
    assert(dash_ui_get_page() == 6 && mi_requests == 2 && mi_bri == 70);
    run(1000); assert(mi_requests == 2);
    state.mi[0].online = false; update(); dash_ui_set_page(5, false);
    dash_ui_knob(DASH_KNOB_TURN, 1); run(1000); assert(mi_requests == 2);
    state.mi[0].online = true; update();
    dash_ui_knob(DASH_KNOB_TURN, INT_MAX); run(600); assert(mi_bri == 100);
    dash_ui_knob(DASH_KNOB_TURN, INT_MIN); run(600); assert(mi_bri == 1);
    int before = mi_requests;
    dash_ui_knob(DASH_KNOB_CLICK, 0); assert(mi_requests == before+1 && mi_on == 0);

    /* Long mail is read continuously; fast detents accumulate instead of being lost. */
    state.muse_count = DASH_MAX_MUSE;
    for (int i = 0; i < DASH_MAX_MUSE; ++i) {
        state.muse[i].src = DASH_SRC_KIKI; state.muse[i].ts = utc;
        memset(state.muse[i].text, 'W', sizeof(state.muse[i].text)-1);
        state.muse[i].text[sizeof(state.muse[i].text)-1] = 0;
    }
    update(); dash_ui_set_page(4, false); run(300);
    lv_obj_t *body = find_label(lv_screen_active(), state.muse[0].text); assert(body);
    lv_obj_t *list = lv_obj_get_parent(lv_obj_get_parent(body));
    dash_ui_knob(DASH_KNOB_TURN, 1); dash_ui_knob(DASH_KNOB_TURN, 1);
    dash_ui_knob(DASH_KNOB_TURN, 1); run(1000);
    assert(dash_ui_get_page() == 4 && lv_obj_get_scroll_y(list) == 96);
    update(); assert(lv_obj_get_scroll_y(list) == 96);
    dash_ui_knob(DASH_KNOB_TURN, INT_MAX); run(1000);
    assert(lv_obj_get_scroll_bottom(list) == 0 && dash_ui_get_page() == 4);
    dash_ui_knob(DASH_KNOB_TURN, INT_MIN); run(1000); assert(lv_obj_get_scroll_y(list) == 0);
    state.muse_count = 0; update(); dash_ui_knob(DASH_KNOB_TURN, 20); run(1000);
    assert(lv_obj_get_scroll_y(list) == 0);

    state.flights.have = state.flights.configured = state.flights.ok = true;
    state.flights.fetched_at = utc; state.flights.count = 3;
    for (int i = 0; i < 3; ++i) {
        snprintf(state.flights.aircraft[i].hex, 7, "%06d", i+1);
        snprintf(state.flights.aircraft[i].flight, 9, "TEST%d", i+1);
        state.flights.aircraft[i].observed_at = utc;
    }
    update(); dash_ui_set_page(6, false); assert(text("TEST1"));
    dash_ui_knob(DASH_KNOB_TURN, 1); assert(text("TEST2") && dash_ui_get_page() == 6);
    dash_ui_knob(DASH_KNOB_TURN, -2); assert(text("TEST3"));
    dash_flight_t swap = state.flights.aircraft[0];
    state.flights.aircraft[0] = state.flights.aircraft[2]; state.flights.aircraft[2] = swap;
    update(); assert(text("TEST3")); /* reordering keeps the selected identity */
    state.flights.count = 1; update(); dash_ui_knob(DASH_KNOB_TURN, -20); assert(text("TEST3"));
    state.flights.count = 0; update(); dash_ui_knob(DASH_KNOB_TURN, 1); assert(dash_ui_get_page() == 6);

    dash_ui_set_page(8, false); run(300); uint32_t activity = health_hash();
    dash_ui_knob(DASH_KNOB_TURN, 1); run(300); uint32_t sleep = health_hash();
    dash_ui_knob(DASH_KNOB_TURN, 1); run(300); uint32_t body_hash = health_hash();
    assert(activity != sleep && sleep != body_hash && body_hash != activity);
    dash_ui_knob(DASH_KNOB_TURN, 1); run(300); assert(health_hash() == activity);
    dash_ui_knob(DASH_KNOB_TURN, -1); run(300); assert(health_hash() == body_hash);
    dash_ui_set_lang(DASH_LANG_ZH); dash_ui_set_lang(DASH_LANG_EN); run(300);
    assert(health_hash() == body_hash);

    /* Four alternating runs, using capture times, not queue-drain times. */
    dash_ui_set_page(0, false); run(8000);
    gesture(1, 400); assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    for (int i = 0; i < 4; ++i)
        dash_ui_knob_at(DASH_KNOB_TURN, i % 2 ? -1 : 1, ticks - (uint32_t)(3-i)*400);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE); /* queued events retain their real gaps */
    run(500);
    dash_ui_knob(DASH_KNOB_TURN, 3); dash_ui_knob(DASH_KNOB_TURN, 2);
    dash_ui_knob(DASH_KNOB_TURN, -1); dash_ui_knob(DASH_KNOB_TURN, 1);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    dash_ui_knob(DASH_KNOB_TURN, -1); assert(dash_cat_reacting() == DASH_CAT_REACT_PET);
    run(4100); gesture(-1, 100); assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    run(2100); gesture(-1, 100); assert(dash_cat_reacting() == DASH_CAT_REACT_PET);
    run(6200);
    dash_ui_knob_at(DASH_KNOB_TURN, 1, ticks-600);
    dash_ui_knob_at(DASH_KNOB_TURN, -1, ticks-2000); /* out-of-order/stale input */
    dash_ui_knob(DASH_KNOB_TURN, 1); dash_ui_knob(DASH_KNOB_TURN, -1);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    dash_ui_set_page(1, false); dash_ui_set_page(0, false);
    dash_ui_knob(DASH_KNOB_TURN, 1); dash_ui_knob(DASH_KNOB_TURN, -1);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    dash_ui_knob(DASH_KNOB_HOLD_TURN, 0);
    dash_ui_knob(DASH_KNOB_TURN, 1); dash_ui_knob(DASH_KNOB_TURN, -1);
    assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    dash_ui_show_muse("Priority", utc, 6000); gesture(1, 100);
    assert(dash_cat_reacting() == DASH_CAT_REACT_MUSE); run(8100);
    dash_ui_debug_flyover(); run(100); assert(dash_flight_ui_scene_active());
    gesture(-1, 100); assert(dash_cat_reacting() == DASH_CAT_REACT_NONE);
    dash_flight_ui_interrupt();
    state.ota = (dash_ota_status_t){.phase=DASH_OTA_DOWNLOADING, .received=42, .total=100};
    update(); gesture(1, 100); dash_ui_knob(DASH_KNOB_HOLD_TURN, 1);
    assert(dash_ui_get_page() == 0 && dash_cat_reacting() == DASH_CAT_REACT_NONE);
    state.ota.phase = DASH_OTA_IDLE; update(); run(3000);
    ticks = UINT32_MAX-200; gesture(-1, 100);
    assert(dash_cat_reacting() == DASH_CAT_REACT_PET); /* monotonic wraparound */

    dash_ui_set_action_cb(action_request);
    state.have_hub = true;
    state.run_count = DASH_MAX_RUNS;
    for (int i = 0; i < state.run_count; ++i) {
        state.runs[i] = (dash_run_t){.id = 100 + i, .attempt = 1, .state = DASH_RUN_SUCCESS};
    }
    update();
    dash_ui_set_page(1, false);
    dash_ui_knob(DASH_KNOB_TURN, 4);
    dash_run_t newer = state.runs[0];
    state.runs[0] = state.runs[4];
    state.runs[4] = newer;
    update();
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    assert(action_query.run_id == 104);
    uint32_t stale = action_seq;
    dash_ui_knob(DASH_KNOB_HOLD_TURN, 1);
    assert(dash_ui_get_page() == 2 && !action_query.run_id);
    dash_ui_set_page(1, false);
    dash_ui_knob(DASH_KNOB_TURN, 1);
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    assert(action_query.run_id == 101);
    dash_action_page_t reply = {.count = 1, .total = 1, .items = {{.id = 99, .name = "Test job"}}};
    dash_ui_action_reply(stale, &reply);
    assert(text("Loading...") && !text("Test job"));
    dash_ui_action_reply(action_seq, &reply);
    assert(text("Test job"));
    dash_ui_knob(DASH_KNOB_CLICK, 0);
    assert(action_query.run_id == 101 && action_query.job_id == 99);
    dash_ui_set_lang(DASH_LANG_ZH);
    assert(!action_query.run_id && !text("Test job"));
    puts("knob button, context, timing, pomo accounting, lamp, scroll, selection and priority passed");
    return 0;
}
