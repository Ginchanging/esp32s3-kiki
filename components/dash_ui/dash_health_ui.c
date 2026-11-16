#include "dash_health_ui.h"
#include "dash_i18n.h"
#include "dash_theme.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(dash_font_zh12)
LV_FONT_DECLARE(dash_font_zh20)

/* Four objects (one drawing surface + three hit targets), rather than another
 * tree of cards/labels. Small LVGL objects use scarce internal RAM on the S3.
 * Text lives in static storage because LVGL draw tasks can outlive a callback. */
typedef struct {
    bool configured;
    int tab;
    char status[40], note[112], values[4][32];
    uint32_t graph_hash;
} presentation_t;

static struct {
    lv_obj_t *panel;
    const dash_health_t *data;
    int64_t now;
    bool clock_valid;
    int tab;
    presentation_t view;
} health;

static const dash_str_t names[3][4] = {
    {S_HEALTH_STEPS, S_HEALTH_DISTANCE, S_HEALTH_EXERCISE, S_HEALTH_ENERGY},
    {S_HEALTH_LAST_SLEEP, S_HEALTH_CORE, S_HEALTH_DEEP, S_HEALTH_REM},
    {S_HEALTH_DELTA, S_HEALTH_CHANGE, S_HEALTH_FAT, S_HEALTH_SPAN},
};

static const lv_font_t *small(void)
{
    return dash_i18n_get() == DASH_LANG_ZH ? &dash_font_zh12 : &lv_font_montserrat_12;
}

static void rect(lv_layer_t *layer, int x, int y, int w, int h, uint32_t color, int radius)
{
    if (w <= 0 || h <= 0) return;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_area_t a = {x, y, x+w-1, y+h-1};
    lv_draw_rect(layer, &d, &a);
}

static void label(lv_layer_t *layer, int x, int y, int w, int h, const lv_font_t *font,
                  uint32_t color, const char *text, lv_text_align_t align)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.color = lv_color_hex(color);
    d.text = text;
    d.align = align;
    lv_area_t a = {x, y, x+w-1, y+h-1};
    lv_draw_label(layer, &d, &a);
}

static void graph(lv_layer_t *layer, int x, int y, int width, int height)
{
    const dash_health_t *s = health.data;
    if (!s) return;
    if (health.tab == 0) {
        int32_t maximum = 1;
        for (int i = 0; i < DASH_HEALTH_DAYS; ++i)
            if (s->activity[i][0] != DASH_HEALTH_NONE && s->activity[i][0] > maximum)
                maximum = s->activity[i][0];
        for (int i = 0; i < DASH_HEALTH_DAYS; ++i) {
            int bx = x+i*width/7+3, w = width/7-6;
            rect(layer, bx, y+height-1, w, 1, T_LINE, 0);
            if (s->activity[i][0] == DASH_HEALTH_NONE) continue;
            int h = 1+(int)((int64_t)s->activity[i][0]*(height-2)/maximum);
            rect(layer, bx, y+height-h, w, h, i == 6 ? T_ACCENT : T_GOOD, 0);
        }
    } else if (health.tab == 1) {
        int32_t total = s->values[DASH_HEALTH_SLEEP];
        if (total == DASH_HEALTH_NONE || total <= 0) return;
        static const uint32_t colors[] = {T_INFO, 0x9279DB, T_ACCENT};
        int used = 0;
        rect(layer, x, y+6, width, height-12, T_LINE, 0);
        for (int i = 0; i < 3; ++i) {
            int32_t v = s->values[DASH_HEALTH_CORE+i];
            if (v == DASH_HEALTH_NONE) continue;
            int w = (int)((int64_t)v*width/total);
            if (w > width-used) w = width-used;
            rect(layer, x+used, y+6, w, height-12, colors[i], 0);
            used += w;
        }
    } else {
        int lo = 0, hi = 0;
        for (int i = 0; i < DASH_HEALTH_WEIGHTS; ++i) {
            int v = s->weight_hist[i];
            if (v == DASH_HEALTH_WEIGHT_NONE) continue;
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (lo == hi) { --lo; ++hi; }
        int zero = y+height-3-(0-lo)*(height-5)/(hi-lo);
        rect(layer, x, zero, width, 1, T_LINE, 0);
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.color = lv_color_hex(T_ACCENT);
        line.width = 2;
        bool previous = false;
        for (int i = 0; i < DASH_HEALTH_WEIGHTS; ++i) {
            int v = s->weight_hist[i];
            if (v == DASH_HEALTH_WEIGHT_NONE) { previous = false; continue; }
            int px = x+2+i*(width-5)/29;
            int py = y+height-3-(v-lo)*(height-5)/(hi-lo);
            line.p2 = (lv_point_precise_t){px, py};
            if (previous) lv_draw_line(layer, &line);
            rect(layer, px-1, py-1, 3, 3, T_ACCENT, 0);
            line.p1 = line.p2;
            previous = true;
        }
    }
}

static void draw(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(health.panel, &a);
    int x = a.x1, y = a.y1;
    lv_layer_t *layer = lv_event_get_layer(e);
    const presentation_t *p = &health.view;
    const lv_font_t *title = dash_i18n_get() == DASH_LANG_ZH ? &dash_font_zh20 : &lv_font_montserrat_20;
    label(layer, x+10, y+4, 112, 24, title, T_TEXT, dash_tr(S_HEALTH_TITLE), LV_TEXT_ALIGN_LEFT);
    label(layer, x+120, y+10, 110, 18, small(), T_MUTED, p->status, LV_TEXT_ALIGN_RIGHT);
    for (int i = 0; i < 3; ++i) {
        rect(layer, x+10+i*75, y+32, 70, 27, i == p->tab ? T_LINE : T_CARD, 5);
        label(layer, x+10+i*75, y+39, 70, 18, small(), T_TEXT,
              dash_tr((dash_str_t)(S_HEALTH_MOVE+i)), LV_TEXT_ALIGN_CENTER);
    }
    label(layer, x+10, y+66, 220, 28, small(), T_MUTED, p->note, LV_TEXT_ALIGN_LEFT);
    if (!p->configured) {
        label(layer, x+20, y+115, 200, 100, small(), T_MUTED,
              dash_tr(S_HEALTH_SETUP_BODY), LV_TEXT_ALIGN_CENTER);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        int cx = x+10+(i%2)*113, cy = y+96+(i/2)*63;
        rect(layer, cx, cy, 107, 56, T_CARD, 8);
        label(layer, cx+8, cy+6, 91, 18, small(), T_MUTED,
              dash_tr(names[p->tab][i]), LV_TEXT_ALIGN_LEFT);
        label(layer, cx+8, cy+26, 91, 25, &lv_font_montserrat_20, T_TEXT,
              p->values[i], LV_TEXT_ALIGN_LEFT);
    }
    label(layer, x+10, y+224, 220, 18, small(), T_MUTED,
          dash_tr(p->tab == 0 ? S_HEALTH_STEPS_WEEK : p->tab == 1 ? S_HEALTH_STAGES : S_HEALTH_WEIGHT_TREND),
          LV_TEXT_ALIGN_LEFT);
    graph(layer, x+10, y+245, 220, 30);
}

static void render(void);

static void select_tab(lv_event_t *e)
{
    health.tab = (int)(intptr_t)lv_event_get_user_data(e);
    render();
}

void dash_health_ui_create(lv_obj_t *tile)
{
    health.panel = lv_obj_create(tile);
    lv_obj_remove_style_all(health.panel);
    lv_obj_set_pos(health.panel, 0, 0);
    lv_obj_set_size(health.panel, 240, 282);
    lv_obj_set_clickable(health.panel, false);
    lv_obj_set_scrollable(health.panel, false);
    lv_obj_add_event_cb(health.panel, draw, LV_EVENT_DRAW_MAIN, NULL);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *hit = lv_obj_create(tile);
        lv_obj_remove_style_all(hit);
        lv_obj_set_pos(hit, 10+i*75, 32);
        lv_obj_set_size(hit, 70, 27);
        lv_obj_set_scrollable(hit, false);
        lv_obj_set_clickable(hit, true);
        lv_obj_add_event_cb(hit, select_tab, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    render();
}

void dash_health_ui_destroy(void)
{
    int tab = health.tab;
    memset(&health, 0, sizeof(health));
    health.tab = tab;
}

static void duration(int32_t v, char *buf, size_t n)
{
    if (v == DASH_HEALTH_NONE) snprintf(buf, n, "--");
    else snprintf(buf, n, "%ldh%02ld", (long)(v/60), (long)(v%60));
}

static void delta(int16_t v, char *buf, size_t n)
{
    if (v == DASH_HEALTH_WEIGHT_NONE) snprintf(buf, n, "--");
    else {
        int magnitude = v < 0 ? -v : v;
        snprintf(buf, n, "%s%d.%d kg", v < 0 ? "-" : "+", magnitude/10, magnitude%10);
    }
}

static void render(void)
{
    if (!health.panel) return;
    const dash_health_t *s = health.data;
    presentation_t next;
    memset(&next, 0, sizeof(next));
    next.tab = health.tab;
    next.configured = s && s->have && s->configured;
    int64_t newest = 0, at = 0;
    bool any = false;
    if (s) for (int i = 0; i < DASH_HEALTH_METRICS; ++i) {
        if (s->source_at[i] > newest) newest = s->source_at[i];
        if (s->values[i] != DASH_HEALTH_NONE) any = true;
        bool selected = health.tab == 0 ? i < 4 : health.tab == 1 ? i >= 4 && i < 8 : i == 8;
        if (selected && s->source_at[i] > at) at = s->source_at[i];
    }
    bool cached = next.configured && (s->error || !s->ok || health.now-s->fetched_at > 300);
    snprintf(next.status, sizeof(next.status), "%s", !next.configured ? dash_tr(S_HEALTH_SETUP) :
             cached ? dash_tr(S_CAL_CACHED) : !any || !newest || health.now-newest > 7200 ?
             dash_tr(S_HEALTH_PENDING) : "Apple Health");
    if (next.configured) {
        if (health.tab == 2 && s->weight_at > at) at = s->weight_at;
        if (!at) snprintf(next.note, sizeof(next.note), "%s", dash_tr(S_HEALTH_NO_VALUES));
        else {
            time_t t = (time_t)(at+DASH_TZ_OFFSET_S);
            struct tm tm;
            char when[24];
            gmtime_r(&t, &tm);
            strftime(when, sizeof(when), "%m-%d %H:%M", &tm);
            snprintf(next.note, sizeof(next.note), dash_tr(S_HEALTH_SYNC_FMT), when);
            if (health.tab) {
                size_t used = strlen(next.note);
                snprintf(next.note+used, sizeof(next.note)-used, "\n%s", dash_tr(S_HEALTH_DATE_UNKNOWN));
            }
        }
        for (int i = 0; i < 4; ++i) {
            char *buf = next.values[i];
            size_t n = sizeof(next.values[i]);
            if (health.tab == 0) {
                int32_t v = health.clock_valid ? dash_health_value(s, (dash_health_metric_t)i, health.now) : DASH_HEALTH_NONE;
                if (v == DASH_HEALTH_NONE) snprintf(buf, n, "--");
                else if (i == 1) snprintf(buf, n, "%ld.%02ld km", (long)(v/1000), (long)(v%1000/10));
                else snprintf(buf, n, "%ld%s", (long)v, i == 2 ? " min" : i == 3 ? " kcal" : "");
            } else if (health.tab == 1) duration(s->values[DASH_HEALTH_SLEEP+i], buf, n);
            else if (i < 2) delta(i ? s->weight_change : s->weight_delta, buf, n);
            else if (i == 2) {
                int32_t v = s->values[DASH_HEALTH_FAT];
                if (v == DASH_HEALTH_NONE) snprintf(buf, n, "--");
                else snprintf(buf, n, "%ld.%ld%%", (long)(v/10), (long)(v%10));
            } else if (!s->weight_span) snprintf(buf, n, "--");
            else snprintf(buf, n, "%u d", (unsigned)s->weight_span);
        }
        uint32_t hash = 2166136261u;
        const unsigned char *bytes = (const unsigned char *)s->values;
        for (size_t i = 0; i < sizeof(s->values); ++i) hash = (hash^bytes[i])*16777619u;
        bytes = (const unsigned char *)s->activity;
        for (size_t i = 0; i < sizeof(s->activity); ++i) hash = (hash^bytes[i])*16777619u;
        bytes = (const unsigned char *)s->weight_hist;
        for (size_t i = 0; i < sizeof(s->weight_hist); ++i) hash = (hash^bytes[i])*16777619u;
        next.graph_hash = hash;
    }
    if (memcmp(&health.view, &next, sizeof(next))) {
        health.view = next;
        lv_obj_invalidate(health.panel);
    }
}

void dash_health_ui_update(const dash_health_t *st, int64_t now, bool clock_valid)
{
    health.data = st;
    health.now = now;
    health.clock_valid = clock_valid;
    render();
}
