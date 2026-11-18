/* Pixel radar, temporary outdoor scene and low-priority, actionable flyover cards. */
#include "dash_flight_ui.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dash_ui.h"
#include "dash_cat.h"
#include "dash_i18n.h"
#include "dash_pix.h"
#include "dash_theme.h"
#include "sprites/dash_sprites.h"

LV_FONT_DECLARE(dash_font_zh12)
LV_FONT_DECLARE(dash_font_zh16)
LV_FONT_DECLARE(dash_font_zh20)

static struct {
    lv_obj_t *status, *place, *radar, *inner, *empty, *detail, *number, *airline, *route, *metrics, *note;
    lv_obj_t *prev, *next, *counter, *scene, *sun, *bite, *stars[9], *cloud[12], *plane, *trail, *range, *alert_range;
    lv_obj_t *horizon, *buildings[8], *ledge, *ledge_top, *shadow;
    lv_obj_t *marker[DASH_MAX_FLIGHTS], *notice;
    lv_point_precise_t arrows[DASH_MAX_FLIGHTS][4];
    lv_timer_t *timer;
    dash_pix_t cat, aircraft_img;
    const dash_flights_t *data; /* always points into dash_ui's stable snapshot */
    const lv_font_t *small, *body, *title;
    dash_flight_cursor_t cursor;
    dash_flyover_t queue[DASH_MAX_FLYOVERS], active;
    dash_flight_t selected;
    int queued, cat_look, scene_mode;
    bool have_selected, frozen, playing, pending_notice, busy;
    int64_t now, scene_now;
    uint32_t updated_tick, scene_start, notice_until, block_until;
} fl;

static lv_obj_t *box(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *text(lv_obj_t *p, int x, int y, int w, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, w);
    lv_obj_set_height(o, lv_font_get_line_height(font));
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(o, "");
    return o;
}

static void set_text(lv_obj_t *o, const char *s)
{
    if (strcmp(lv_label_get_text(o), s)) lv_label_set_text(o, s);
}

static const char *identity(const dash_flight_t *f)
{
    return f->flight[0] ? f->flight : f->callsign[0] ? f->callsign : f->registration[0] ? f->registration : f->hex;
}

static bool same(const dash_flight_t *a, const dash_flight_t *b)
{
    return !strcmp(a->hex, b->hex) && !strcmp(a->callsign, b->callsign);
}

static int64_t now(void)
{
    return fl.now + (uint32_t)(lv_tick_get()-fl.updated_tick)/1000;
}

static void route_text(const dash_flight_t *f, char *out, size_t n)
{
    if (!f->origin[0] || !f->destination[0]) {
        snprintf(out, n, "%s", dash_tr(S_FLIGHT_ROUTE_NONE));
    } else {
        snprintf(out, n, "%s > %s", dash_airport_city(f->origin, f->origin_city),
                 dash_airport_city(f->destination, f->destination_city));
    }
}

static void close_notice(void)
{
    if (fl.notice) lv_obj_delete(fl.notice);
    fl.notice = NULL;
}

static void open_flight(lv_event_t *e)
{
    fl.selected = fl.active.flight;
    fl.have_selected = fl.frozen = true;
    close_notice();
    dash_ui_set_page(6, true);
}

static void notice(void)
{
    close_notice();
    lv_obj_t *p = box(lv_layer_top(), 10, DASH_UI_H-126, 220, 100, T_CARD, T_RADIUS);
    lv_obj_set_clickable(p, true);
    lv_obj_set_style_border_side(p, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(p, 3, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(T_INFO), 0);
    lv_obj_set_style_outline_width(p, 2, 0);
    lv_obj_set_style_outline_color(p, lv_color_hex(T_SHADOW), 0);
    lv_obj_add_event_cb(p, open_flight, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(text(p, 10, 8, 200, fl.body, T_INFO), dash_tr(fl.active.demo ? S_FLIGHT_DEMO : S_FLIGHT_OVERHEAD));
    char s[128], route[104];
    route_text(&fl.active.flight, route, sizeof(route));
    snprintf(s, sizeof(s), "%s  %s", identity(&fl.active.flight), fl.active.flight.model);
    lv_label_set_text(text(p, 10, 30, 200, &dash_font_zh16, T_TEXT), s);
    lv_label_set_text(text(p, 10, 53, 200, &dash_font_zh16, T_TEXT), route);
    lv_label_set_text(text(p, 10, 79, 200, fl.small, T_MUTED), dash_tr(S_FLIGHT_OPEN));
    fl.notice = p;
    fl.notice_until = lv_tick_get()+8000;
    fl.pending_notice = false;
}

static void stop_scene(bool remind)
{
    if (fl.playing) lv_obj_set_hidden(fl.scene, true);
    if (remind && fl.playing) fl.pending_notice = true;
    if (fl.playing) dash_ui_flight_bubble(false);
    fl.playing = false;
}

void dash_flight_ui_interrupt(void)
{
    stop_scene(true);
    if (fl.notice) { close_notice(); fl.pending_notice = true; }
    fl.block_until = lv_tick_get()+1000;
}

void dash_flight_ui_page_changed(void)
{
    if (fl.playing && dash_ui_get_page() != 0) stop_scene(true);
}

bool dash_flight_ui_scene_active(void) { return fl.playing; }

static void scene_sky(void)
{
    time_t t = (time_t)(fl.scene_now + DASH_TZ_OFFSET_S);
    struct tm tm;
    gmtime_r(&t, &tm);
    int h = tm.tm_hour;
    int mode = h >= 5 && h < 7 ? 0 : h >= 7 && h < 17 ? 1 : h >= 17 && h < 19 ? 2 : 3;
    if (mode == fl.scene_mode) return;
    fl.scene_mode = mode;
    static const uint32_t sky[4][2] = {{0x7886AB,0xF4C4B0},{0x83BFD7,0xDBE9DE},
                                       {0x7774AA,0xEDAF97},{0x202641,0x68628A}};
    /* Distant roofs, near roofs, sunlit parapet and its shaded front. */
    static const uint32_t land[4][4] = {{0xA39BAA,0x7C7C93,0xE0C1AE,0xAD8D88},
                                        {0xA0BCB9,0x779C99,0xE1D3BA,0xAAB2A0},
                                        {0x9583A1,0x65617F,0xDAB299,0x9C7B83},
                                        {0x575579,0x3E435F,0x9490A7,0x625E7B}};
    static const uint32_t clouds[4] = {0xF9DBCE,0xF2F5E9,0xEDC0BD,0x9A91B4};
    lv_obj_set_style_bg_color(fl.scene, lv_color_hex(sky[mode][0]), 0);
    lv_obj_set_style_bg_grad_color(fl.scene, lv_color_hex(sky[mode][1]), 0);
    lv_obj_set_style_bg_color(fl.sun, lv_color_hex(mode == 3 ? 0xF1E8D8 : mode == 1 ? 0xFFF0BD : 0xFFD2AD), 0);
    lv_obj_set_y(fl.sun, mode == 0 || mode == 2 ? 86 : 18);
    lv_obj_set_y(fl.bite, mode == 0 || mode == 2 ? 84 : 16);
    lv_obj_set_style_bg_color(fl.bite, lv_color_hex(sky[mode][0]), 0);
    lv_obj_set_hidden(fl.bite, mode != 3);
    for (int i = 0; i < 9; ++i) lv_obj_set_hidden(fl.stars[i], mode != 3);
    for (int i = 0; i < 12; ++i) {
        lv_obj_set_style_bg_color(fl.cloud[i], lv_color_hex(clouds[mode]), 0);
        lv_obj_set_style_bg_opa(fl.cloud[i], mode == 3 ? LV_OPA_20 : LV_OPA_60, 0);
    }
    lv_obj_set_style_bg_color(fl.horizon, lv_color_hex(land[mode][1]), 0);
    for (int i = 0; i < 8; ++i)
        lv_obj_set_style_bg_color(fl.buildings[i], lv_color_hex(land[mode][i%2]), 0);
    lv_obj_set_style_bg_color(fl.ledge_top, lv_color_hex(land[mode][2]), 0);
    lv_obj_set_style_bg_color(fl.ledge, lv_color_hex(land[mode][3]), 0);
    lv_obj_set_style_bg_color(fl.shadow, lv_color_hex(land[mode][1]), 0);
    if (fl.cat.img) {
        lv_obj_set_style_image_recolor(fl.cat.img, lv_color_hex(sky[mode][0]), 0);
        lv_obj_set_style_image_recolor_opa(fl.cat.img, mode == 3 ? LV_OPA_20 : LV_OPA_TRANSP, 0);
    }
}

static void scene_cat(int look)
{
    if (!fl.cat.px || look == fl.cat_look) return;
    fl.cat_look = look;
    dash_pix_clear(&fl.cat, 0);
    dash_cat_draw_look_into(&fl.cat, look, 0, 0, 2);
    dash_pix_commit(&fl.cat);
}

static void timer(lv_timer_t *t)
{
    if (fl.notice && (int32_t)(lv_tick_get()-fl.notice_until) >= 0) close_notice();
    if (dash_ui_alert_active()) { if (fl.playing || fl.notice) dash_flight_ui_interrupt(); return; }
    if ((int32_t)(lv_tick_get()-fl.block_until) < 0) return;
    if (fl.playing) {
        if (dash_ui_get_page() != 0) { stop_scene(true); return; }
        uint32_t elapsed = lv_tick_get()-fl.scene_start;
        if (elapsed >= 6000) { stop_scene(true); return; }
        int x = -36 + (int)((elapsed > 500 ? elapsed-500 : 0)*312/4000);
        bool east = fl.active.flight.heading < 0 || fl.active.flight.heading < 180;
        if (!east) x = 240-32-x;
        /* A shallow arc keeps the pass above Kiki's gaze and clear of the horizon. */
        int centre = x + 16;
        int y = 27 + (centre-120)*(centre-120)/1600;
        lv_obj_set_pos(fl.plane, x, y);
        lv_obj_set_pos(fl.trail, east ? x-17 : x+32, y+8);
        lv_obj_set_hidden(fl.plane, elapsed < 500 || elapsed > 4500);
        lv_obj_set_hidden(fl.trail, elapsed < 500 || elapsed > 4500);
        /* Kiki sits still throughout the pass; only the tail sways slowly. */
        static const int tail[] = {DASH_LOOK_SKY_BACK, DASH_LOOK_SKY_TAIL_MID,
                                   DASH_LOOK_SKY_TAIL_UP, DASH_LOOK_SKY_TAIL_MID};
        scene_cat(tail[(elapsed/450)%4]);
        return;
    }
    if (fl.pending_notice) {
        if (now()-fl.active.at <= 60) notice();
        else fl.pending_notice = false;
        return;
    }
    if (fl.notice || !fl.queued) return;
    fl.active = fl.queue[0];
    memmove(fl.queue, fl.queue+1, (size_t)(--fl.queued)*sizeof(fl.queue[0]));
    if (now()-fl.active.at > 60) return;
    if (dash_ui_get_page() != 0) { notice(); return; }
    fl.scene_start = lv_tick_get();
    fl.playing = true;
    fl.cat_look = -1;
    scene_cat(DASH_LOOK_SKY_BACK);
    if (fl.aircraft_img.px) lv_image_set_rotation(fl.plane,
        fl.active.flight.heading >= 180 ? 1800 : 0);
    scene_sky();
    dash_ui_flight_bubble(true);
    lv_obj_set_hidden(fl.scene, false);
}

static void select_index(int index)
{
    if (!fl.data || index < 0 || index >= fl.data->count) return;
    fl.selected = fl.data->aircraft[index];
    fl.have_selected = true;
    fl.frozen = false;
}

static void marker_clicked(lv_event_t *e)
{
    select_index((int)(intptr_t)lv_event_get_user_data(e));
    if (fl.data) dash_flight_ui_update(fl.data, now(), fl.scene_now, fl.busy);
}

void dash_flight_ui_step(int delta)
{
    if (!fl.data || !fl.data->count || !delta) return;
    int index = -1;
    for (int i = 0; i < fl.data->count; ++i) if (same(&fl.selected, &fl.data->aircraft[i])) index = i;
    if (index < 0) index = delta > 0 ? -1 : 0;
    int n = fl.data->count;
    select_index(((index + delta % n) % n + n) % n);
    dash_flight_ui_update(fl.data, now(), fl.scene_now, fl.busy);
}

static void next_clicked(lv_event_t *e)
{
    dash_flight_ui_step((int)(intptr_t)lv_event_get_user_data(e));
}

static void plane_art(dash_pix_t *p)
{
    static const char *const art[] = {
        "........##......", ".......###......", "..#...####......",
        "..##..####......", ".##############.", "################",
        "..##..####......", "..#....###......", "........##......"
    };
    dash_pix_clear(p, 0);
    for (int y = 0; y < 9; ++y) for (int x = 0; x < 16; ++x)
        if (art[y][x] == '#') dash_pix_rect(p, x*2, y*2, 2, 2, dash_argb(T_TEXT));
    dash_pix_rect(p, 26, 8, 4, 2, dash_argb(T_INFO));
    dash_pix_rect(p, 2, 8, 2, 4, dash_argb(T_ACCENT));
    dash_pix_commit(p);
}

void dash_flight_ui_create(lv_obj_t *tile, lv_obj_t *home)
{
    dash_flight_cursor_t cursor = fl.cursor;
    dash_flight_t selected = fl.selected;
    bool have_selected = fl.have_selected;
    memset(&fl, 0, sizeof(fl));
    fl.cursor = cursor;
    fl.selected = selected;
    fl.have_selected = have_selected;
    bool zh = dash_i18n_get() == DASH_LANG_ZH;
    fl.small = zh ? &dash_font_zh12 : &lv_font_montserrat_12;
    fl.body = zh ? &dash_font_zh16 : &lv_font_montserrat_14;
    fl.title = zh ? &dash_font_zh20 : &lv_font_montserrat_20;
    fl.scene_mode = -1;
    lv_label_set_text(text(tile, 10, 2, 148, fl.title, T_TEXT), dash_tr(S_FLIGHT_TITLE));
    fl.status = text(tile, 152, 8, 78, fl.small, T_MUTED);
    lv_obj_set_style_text_align(fl.status, LV_TEXT_ALIGN_RIGHT, 0);
    /* Place names (e.g. 榕园) may contain Chinese in either language. */
    fl.place = text(tile, 10, 27, 220, &dash_font_zh16, T_MUTED);
    fl.radar = box(tile, 10, 51, 220, 100, T_CARD, T_RADIUS);
    lv_obj_t *ring = box(fl.radar, 67, 7, 86, 86, T_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_border_color(ring, lv_color_hex(T_LINE), 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    box(fl.radar, 69, 49, 82, 1, T_LINE, 0);
    box(fl.radar, 109, 9, 1, 82, T_LINE, 0);
    fl.inner = box(fl.radar, 99, 39, 22, 22, T_BG, LV_RADIUS_CIRCLE);
    lv_obj_set_style_bg_opa(fl.inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(fl.inner, lv_color_hex(T_ACCENT), 0);
    lv_obj_set_style_border_width(fl.inner, 1, 0);
    box(fl.radar, 108, 48, 4, 4, T_ACCENT, 0);
    lv_label_set_text(text(fl.radar, 105, 0, 14, &lv_font_montserrat_12, T_MUTED), "N");
    fl.range = text(fl.radar, 8, 8, 50, &lv_font_montserrat_12, T_MUTED);
    fl.alert_range = text(fl.radar, 172, 78, 45, &lv_font_montserrat_12, T_ACCENT);
    lv_label_set_text(text(fl.radar, 8, 78, 60, &lv_font_montserrat_12, T_MUTED), "adsb.fi");
    for (int i = 0; i < DASH_MAX_FLIGHTS; ++i) {
        lv_obj_t *m = lv_line_create(fl.radar);
        fl.marker[i] = m;
        lv_obj_set_size(m, 12, 12);
        lv_obj_set_style_line_width(m, 2, 0);
        lv_obj_set_clickable(m, true);
        lv_obj_set_ext_click_area(m, 7);
        lv_obj_add_event_cb(m, marker_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_hidden(m, true);
    }
    fl.detail = box(tile, 10, 159, 220, 88, T_CARD, T_RADIUS);
    fl.number = text(fl.detail, 10, 5, 100, &lv_font_montserrat_20, T_ACCENT);
    fl.airline = text(fl.detail, 112, 11, 98, fl.small, T_MUTED);
    fl.route = text(fl.detail, 10, 32, 200, fl.body, T_TEXT);
    fl.metrics = text(fl.detail, 10, 55, 200, &lv_font_montserrat_12, T_TEXT);
    fl.note = text(fl.detail, 10, 72, 200, fl.small, T_MUTED);
    fl.empty = text(tile, 20, 178, 200, fl.body, T_MUTED);
    lv_obj_set_height(fl.empty, 60);
    lv_label_set_long_mode(fl.empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(fl.empty, LV_TEXT_ALIGN_CENTER, 0);
    fl.prev = box(tile, 10, 254, 87, 26, T_CARD, 6);
    fl.next = box(tile, 143, 254, 87, 26, T_CARD, 6);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *b = i ? fl.next : fl.prev;
        lv_obj_set_clickable(b, true);
        lv_obj_add_event_cb(b, next_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(i ? 1 : -1));
        lv_obj_t *l = text(b, 0, 4, 87, fl.small, T_TEXT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(l, dash_tr(i ? S_FLIGHT_NEXT : S_FLIGHT_PREV));
    }
    fl.counter = text(tile, 99, 261, 42, &lv_font_montserrat_12, T_MUTED);
    lv_obj_set_style_text_align(fl.counter, LV_TEXT_ALIGN_CENTER, 0);

    fl.scene = box(home, 0, 0, 240, 180, 0x83BFD7, 0);
    lv_obj_set_style_bg_grad_dir(fl.scene, LV_GRAD_DIR_VER, 0);
    fl.sun = box(fl.scene, 198, 18, 20, 20, T_WARN, LV_RADIUS_CIRCLE);
    fl.bite = box(fl.scene, 204, 16, 17, 17, 0x202641, LV_RADIUS_CIRCLE);
    for (int i = 0; i < 9; ++i) fl.stars[i] = box(fl.scene, 12+(i*31)%215, 9+(i*17)%70, 2, 2, T_TEXT, 0);
    /* Low, stepped cloud banks keep the plane's path open. */
    static const int clouds[12][4] = {{8,69,60,4},{16,63,42,6},{24,57,23,6},
        {37,61,28,6},{53,67,33,4},{69,71,25,3},
        {148,92,69,4},{157,86,48,6},{166,80,24,6},
        {180,84,28,6},{198,90,30,4},{219,94,21,3}};
    for (int i = 0; i < 12; ++i)
        fl.cloud[i] = box(fl.scene, clouds[i][0], clouds[i][1], clouds[i][2], clouds[i][3], 0xF2F5E9, 0);
    static const int roofs[8][4] = {{0,139,24,25},{24,145,18,19},{42,134,28,30},{70,142,25,22},
        {146,141,22,23},{168,133,29,31},{197,144,20,20},{217,137,23,27}};
    for (int i = 0; i < 8; ++i)
        fl.buildings[i] = box(fl.scene, roofs[i][0], roofs[i][1], roofs[i][2], roofs[i][3], 0xA0BCB9, 0);
    fl.horizon = box(fl.scene, 0, 154, 240, 10, 0x779C99, 0);
    fl.ledge = box(fl.scene, 0, 164, 240, 16, 0xAAB2A0, 0);
    fl.ledge_top = box(fl.scene, 0, 164, 240, 4, 0xE1D3BA, 0);
    fl.shadow = box(fl.scene, 103, 162, 44, 3, 0x779C99, LV_RADIUS_CIRCLE);
    if (dash_pix_create(&fl.cat, fl.scene, 64, 64)) lv_obj_set_pos(fl.cat.img, 88, 102);
    fl.trail = box(fl.scene, 0, 29, 15, 2, T_TEXT, 0);
    lv_obj_set_style_bg_opa(fl.trail, LV_OPA_30, 0);
    if (dash_pix_create(&fl.aircraft_img, fl.scene, 32, 18)) {
        plane_art(&fl.aircraft_img);
        fl.plane = fl.aircraft_img.img;
    } else fl.plane = box(fl.scene, 0, 22, 32, 6, T_TEXT, 0);
    lv_obj_set_hidden(fl.scene, true);
    fl.timer = lv_timer_create(timer, 50, NULL);
}

void dash_flight_ui_destroy(void)
{
    stop_scene(false);
    if (fl.timer) lv_timer_delete(fl.timer);
    fl.timer = NULL;
    close_notice();
    dash_pix_destroy(&fl.cat);
    dash_pix_destroy(&fl.aircraft_img);
    fl.playing = fl.pending_notice = false;
    fl.data = NULL;
}

void dash_flight_ui_update(const dash_flights_t *st, int64_t utc, int64_t scene_now, bool busy)
{
    fl.data = st;
    fl.now = utc;
    fl.scene_now = scene_now;
    fl.updated_tick = lv_tick_get();
    fl.busy = busy;
    static dash_flyover_t events[DASH_MAX_FLYOVERS];
    int n = dash_flights_take_new(&fl.cursor, st, utc, events, DASH_MAX_FLYOVERS);
    for (int i = 0; i < n; ++i) dash_flight_ui_notify(&events[i]);
    dash_flight_status_t status = dash_flights_status(st, utc);
    uint32_t color = status == DASH_FLIGHT_LIVE ? T_GOOD : status == DASH_FLIGHT_WAIT || status == DASH_FLIGHT_DISABLED ? T_MUTED : T_WARN;
    lv_obj_set_style_text_color(fl.status, lv_color_hex(color), 0);
    set_text(fl.status, dash_tr(status == DASH_FLIGHT_LIVE ? S_LIVE : status == DASH_FLIGHT_STALE ? S_STALE :
                              status == DASH_FLIGHT_ERROR ? S_OFFLINE : S_NO_DATA));
    char buf[160], age[32];
    int radius = st->radius_m ? st->radius_m : 20000;
    int alert = st->alert_radius_m ? st->alert_radius_m : 3000;
    snprintf(buf, sizeof(buf), "%d km", radius/1000);
    set_text(fl.range, buf);
    snprintf(buf, sizeof(buf), "%d km", alert/1000);
    set_text(fl.alert_range, buf);
    int r = 43*alert/radius;
    lv_obj_set_pos(fl.inner, 110-r, 50-r);
    lv_obj_set_size(fl.inner, r*2, r*2);
    if (st->fetched_at) dash_fmt_ago(utc > st->fetched_at ? utc-st->fetched_at : 0, dash_i18n_get(), age, sizeof(age));
    else age[0] = 0;
    snprintf(buf, sizeof(buf), "%s  %s", st->place, age);
    set_text(fl.place, buf);
    int selected_index = -1;
    if (fl.have_selected) {
        for (int i = 0; i < st->count; ++i) if (same(&fl.selected, &st->aircraft[i])) {
            fl.selected = st->aircraft[i]; selected_index = i; fl.frozen = false; break;
        }
    }
    if (selected_index < 0 && !fl.frozen) {
        if (st->count) { select_index(0); selected_index = 0; }
        else fl.have_selected = false;
    }
    for (int i = 0; i < DASH_MAX_FLIGHTS; ++i) {
        lv_obj_set_hidden(fl.marker[i], i >= st->count);
        if (i >= st->count) continue;
        const dash_flight_t *f = &st->aircraft[i];
        double bearing = f->bearing * .017453292519943295;
        double heading = (f->heading < 0 ? 0 : f->heading)*.017453292519943295;
        double distance = 40.0 * f->distance_m / (st->radius_m ? st->radius_m : 20000);
        if (distance > 40) distance = 40;
        int x = 110+(int)lround(sin(bearing)*distance), y = 50-(int)lround(cos(bearing)*distance);
        lv_obj_set_pos(fl.marker[i], x-6, y-6);
        static const int xy[4][2] = {{-4,3},{0,-4},{4,3},{0,1}};
        for (int k = 0; k < 4; ++k) {
            fl.arrows[i][k].x = 6+(int)lround(xy[k][0]*cos(heading)-xy[k][1]*sin(heading));
            fl.arrows[i][k].y = 6+(int)lround(xy[k][0]*sin(heading)+xy[k][1]*cos(heading));
        }
        lv_line_set_points(fl.marker[i], fl.arrows[i], 4);
        lv_obj_set_style_line_color(fl.marker[i], lv_color_hex(status != DASH_FLIGHT_LIVE || utc-f->observed_at > 30 ? T_GREY :
                                                               i == selected_index ? T_ACCENT : T_INFO), 0);
    }
    lv_obj_set_hidden(fl.detail, !fl.have_selected);
    lv_obj_set_hidden(fl.empty, fl.have_selected);
    lv_obj_set_hidden(fl.prev, st->count < 2);
    lv_obj_set_hidden(fl.next, st->count < 2);
    snprintf(buf, sizeof(buf), "%d/%d", selected_index+1, st->count);
    set_text(fl.counter, fl.frozen ? "LAST" : st->count ? buf : "");
    if (!fl.have_selected) {
        dash_str_t s = status == DASH_FLIGHT_DISABLED ? S_FLIGHT_DISABLED : status == DASH_FLIGHT_ERROR ? S_FLIGHT_LOST :
                       status == DASH_FLIGHT_STALE ? S_FLIGHT_OLD : status == DASH_FLIGHT_WAIT ? S_FLIGHT_WAIT : S_FLIGHT_NONE;
        set_text(fl.empty, dash_tr(s));
        return;
    }
    const dash_flight_t *f = &fl.selected;
    set_text(fl.number, identity(f));
    set_text(fl.airline, f->airline[0] ? f->airline : f->callsign);
    route_text(f, buf, sizeof(buf));
    set_text(fl.route, buf);
    char altitude[24];
    if (f->altitude_m == DASH_FLIGHT_UNKNOWN) snprintf(altitude, sizeof(altitude), "-- m");
    else snprintf(altitude, sizeof(altitude), "%ld m", (long)f->altitude_m);
    snprintf(buf, sizeof(buf), "%s   %s   %.1f km", f->model[0] ? f->model : "--", altitude, f->distance_m/1000.0);
    set_text(fl.metrics, buf);
    const char *note = fl.frozen ? dash_tr(S_FLIGHT_PASSED) : status == DASH_FLIGHT_ERROR ? dash_tr(S_FLIGHT_LOST) :
                       status == DASH_FLIGHT_STALE || utc-f->observed_at > 30 ? dash_tr(S_FLIGHT_OLD) :
                       !f->eligible ? dash_tr(S_FLIGHT_UNCLASSIFIED) : dash_tr(S_FLIGHT_REFERENCE);
    set_text(fl.note, note);
    if (fl.frozen) {
        char when[16];
        dash_fmt_hhmm(f->observed_at, when, sizeof(when));
        snprintf(buf, sizeof(buf), "%s  %s", note, when);
        set_text(fl.note, buf);
    }
}

void dash_flight_ui_notify(const dash_flyover_t *event)
{
    if (fl.queued == DASH_MAX_FLYOVERS) return;
    for (int i = 0; i < fl.queued; ++i) if (fl.queue[i].id == event->id) return;
    fl.queue[fl.queued++] = *event;
}

void dash_flight_ui_debug(void)
{
    dash_flyover_t e = {.id = (int64_t)lv_tick_get()+1, .at = now(), .demo = true};
    if (fl.data && fl.data->count) e.flight = fl.data->aircraft[0];
    else {
        snprintf(e.flight.hex, sizeof(e.flight.hex), "ffffff");
        snprintf(e.flight.callsign, sizeof(e.flight.callsign), "TEST001");
        snprintf(e.flight.model, sizeof(e.flight.model), "A321");
        snprintf(e.flight.origin, sizeof(e.flight.origin), "ZUH");
        snprintf(e.flight.destination, sizeof(e.flight.destination), "WUX");
        e.flight.distance_m = 3200;
        e.flight.altitude_m = 10800;
        e.flight.heading = 90;
        e.flight.observed_at = now();
    }
    dash_flight_ui_notify(&e);
}
