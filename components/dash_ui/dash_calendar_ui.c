#include "dash_calendar_ui.h"
#include "dash_i18n.h"
#include "dash_theme.h"
#include "dash_tools_ui.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

LV_FONT_DECLARE(dash_font_zh12)
LV_FONT_DECLARE(dash_font_zh16)
LV_FONT_DECLARE(dash_font_zh20)

static dash_ui_calendar_cb_t query_cb;
static struct {
    lv_obj_t *wall_day, *wall_dot, *status, *date, *list, *empty, *counter, *prev, *next;
    lv_obj_t *days[7], *numbers[7], *marks[7];
    lv_obj_t *rows[8], *times[8], *titles[8], *sources[8], *detail;
    const dash_calendar_t *data;
    int32_t today, day;
    uint16_t offset;
    int64_t now;
    bool clock_valid, active;
    int32_t drawn_day;
    uint16_t drawn_offset;
} cal = {.day = -1, .drawn_day = -1};

static const lv_font_t *small(void) { return dash_i18n_get() == DASH_LANG_ZH ? &dash_font_zh12 : &lv_font_montserrat_12; }

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    return o;
}

static lv_obj_t *label(lv_obj_t *p, int x, int y, int w, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, lv_font_get_line_height(f));
    lv_label_set_long_mode(o, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    lv_label_set_text(o, "");
    return o;
}

static void text(lv_obj_t *o, const char *value)
{
    if (strcmp(lv_label_get_text(o), value)) lv_label_set_text(o, value);
}

static void color(lv_obj_t *o, uint32_t value)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), lv_color_hex(value)))
        lv_obj_set_style_bg_color(o, lv_color_hex(value), 0);
}

static void show(lv_obj_t *o, bool yes)
{
    if (lv_obj_is_hidden(o) == yes) lv_obj_set_hidden(o, !yes);
}

static bool matching(void)
{
    return cal.data && cal.data->have && cal.data->day == cal.day && cal.data->offset == cal.offset;
}

static void render(void);

static void request(void)
{
    if (cal.clock_valid && query_cb) query_cb(cal.day, cal.offset);
    render();
}

static void open_page(lv_event_t *e) { dash_ui_open_tool(DASH_TOOL_CALENDAR); }

static void select_day(lv_event_t *e)
{
    if (!cal.clock_valid) return;
    cal.day = cal.today+(int)(intptr_t)lv_event_get_user_data(e);
    cal.offset = 0;
    request();
}

static void turn_batch(lv_event_t *e)
{
    if (!matching()) return;
    int offset = cal.offset+(int)(intptr_t)lv_event_get_user_data(e)*8;
    if (offset < 0 || (offset && offset >= cal.data->total)) return;
    cal.offset = (uint16_t)offset;
    request();
}

static void close_detail(lv_event_t *e)
{
    if (cal.detail) lv_obj_delete(cal.detail);
    cal.detail = NULL;
}

static void event_time(const dash_calendar_event_t *event, char *buf, size_t n)
{
    if (event->all_day) { snprintf(buf, n, "%s", dash_tr(S_CAL_ALL_DAY)); return; }
    int64_t lo = (int64_t)cal.day*86400-DASH_TZ_OFFSET_S;
    char a[8], b[8];
    if (event->start < lo) snprintf(a, sizeof(a), "%s", "<");
    else dash_fmt_hhmm(event->start, a, sizeof(a));
    if (event->end > lo+86400) snprintf(b, sizeof(b), "%s", ">");
    else if (event->end == lo+86400) snprintf(b, sizeof(b), "24:00");
    else dash_fmt_hhmm(event->end, b, sizeof(b));
    snprintf(buf, n, "%s-%s", a, b);
}

static void open_detail(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (!matching() || i >= cal.data->count) return;
    close_detail(NULL);
    const dash_calendar_event_t *event = &cal.data->events[i];
    cal.detail = box(lv_layer_top(), 0, 0, 240, 320, T_BG);
    lv_obj_set_clickable(cal.detail, true);
    lv_obj_add_event_cb(cal.detail, close_detail, LV_EVENT_CLICKED, NULL);
    char buf[80], a[24], b[24];
    dash_fmt_date(event->start, dash_i18n_get(), a, sizeof(a));
    dash_fmt_date(event->end-1, dash_i18n_get(), b, sizeof(b));
    event_time(event, buf, sizeof(buf));
    text(label(cal.detail, 16, 20, 208, small(), T_ACCENT), buf);
    text(label(cal.detail, 16, 44, 208, &dash_font_zh16, T_MUTED), a);
    if (strcmp(a, b)) text(label(cal.detail, 16, 65, 208, &dash_font_zh16, T_MUTED), b);
    lv_obj_t *title = label(cal.detail, 16, 100, 208, &dash_font_zh16, T_TEXT);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(title, LV_SIZE_CONTENT);
    text(title, event->title[0] ? event->title : dash_tr(S_CAL_UNTITLED));
    text(label(cal.detail, 16, 222, 208, &dash_font_zh16, T_MUTED), event->calendar);
    text(label(cal.detail, 16, 280, 208, small(), T_ACCENT), dash_tr(S_CAL_CLOSE));
}

void dash_calendar_ui_home(lv_obj_t *home)
{
    /* Clear wall between the plant and Kiki; create after Kiki's hit rectangle. */
    lv_obj_t *paper = box(home, 76, 112, 26, 22, T_TEXT);
    lv_obj_set_clickable(paper, true);
    lv_obj_set_ext_click_area(paper, 6);
    lv_obj_add_event_cb(paper, open_page, LV_EVENT_CLICKED, NULL);
    box(paper, 0, 0, 26, 5, T_BAD);
    box(paper, 5, 0, 2, 2, T_FRAME);
    box(paper, 19, 0, 2, 2, T_FRAME);
    cal.wall_day = label(paper, 1, 6, 24, &lv_font_montserrat_12, T_BG);
    lv_obj_set_style_text_align(cal.wall_day, LV_TEXT_ALIGN_CENTER, 0);
    cal.wall_dot = box(paper, 22, 17, 2, 2, T_ACCENT);
}

void dash_calendar_ui_create(lv_obj_t *tile)
{
    const lv_font_t *title = dash_i18n_get() == DASH_LANG_ZH ? &dash_font_zh20 : &lv_font_montserrat_20;
    text(label(tile, DASH_TOOL_TITLE_X, 2, 92, title, T_TEXT), dash_tr(S_CAL_TITLE));
    cal.status = label(tile, 143, 9, 87, small(), T_MUTED);
    lv_obj_set_style_text_align(cal.status, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 7; ++i) {
        cal.days[i] = box(tile, 10+i*32, 31, 28, 42, T_CARD);
        lv_obj_set_style_radius(cal.days[i], 5, 0);
        lv_obj_set_clickable(cal.days[i], true);
        lv_obj_add_event_cb(cal.days[i], select_day, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        cal.numbers[i] = label(cal.days[i], 0, 1, 28, small(), T_TEXT);
        lv_obj_set_height(cal.numbers[i], 34);
        lv_obj_set_style_text_align(cal.numbers[i], LV_TEXT_ALIGN_CENTER, 0);
        cal.marks[i] = box(cal.days[i], 12, 37, 4, 2, T_ACCENT);
    }
    cal.date = label(tile, 10, 80, 220, small(), T_ACCENT);
    cal.list = box(tile, 10, 100, 220, 146, T_BG);
    lv_obj_set_scrollable(cal.list, true);
    lv_obj_set_scroll_dir(cal.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(cal.list, LV_SCROLLBAR_MODE_AUTO);
    for (int i = 0; i < 8; ++i) {
        cal.rows[i] = box(cal.list, 0, i*54, 218, 50, T_CARD);
        lv_obj_set_style_radius(cal.rows[i], 6, 0);
        lv_obj_set_clickable(cal.rows[i], true);
        lv_obj_add_event_cb(cal.rows[i], open_detail, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        cal.times[i] = label(cal.rows[i], 8, 5, 90, small(), T_ACCENT);
        cal.sources[i] = label(cal.rows[i], 100, 3, 110, &dash_font_zh16, T_MUTED);
        lv_obj_set_style_text_align(cal.sources[i], LV_TEXT_ALIGN_RIGHT, 0);
        cal.titles[i] = label(cal.rows[i], 8, 26, 202, &dash_font_zh16, T_TEXT);
    }
    cal.empty = label(tile, 20, 143, 200, &dash_font_zh16, T_MUTED);
    lv_label_set_long_mode(cal.empty, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(cal.empty, 80);
    lv_obj_set_style_text_align(cal.empty, LV_TEXT_ALIGN_CENTER, 0);
    cal.prev = box(tile, 10, 254, 58, 26, T_CARD);
    cal.next = box(tile, 172, 254, 58, 26, T_CARD);
    lv_obj_set_clickable(cal.prev, true);
    lv_obj_set_clickable(cal.next, true);
    lv_obj_add_event_cb(cal.prev, turn_batch, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_event_cb(cal.next, turn_batch, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    text(label(cal.prev, 8, 6, 46, small(), T_TEXT), dash_tr(S_CAL_PREV));
    text(label(cal.next, 8, 6, 46, small(), T_TEXT), dash_tr(S_CAL_NEXT));
    cal.counter = label(tile, 70, 260, 100, &lv_font_montserrat_12, T_MUTED);
    lv_obj_set_style_text_align(cal.counter, LV_TEXT_ALIGN_CENTER, 0);
    cal.drawn_day = -1;
}

static void render(void)
{
    if (!cal.status) return;
    char buf[120];
    time_t t = (time_t)((int64_t)cal.today*86400);
    struct tm date;
    gmtime_r(&t, &date);
    if (cal.clock_valid) snprintf(buf, sizeof(buf), "%d", date.tm_mday);
    else snprintf(buf, sizeof(buf), "--");
    text(cal.wall_day, buf);
    bool week = cal.data && cal.data->have && cal.data->week_start == cal.today;
    show(cal.wall_dot, cal.clock_valid && week && cal.data->counts[0] > 0);
    for (int i = 0; i < 7; ++i) {
        t = (time_t)((int64_t)(cal.today+i)*86400);
        gmtime_r(&t, &date);
        if (cal.clock_valid) snprintf(buf, sizeof(buf), "%s\n%d", dash_tr((dash_str_t)(S_WD_SUN+date.tm_wday)), date.tm_mday);
        else snprintf(buf, sizeof(buf), "--");
        text(cal.numbers[i], buf);
        color(cal.days[i], cal.clock_valid && cal.day == cal.today+i ? T_LINE : T_CARD);
        show(cal.marks[i], cal.clock_valid && week && cal.data->counts[i] > 0);
    }
    if (cal.clock_valid) {
        dash_fmt_date((int64_t)cal.day*86400-DASH_TZ_OFFSET_S, dash_i18n_get(), buf, sizeof(buf));
        uint16_t holidays = dash_holidays_at((int64_t)cal.day*86400-DASH_TZ_OFFSET_S);
        int index = holidays & DASH_HOL_BIRTHDAY ? 10 : 0;
        while (index < 11 && !(holidays & (1u<<index))) ++index;
        if (index < 11) {
            size_t used = strlen(buf);
            snprintf(buf+used, sizeof(buf)-used, " / %s", dash_tr((dash_str_t)(S_HOL_SPRING+index)));
        }
    } else snprintf(buf, sizeof(buf), "%s", dash_tr(S_CAL_CLOCK));
    text(cal.date, buf);
    bool match = matching();
    bool fresh = cal.data && dash_calendar_fresh(cal.data, cal.now);
    bool failed = cal.data && (cal.data->error || cal.data->fetch_failures);
    text(cal.status, !cal.clock_valid ? dash_tr(S_CAL_CLOCK) : failed ? dash_tr(cal.data->fetched_at ? S_CAL_CACHED : S_OFFLINE) : !cal.data || !cal.data->have ? dash_tr(S_CAL_LOADING) :
         !cal.data->configured ? dash_tr(S_CAL_SETUP) : fresh ? "iCloud" : dash_tr(S_CAL_CACHED));
    int n = match && cal.clock_valid ? cal.data->count : 0;
    if (cal.drawn_day != cal.day || cal.drawn_offset != cal.offset) {
        lv_obj_scroll_to_y(cal.list, 0, LV_ANIM_OFF);
        cal.drawn_day = cal.day;
        cal.drawn_offset = cal.offset;
    }
    for (int i = 0; i < 8; ++i) {
        show(cal.rows[i], i < n);
        if (i >= n) continue;
        const dash_calendar_event_t *event = &cal.data->events[i];
        event_time(event, buf, sizeof(buf));
        text(cal.times[i], buf);
        text(cal.titles[i], event->title[0] ? event->title : dash_tr(S_CAL_UNTITLED));
        text(cal.sources[i], event->calendar);
    }
    show(cal.empty, n == 0);
    if (n == 0) text(cal.empty, !cal.clock_valid ? dash_tr(S_CAL_CLOCK) : failed ? dash_tr(S_CAL_FAILED) :
        !cal.data || !cal.data->have ? dash_tr(S_CAL_LOADING) : !cal.data->configured ? dash_tr(S_CAL_SETUP_BODY) :
        !match ? dash_tr(S_CAL_LOADING) :
        !fresh ? dash_tr(S_CAL_FAILED) : dash_tr(S_CAL_EMPTY));
    show(cal.prev, match && cal.offset > 0);
    show(cal.next, match && cal.offset+cal.data->count < cal.data->total && cal.data->count > 0);
    if (match && n) snprintf(buf, sizeof(buf), "%u-%u / %u", cal.offset+1, cal.offset+n, cal.data->total);
    else buf[0] = '\0';
    text(cal.counter, buf);
}

void dash_calendar_ui_update(const dash_calendar_t *st, int64_t now, bool clock_valid)
{
    cal.data = st;
    cal.now = now;
    cal.clock_valid = clock_valid;
    int32_t today = dash_local_day(now);
    bool changed = cal.today != today;
    cal.today = today;
    if (clock_valid && (cal.day < today || cal.day > today+6 || changed)) {
        cal.day = today;
        cal.offset = 0;
        request();
    }
    if (matching() && st->ok && st->total <= cal.offset && cal.offset) {
        cal.offset = st->total ? (uint16_t)(((st->total-1)/8)*8) : 0;
        request();
    }
    render();
}

void dash_calendar_ui_page_changed(void)
{
    bool active = dash_ui_get_tool() == DASH_TOOL_CALENDAR;
    if (active && !cal.active && cal.clock_valid) {
        cal.day = cal.today;
        cal.offset = 0;
        request();
    }
    cal.active = active;
    close_detail(NULL);
}

void dash_calendar_ui_destroy(void)
{
    close_detail(NULL);
    cal.status = NULL;
    cal.data = NULL;
}

void dash_calendar_ui_set_cb(dash_ui_calendar_cb_t cb) { query_cb = cb; }
