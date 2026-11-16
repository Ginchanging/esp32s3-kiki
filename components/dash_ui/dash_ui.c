#include "dash_ui.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dash_cat.h"
#include "dash_flight_ui.h"
#include "dash_calendar_ui.h"
#include "dash_season.h"
#include "dash_ota_ui.h"
#include "dash_i18n.h"
#include "dash_pix.h"
#include "dash_theme.h"
#include "sprites/dash_sprites.h"

LV_FONT_DECLARE(dash_font_zh12)
LV_FONT_DECLARE(dash_font_zh16)
LV_FONT_DECLARE(dash_font_zh20)

#define TOP_H 24
#define DOTS_H 14
#define TILE_H (DASH_UI_H - TOP_H - DOTS_H) /* 282 */
#define PAD 10
#define INNER_W (DASH_UI_W - 2 * PAD)

#define FOCUS_MS (25 * 60 * 1000)
#define BREAK_MS (5 * 60 * 1000)
#define HEAT_COLS (DASH_CONTRIB_DAYS / 7)
#define HEAT_CELL 10
#define SPARK_W 136
#define SPARK_H 28

typedef struct {
    const lv_font_t *small, *body, *title, *icon;
} fonts_t;

static fonts_t F;

typedef struct {
    lv_obj_t *row, *dot, *name, *right;
} run_row_t;

typedef struct {
    lv_obj_t *card, *dot, *name, *right, *spark_box, *spark, *val[3], *svc_box, *svc[DASH_MAX_SERVICES], *note;
    lv_point_precise_t pts[DASH_CPU_HIST];
    int npts;
    uint8_t last_hist[DASH_CPU_HIST];
    int last_len;
} srv_card_t;

typedef struct {
    lv_obj_t *box, *time, *text;
} mail_row_t;

#define MI_SPARK_W 64
#define MI_SPARK_H 14

typedef struct {
    lv_obj_t *shadow, *card, *icon, *name, *sub, *state, *btn, *btn_lbl, *bar, *left, *spark;
    lv_point_precise_t pts[DASH_WEIGHT_DAYS]; /* scale trend */
    int npts;
} mi_card_t;

static struct {
    lv_obj_t *scr;
    lv_obj_t *top_bar, *top_time, *top_fresh, *top_mqtt, *top_wifi, *lang_btn;
    lv_obj_t *tv, *tiles[DASH_UI_PAGES], *dots[DASH_UI_PAGES];
    /* home */
    lv_obj_t *wall, *sky, *sun, *moon_bite, *stars[7], *date, *env, *cat_img, *bubble_text, *night_veil;
    lv_obj_t *chip[4], *chip_icon[4], *chip_val[4];
    dash_pix_t clock;
    char clock_drawn[8];
    /* github */
    lv_obj_t *gh_repo, *gh_block, *gh_icon, *gh_state, *gh_wf, *gh_meta, *gh_empty;
    lv_obj_t *heat_card, *heat_week, *heat_today, *heat_none;
    dash_pix_t heat;
    uint8_t heat_drawn[DASH_CONTRIB_DAYS];
    bool heat_valid;
    run_row_t runs[2];
    /* servers */
    srv_card_t srv[DASH_MAX_SERVERS];
    /* pomodoro */
    lv_obj_t *pomo_arc, *pomo_phase_lbl, *pomo_btn_lbl[2], *pomo_stats;
    dash_pix_t pomo_cat, pomo_time;
    char pomo_time_drawn[8];
    int pomo_cat_drawn;
    /* mailbox */
    lv_obj_t *mail_status, *mail_list, *mail_empty;
    mail_row_t mail[DASH_MAX_MUSE];
    /* Mi Home */
    lv_obj_t *mi_status, *mi_empty;
    mi_card_t mi[DASH_MAX_MI];
    struct {
        bool active, want;
        uint32_t deadline, fail_until; /* lv_tick ms */
    } mi_pend[DASH_MAX_MI];
    /* popup */
    lv_obj_t *popup;
    lv_timer_t *popup_timer;

    int page;
    bool have_last;
    dash_state_t last;
    int64_t last_now;
    dash_ui_lang_cb_t lang_cb;
    dash_ui_pomo_cb_t pomo_cb;
    dash_ui_mi_cb_t mi_cb;

    /* Kiki */
    char bubble_override[DASH_MUSE_TEXT];
    uint32_t bubble_until; /* lv_tick ms */
    bool flight_bubble;
    int touch_idx;
    int last_activity;
    int debug_activity, debug_night;
    int64_t time_offset;
    uint32_t wake_until;
    bool night;
} ui = {.debug_activity = -1, .debug_night = -1, .last_activity = -1};

static struct {
    dash_pomo_phase_t phase, paused_from;
    uint32_t end_tick, remain_ms;
    int count, minutes, yday;
    dash_pomo_day_t week[DASH_DAYS]; /* per-day totals for the weekly report */
} pomo;

/* ------------------------------------------------------------------ helpers */

static lv_color_t hex(uint32_t c)
{
    return lv_color_hex(c);
}

static void pick_fonts(dash_lang_t lang)
{
    F.icon = &lv_font_montserrat_20;
    if (lang == DASH_LANG_ZH) {
        F.small = &dash_font_zh12;
        F.body = &dash_font_zh16;
        F.title = &dash_font_zh20;
    } else {
        F.small = &lv_font_montserrat_12;
        F.body = &lv_font_montserrat_14;
        F.title = &lv_font_montserrat_20;
    }
}

static lv_obj_t *box(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, hex(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_scrollable(o, false);
    return o;
}

/* card with a solid 2 px "pixel" drop shadow */
static lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg)
{
    lv_obj_t *sh = box(parent, x + 2, y + 2, w, h, T_SHADOW);
    lv_obj_set_style_radius(sh, T_RADIUS, 0);
    lv_obj_t *c = box(parent, x, y, w, h, bg);
    lv_obj_set_style_radius(c, T_RADIUS, 0);
    return c;
}

static lv_obj_t *dot(lv_obj_t *parent, int32_t x, int32_t y, int32_t d)
{
    lv_obj_t *o = box(parent, x, y, d, d, T_GREY);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t x, int32_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *label_w(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t x, int32_t y,
                         int32_t w, lv_label_long_mode_t mode)
{
    lv_obj_t *l = label(parent, font, color, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, mode);
    if (mode == LV_LABEL_LONG_MODE_DOTS || mode == LV_LABEL_LONG_MODE_CLIP) {
        lv_obj_set_height(l, lv_font_get_line_height(font));
    }
    return l;
}

/* The setters below skip unchanged values: every LVGL set_* invalidates the
 * widget, and on the real SPI screen each redraw costs bus time. */
static void set_color(lv_obj_t *o, uint32_t c)
{
    if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), hex(c))) {
        lv_obj_set_style_bg_color(o, hex(c), 0);
    }
}

static void set_text_color(lv_obj_t *o, uint32_t c)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(o, 0), hex(c))) {
        lv_obj_set_style_text_color(o, hex(c), 0);
    }
}

static void show(lv_obj_t *o, bool visible)
{
    if (lv_obj_is_hidden(o) == visible) {
        lv_obj_set_hidden(o, !visible);
    }
}

static void set_text(lv_obj_t *l, const char *text)
{
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, text) != 0) {
        lv_label_set_text(l, text);
    }
}

static uint32_t run_color(dash_run_state_t s)
{
    switch (s) {
    case DASH_RUN_SUCCESS:
        return T_GOOD;
    case DASH_RUN_FAILURE:
        return T_BAD;
    case DASH_RUN_RUNNING:
    case DASH_RUN_QUEUED:
        return T_WARN;
    default:
        return T_GREY;
    }
}

static const char *run_text(dash_run_state_t s)
{
    static const dash_str_t map[] = {
        [DASH_RUN_UNKNOWN] = S_RUN_UNKNOWN, [DASH_RUN_QUEUED] = S_RUN_QUEUED,
        [DASH_RUN_RUNNING] = S_RUN_RUNNING, [DASH_RUN_SUCCESS] = S_RUN_SUCCESS,
        [DASH_RUN_FAILURE] = S_RUN_FAILED,  [DASH_RUN_CANCELLED] = S_RUN_CANCELLED,
        [DASH_RUN_SKIPPED] = S_RUN_SKIPPED,
    };
    return dash_tr(map[s]);
}

static const char *src_name(uint8_t src)
{
    switch (src) {
    case DASH_SRC_MIHOME:
        return dash_tr(S_MI_TITLE);
    case DASH_SRC_CLAUDE:
        return "Claude Code";
    case DASH_SRC_CODEX:
        return "Codex";
    case DASH_SRC_MUSE:
        return "Muse";
    case DASH_SRC_KIKI:
        return "Kiki";
    default:
        return "Agent";
    }
}

static uint32_t msg_color(const dash_muse_msg_t *m)
{
    if (m->src == DASH_SRC_MUSE || m->src == DASH_SRC_KIKI) {
        return T_ACCENT;
    }
    switch (m->kind) {
    case DASH_MSG_DONE:
        return T_GOOD;
    case DASH_MSG_ATTENTION:
        return T_WARN;
    case DASH_MSG_ERROR:
        return T_BAD;
    default:
        return T_INFO;
    }
}

static const char *msg_icon(const dash_muse_msg_t *m)
{
    if (m->src == DASH_SRC_MUSE) {
        return LV_SYMBOL_ENVELOPE;
    }
    if (m->src == DASH_SRC_MIHOME) {
        return LV_SYMBOL_HOME;
    }
    if (m->src == DASH_SRC_KIKI) {
        return LV_SYMBOL_EDIT; /* Kiki writing her diary */
    }
    switch (m->kind) {
    case DASH_MSG_DONE:
        return LV_SYMBOL_OK;
    case DASH_MSG_ATTENTION:
        return LV_SYMBOL_BELL;
    case DASH_MSG_ERROR:
        return LV_SYMBOL_WARNING;
    default:
        return LV_SYMBOL_ENVELOPE;
    }
}

static void agent_title(const dash_muse_msg_t *m, char *buf, size_t n)
{
    if (m->src == DASH_SRC_MIHOME) {
        snprintf(buf, n, "%s", dash_tr(S_MI_POPUP));
        return;
    }
    if (m->src == DASH_SRC_KIKI) {
        snprintf(buf, n, dash_tr(S_DIARY_TITLE_FMT), m->project);
        return;
    }
    dash_str_t fmt = m->kind == DASH_MSG_DONE        ? S_AGENT_DONE_FMT
                     : m->kind == DASH_MSG_ATTENTION ? S_AGENT_ATTN_FMT
                     : m->kind == DASH_MSG_ERROR     ? S_AGENT_ERR_FMT
                                                     : S_AGENT_INFO_FMT;
    snprintf(buf, n, dash_tr(fmt), src_name(m->src));
}

/* short form for the 50 px home chip */
static const char *run_chip(dash_run_state_t s)
{
    static const char *const en[] = {"?", "WAIT", "RUN", "OK", "FAIL", "STOP", "SKIP"};
    static const char *const zh[] = {"未知", "排队", "运行", "成功", "失败", "取消", "跳过"};
    return dash_i18n_get() == DASH_LANG_ZH ? zh[s] : en[s];
}

static const char *run_icon(dash_run_state_t s)
{
    switch (s) {
    case DASH_RUN_SUCCESS:
        return LV_SYMBOL_OK;
    case DASH_RUN_FAILURE:
        return LV_SYMBOL_CLOSE;
    case DASH_RUN_RUNNING:
        return LV_SYMBOL_REFRESH;
    case DASH_RUN_QUEUED:
        return LV_SYMBOL_PAUSE;
    case DASH_RUN_CANCELLED:
        return LV_SYMBOL_STOP;
    case DASH_RUN_SKIPPED:
        return LV_SYMBOL_NEXT;
    default:
        return LV_SYMBOL_WARNING;
    }
}

static uint32_t tick_now(void)
{
    return lv_tick_get();
}

static int64_t routine_now(void)
{
    return ui.last_now + ui.time_offset;
}

static int local_yday(int64_t utc)
{
    time_t t = (time_t)(utc + DASH_TZ_OFFSET_S);
    struct tm tm;
    gmtime_r(&t, &tm);
    return tm.tm_year * 400 + tm.tm_yday;
}

/* ------------------------------------------------------------------ pomodoro */

static void pomo_changed(void)
{
    if (ui.last_now > 1700000000) { /* only with a real clock */
        dash_pomo_week_put(pomo.week, dash_local_day(ui.last_now), pomo.count, pomo.minutes);
    }
    if (ui.pomo_cb) {
        ui.pomo_cb(pomo.count, pomo.minutes, pomo.yday);
    }
}

static uint32_t pomo_remaining(void)
{
    if (pomo.phase == DASH_POMO_PAUSED) {
        return pomo.remain_ms;
    }
    if (pomo.phase == DASH_POMO_FOCUS || pomo.phase == DASH_POMO_BREAK) {
        int32_t left = (int32_t)(pomo.end_tick - tick_now());
        return left > 0 ? (uint32_t)left : 0;
    }
    return FOCUS_MS;
}

static uint8_t pomo_percent(void)
{
    dash_pomo_phase_t ph = pomo.phase == DASH_POMO_PAUSED ? pomo.paused_from : pomo.phase;
    uint32_t total = ph == DASH_POMO_BREAK ? BREAK_MS : FOCUS_MS;
    if (pomo.phase == DASH_POMO_IDLE) {
        return 0;
    }
    return (uint8_t)(100 - (uint64_t)pomo_remaining() * 100 / total);
}

static void bubble_say(const char *text, uint32_t ms);
static void popup_open(uint32_t color, const char *icon, const char *title, const char *body, uint32_t ms);

void dash_ui_pomo_toggle(void)
{
    switch (pomo.phase) {
    case DASH_POMO_IDLE:
        pomo.phase = DASH_POMO_FOCUS;
        pomo.end_tick = tick_now() + FOCUS_MS;
        bubble_say(dash_tr(S_REACT_POMO_START), 4000);
        break;
    case DASH_POMO_FOCUS:
    case DASH_POMO_BREAK:
        pomo.remain_ms = pomo_remaining();
        pomo.paused_from = pomo.phase;
        pomo.phase = DASH_POMO_PAUSED;
        bubble_say(dash_tr(S_REACT_POMO_PAUSE), 3000);
        break;
    case DASH_POMO_PAUSED:
        pomo.phase = pomo.paused_from;
        pomo.end_tick = tick_now() + pomo.remain_ms;
        break;
    }
}

static void pomo_reset(void)
{
    pomo.phase = DASH_POMO_IDLE;
}

static void pomo_tick(void)
{
    if (ui.last_now && local_yday(ui.last_now) != pomo.yday) {
        pomo.yday = local_yday(ui.last_now);
        pomo.count = pomo.minutes = 0;
        pomo_changed();
    }
    if ((pomo.phase == DASH_POMO_FOCUS || pomo.phase == DASH_POMO_BREAK) && pomo_remaining() == 0) {
        if (pomo.phase == DASH_POMO_FOCUS) {
            pomo.count++;
            pomo.minutes += FOCUS_MS / 60000;
            pomo_changed();
            pomo.phase = DASH_POMO_BREAK;
            pomo.end_tick = tick_now() + BREAK_MS;
            dash_cat_react(DASH_CAT_REACT_POMO_DONE);
            bubble_say(dash_tr(S_REACT_POMO_DONE), 6000);
        } else {
            pomo.phase = DASH_POMO_IDLE;
        }
    }
}

/* ------------------------------------------------------------------ Kiki's bubble */

static void bubble_say(const char *text, uint32_t ms)
{
    ui.flight_bubble = false;
    snprintf(ui.bubble_override, sizeof(ui.bubble_override), "%s", text);
    ui.bubble_until = tick_now() + ms;
    if (ui.bubble_text) {
        set_text(ui.bubble_text, text);
    }
}

static int current_activity(void)
{
    if (ui.debug_activity >= 0) {
        return ui.debug_activity;
    }
    if (!ui.have_last || !ui.last.clock_valid) {
        return DASH_ACT_FREE;
    }
    return dash_schedule_at(routine_now(), NULL, NULL);
}

static bool is_night(void)
{
    if (ui.debug_night >= 0) {
        return ui.debug_night;
    }
    return current_activity() == DASH_ACT_SLEEP;
}

static dash_cat_mood_t current_mood(const dash_state_t *st, int64_t now, char *line, size_t n)
{
    dash_freshness_t fresh = dash_freshness(st, now);
    if (st->net == DASH_NET_DOWN) {
        snprintf(line, n, "%s", dash_tr(S_MOOD_NO_NET));
        return DASH_CAT_MOOD_CONFUSED;
    }
    if (fresh == DASH_FRESH_STALE || fresh == DASH_FRESH_OFFLINE) {
        snprintf(line, n, "%s", dash_tr(S_MOOD_STALE));
        return DASH_CAT_MOOD_CONFUSED;
    }
    for (int i = 0; st->have_hub && i < st->server_count; ++i) {
        if (st->servers[i].ever_reported && !st->servers[i].online) {
            snprintf(line, n, dash_tr(S_MOOD_NERVOUS_FMT), st->servers[i].name);
            return DASH_CAT_MOOD_NERVOUS;
        }
    }
    if (dash_latest_run(st) == DASH_RUN_FAILURE) {
        snprintf(line, n, dash_tr(S_MOOD_WORRIED_FMT), st->runs[0].workflow);
        return DASH_CAT_MOOD_WORRIED;
    }
    line[0] = '\0';
    return DASH_CAT_MOOD_HAPPY;
}

static void touch_report(void)
{
    char buf[128], tmp[40];
    const dash_state_t *st = &ui.last;
    dash_lang_t lang = dash_i18n_get();
    switch (ui.touch_idx++ % 4) {
    case 0:
        if (st->have_hub && st->run_count) {
            dash_fmt_ago(ui.last_now - st->runs[0].updated_at, lang, tmp, sizeof(tmp));
            snprintf(buf, sizeof(buf), dash_tr(S_TOUCH_CI_FMT), run_text(st->runs[0].state), tmp);
        } else {
            snprintf(buf, sizeof(buf), "%s", dash_tr(S_TOUCH_MEOW));
        }
        break;
    case 1:
        snprintf(buf, sizeof(buf), dash_tr(S_TOUCH_SERVERS_FMT), dash_servers_online(st),
                 dash_servers_known(st));
        break;
    case 2: {
        dash_activity_t next;
        int next_min;
        dash_schedule_at(routine_now(), &next, &next_min);
        snprintf(buf, sizeof(buf), dash_tr(S_TOUCH_NEXT_FMT), next_min / 60, next_min % 60,
                 dash_tr((dash_str_t)(S_ACTN_SLEEP + next)));
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "%s", dash_tr(S_TOUCH_HINT));
        break;
    }
    bubble_say(buf, 5000);
}

static void cat_clicked(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        dash_ui_pomo_toggle();
        return;
    }
    dash_cat_react(DASH_CAT_REACT_TOUCH);
    touch_report();
}

/* ------------------------------------------------------------------ build */

static void update_dots(void)
{
    for (int i = 0; i < DASH_UI_PAGES; ++i) {
        set_color(ui.dots[i], i == ui.page ? T_ACCENT : T_LINE);
    }
}

static void tile_changed(lv_event_t *e)
{
    lv_obj_t *active = lv_tileview_get_tile_active(ui.tv);
    for (int i = 0; i < DASH_UI_PAGES; ++i) {
        if (ui.tiles[i] == active) {
            ui.page = i;
        }
    }
    update_dots();
    dash_flight_ui_page_changed();
    dash_calendar_ui_page_changed();
}

static void lang_toggle_async(void *arg)
{
    dash_lang_t next = dash_i18n_get() == DASH_LANG_ZH ? DASH_LANG_EN : DASH_LANG_ZH;
    dash_ui_set_lang(next);
    if (ui.lang_cb) {
        ui.lang_cb(next);
    }
}

static void lang_clicked(lv_event_t *e)
{
    /* The rebuild deletes this button's screen, so never do it inside its own event. */
    lv_async_call(lang_toggle_async, NULL);
}

static void wake_clicked(lv_event_t *e)
{
    ui.wake_until = tick_now() + 30000;
    show(ui.night_veil, false);
}

static void build_top(lv_obj_t *scr)
{
    ui.top_bar = box(scr, 0, 0, DASH_UI_W, TOP_H, T_BAR);
    ui.top_time = label(ui.top_bar, &lv_font_montserrat_14, T_TEXT, 8, 4);

    ui.top_fresh = label(ui.top_bar, F.small, T_TEXT, 0, 0);
    lv_obj_set_style_pad_hor(ui.top_fresh, 6, 0);
    lv_obj_set_style_pad_ver(ui.top_fresh, 1, 0);
    lv_obj_set_style_radius(ui.top_fresh, 4, 0);
    lv_obj_set_style_bg_opa(ui.top_fresh, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(ui.top_fresh, hex(T_LINE), 0);
    lv_obj_align(ui.top_fresh, LV_ALIGN_LEFT_MID, 58, 0);

    ui.lang_btn = box(ui.top_bar, DASH_UI_W - 40, 3, 34, 18, T_LINE);
    lv_obj_set_style_radius(ui.lang_btn, 4, 0);
    lv_obj_set_clickable(ui.lang_btn, true);
    lv_obj_set_ext_click_area(ui.lang_btn, 6);
    lv_obj_add_event_cb(ui.lang_btn, lang_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = label(ui.lang_btn, &dash_font_zh12, T_TEXT, 0, 0);
    lv_label_set_text(ll, dash_tr(S_LANG_SWITCH));
    lv_obj_center(ll);

    ui.top_wifi = label(ui.top_bar, &lv_font_montserrat_14, T_GREY, DASH_UI_W - 64, 4);
    lv_label_set_text(ui.top_wifi, LV_SYMBOL_WIFI);
    ui.top_mqtt = dot(ui.top_bar, DASH_UI_W - 76, 9, 7);
}

static void goto_page(lv_event_t *e)
{
    dash_ui_set_page((int)(intptr_t)lv_event_get_user_data(e), true);
}

/* The first and last tiles cannot scroll outwards, so a swipe there is not taken by the
 * tileview and LVGL reports it as a gesture instead (it bubbles up to the screen):
 * wrap around. */
static void page_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if ((ui.page == 0 && dir == LV_DIR_RIGHT) || (ui.page == DASH_UI_PAGES - 1 && dir == LV_DIR_LEFT)) {
        dash_ui_step_page(dir == LV_DIR_RIGHT ? -1 : 1);
        lv_indev_wait_release(indev); /* no tap on Kiki or a button at the end of the swipe */
    }
}

/* Double-tap anywhere except on a button goes back to Kiki's room. LVGL keeps the
 * short-click streak on the input device: two short taps within 400 ms, a few px apart. */
static bool on_button(lv_obj_t *o)
{
    for (; o && o != ui.scr && o != ui.tv; o = lv_obj_get_parent(o)) {
        if (lv_obj_get_event_count(o) > 0) { /* lang switch, pomodoro/lamp buttons, popup */
            return true;
        }
    }
    return false;
}

static void double_tap(lv_event_t *e)
{
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(e);
    if (!ui.tv || ui.page == 0 || lv_indev_get_short_click_streak(indev) != 2 ||
        on_button((lv_obj_t *)lv_event_get_param(e))) {
        return;
    }
    dash_ui_set_page(0, ui.page == 1); /* slide back one page, jump from further away */
}

void dash_ui_attach_indev(lv_indev_t *indev)
{
    if (!indev || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return;
    }
    for (uint32_t i = 0; i < lv_indev_get_event_count(indev); ++i) {
        if (lv_event_dsc_get_cb(lv_indev_get_event_dsc(indev, i)) == double_tap) {
            return;
        }
    }
    lv_indev_add_event_cb(indev, double_tap, LV_EVENT_SHORT_CLICKED, NULL);
}

static void build_home(lv_obj_t *t)
{
    /* the room */
    ui.wall = box(t, 0, 0, DASH_UI_W, 180, T_WALL);
    lv_obj_t *frame = box(t, 10, 10, 92, 68, T_FRAME); /* window, left */
    lv_obj_set_style_radius(frame, 4, 0);
    ui.sky = box(frame, 4, 4, 84, 60, 0x6EC6FF);
    lv_obj_set_style_bg_grad_dir(ui.sky, LV_GRAD_DIR_VER, 0);
    static const int16_t star_xy[7][2] = {{8, 8}, {24, 18}, {40, 5}, {62, 24}, {74, 9}, {12, 40}, {54, 44}};
    for (int i = 0; i < 7; ++i) {
        ui.stars[i] = box(ui.sky, star_xy[i][0], star_xy[i][1], 2, 2, 0xF2E9DC);
    }
    ui.sun = box(ui.sky, 58, 8, 14, 14, 0xFFD166);
    lv_obj_set_style_radius(ui.sun, LV_RADIUS_CIRCLE, 0);
    ui.moon_bite = box(ui.sky, 63, 6, 12, 12, 0x1B1F3B);
    lv_obj_set_style_radius(ui.moon_bite, LV_RADIUS_CIRCLE, 0);
    box(frame, 45, 4, 2, 60, T_FRAME); /* window bars */
    box(frame, 4, 33, 84, 2, T_FRAME);
    ui.date = label_w(t, F.small, T_TEXT, 12, 82, 96, LV_LABEL_LONG_MODE_CLIP);
    ui.env = label_w(t, F.small, T_MUTED, 12, 98, 96, LV_LABEL_LONG_MODE_CLIP);

    if (dash_pix_create(&ui.clock, t, 122, 24)) { /* clock + date, top right */
        lv_obj_set_pos(ui.clock.img, 112, 10);
    }
    ui.clock_drawn[0] = '\0';

    box(t, 0, 138, DASH_UI_W, 42, T_FLOOR);
    box(t, 0, 138, DASH_UI_W, 3, T_FLOOR_EDGE);
    lv_obj_t *rug = box(t, 112, 152, 124, 22, T_RUG);
    lv_obj_set_style_radius(rug, LV_RADIUS_CIRCLE, 0);

    /* a little potted plant on the left */
    lv_obj_t *plant = box(t, 0, 0, 74, 180, T_WALL);
    lv_obj_set_style_bg_opa(plant, LV_OPA_TRANSP, 0);
    lv_obj_set_clickable(plant, false);
    static const int16_t leaf[4][4] = {{26, 118, 16, 16}, {38, 113, 18, 18}, {33, 126, 14, 14}, {48, 124, 13, 13}};
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *l = box(plant, leaf[i][0], leaf[i][1], leaf[i][2], leaf[i][3], i % 2 ? 0x5DBB6E : 0x7BD389);
        lv_obj_set_style_radius(l, LV_RADIUS_CIRCLE, 0);
    }
    box(plant, 28, 138, 34, 5, 0x7A5240);
    box(plant, 32, 143, 26, 18, 0x9C6B4E);
    dash_season_create(t, ui.sky, plant);

    ui.cat_img = dash_cat_create(t, 108, 44);
    if (ui.cat_img) {
        lv_obj_add_event_cb(ui.cat_img, cat_clicked, LV_EVENT_SHORT_CLICKED, NULL);
        lv_obj_add_event_cb(ui.cat_img, cat_clicked, LV_EVENT_LONG_PRESSED, NULL);
        lv_obj_set_ext_click_area(ui.cat_img, 8);
    }
    dash_season_foreground();
    dash_calendar_ui_home(t);

    /* speech bubble with a pixel-staircase pointer up to Kiki */
    box(t, 169, 180, 6, 2, T_CARD);
    box(t, 167, 182, 10, 2, T_CARD);
    lv_obj_t *bub = card(t, PAD, 184, INNER_W, 40, T_CARD);
    lv_obj_t *name = label(bub, &lv_font_montserrat_12, T_ACCENT, 10, 12);
    lv_label_set_text(name, "Kiki");
    ui.bubble_text = label_w(bub, &dash_font_zh16, T_TEXT, 44, 8, INNER_W - 52, LV_LABEL_LONG_MODE_DOTS);

    /* status chips: CI · servers · mail · pomodoro; tap = open that page */
    static const int chip_page[4] = {1, 2, 4, 3};
    for (int i = 0; i < 4; ++i) {
        int x = PAD + i * 56;
        ui.chip[i] = card(t, x, 232, 50, 44, T_CARD);
        lv_obj_set_clickable(ui.chip[i], true);
        lv_obj_add_event_cb(ui.chip[i], goto_page, LV_EVENT_CLICKED, (void *)(intptr_t)chip_page[i]);
        ui.chip_icon[i] = label_w(ui.chip[i], &lv_font_montserrat_16, T_MUTED, 0, 4, 50,
                                  LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(ui.chip_icon[i], LV_TEXT_ALIGN_CENTER, 0);
        ui.chip_val[i] = label_w(ui.chip[i], F.small, T_TEXT, 0, 25, 50, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(ui.chip_val[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(ui.chip_icon[1], LV_SYMBOL_DRIVE);
    lv_label_set_text(ui.chip_icon[2], LV_SYMBOL_ENVELOPE);
    lv_label_set_text(ui.chip_icon[3], LV_SYMBOL_BELL);

    /* night: dim the whole room; a tap wakes it for 30 s */
    ui.night_veil = box(t, 0, 0, DASH_UI_W, TILE_H, 0x000000);
    lv_obj_set_style_bg_opa(ui.night_veil, LV_OPA_50, 0);
    lv_obj_set_clickable(ui.night_veil, true);
    lv_obj_add_event_cb(ui.night_veil, wake_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(ui.night_veil, true);
}

static void build_github(lv_obj_t *t)
{
    lv_label_set_text(label(t, F.title, T_TEXT, PAD, 2), dash_tr(S_GH_TITLE));
    ui.gh_repo = label_w(t, F.small, T_MUTED, PAD, 28, INNER_W, LV_LABEL_LONG_MODE_DOTS);

    ui.gh_block = card(t, PAD, 46, INNER_W, 44, T_GREY);
    lv_obj_set_flex_flow(ui.gh_block, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui.gh_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ui.gh_block, 10, 0);
    ui.gh_icon = lv_label_create(ui.gh_block);
    lv_obj_set_style_text_font(ui.gh_icon, F.icon, 0);
    lv_obj_set_style_text_color(ui.gh_icon, hex(T_BG), 0);
    ui.gh_state = lv_label_create(ui.gh_block);
    lv_obj_set_style_text_font(ui.gh_state, F.title, 0);
    lv_obj_set_style_text_color(ui.gh_state, hex(T_BG), 0);

    ui.gh_wf = label_w(t, F.body, T_TEXT, PAD, 96, INNER_W, LV_LABEL_LONG_MODE_DOTS);
    ui.gh_meta = label_w(t, F.small, T_MUTED, PAD, 118, INNER_W, LV_LABEL_LONG_MODE_DOTS);

    ui.heat_card = card(t, PAD, 138, INNER_W, 78, T_CARD);
    if (dash_pix_create(&ui.heat, ui.heat_card, HEAT_COLS * HEAT_CELL, 7 * HEAT_CELL)) {
        lv_obj_set_pos(ui.heat.img, 8, 4);
    }
    ui.heat_valid = false;
    lv_label_set_text(label(ui.heat_card, F.small, T_MUTED, 166, 4), dash_tr(S_HEAT_WEEK));
    ui.heat_week = label(ui.heat_card, &lv_font_montserrat_20, T_GOOD, 166, 18);
    lv_label_set_text(label(ui.heat_card, F.small, T_MUTED, 166, 40), dash_tr(S_HEAT_TODAY));
    ui.heat_today = label(ui.heat_card, &lv_font_montserrat_20, T_TEXT, 166, 54);
    ui.heat_none = label_w(ui.heat_card, F.small, T_MUTED, 8, 30, 150, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(ui.heat_none, dash_tr(S_HEAT_NONE));

    for (int i = 0; i < 2; ++i) {
        run_row_t *r = &ui.runs[i];
        r->row = card(t, PAD, 222 + i * 29, INNER_W, 25, T_CARD);
        lv_obj_set_style_radius(r->row, 6, 0);
        r->dot = dot(r->row, 8, 9, 8);
        r->name = label_w(r->row, &lv_font_montserrat_12, T_TEXT, 22, 5, 108, LV_LABEL_LONG_MODE_DOTS);
        r->right = label_w(r->row, &lv_font_montserrat_12, T_MUTED, 132, 5, INNER_W - 140,
                           LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(r->right, LV_TEXT_ALIGN_RIGHT, 0);
    }
    ui.gh_empty = label_w(t, F.body, T_MUTED, PAD, 60, INNER_W, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ui.gh_empty, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_servers(lv_obj_t *t)
{
    lv_label_set_text(label(t, F.title, T_TEXT, PAD, 2), dash_tr(S_SRV_TITLE));
    for (int i = 0; i < DASH_MAX_SERVERS; ++i) {
        srv_card_t *c = &ui.srv[i];
        c->card = card(t, PAD, 32 + i * 83, INNER_W, 80, T_CARD);
        c->dot = dot(c->card, 8, 10, 8);
        c->name = label_w(c->card, &lv_font_montserrat_14, T_TEXT, 22, 5, 108, LV_LABEL_LONG_MODE_DOTS);
        c->right = label_w(c->card, F.small, T_MUTED, 130, 7, INNER_W - 138, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(c->right, LV_TEXT_ALIGN_RIGHT, 0);
        c->spark_box = box(c->card, 8, 26, SPARK_W, SPARK_H + 4, T_BG);
        lv_obj_set_style_radius(c->spark_box, 4, 0);
        c->spark = lv_line_create(c->spark_box);
        lv_obj_set_pos(c->spark, 0, 2);
        lv_obj_set_style_line_width(c->spark, 2, 0);
        lv_obj_set_style_line_rounded(c->spark, false, 0);
        lv_obj_set_style_line_color(c->spark, hex(T_GOOD), 0);
        c->last_len = -1;
        for (int k = 0; k < 3; ++k) {
            c->val[k] = label_w(c->card, F.small, T_MUTED, 150, 22 + k * 14, 64, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_height(c->val[k], 14);
        }
        c->svc_box = lv_obj_create(c->card);
        lv_obj_remove_style_all(c->svc_box);
        lv_obj_set_pos(c->svc_box, 8, 62);
        lv_obj_set_size(c->svc_box, INNER_W - 16, 16);
        lv_obj_set_flex_flow(c->svc_box, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(c->svc_box, 8, 0);
        lv_obj_set_scrollable(c->svc_box, false);
        for (int k = 0; k < DASH_MAX_SERVICES; ++k) {
            c->svc[k] = lv_label_create(c->svc_box);
            lv_obj_set_style_text_font(c->svc[k], &lv_font_montserrat_12, 0);
        }
        c->note = label_w(c->card, F.body, T_MUTED, 8, 30, INNER_W - 16, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(c->note, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void pomo_btn(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) == 0) {
        dash_ui_pomo_toggle();
    } else {
        pomo_reset();
    }
}

static void build_pomodoro(lv_obj_t *t)
{
    lv_label_set_text(label(t, F.title, T_TEXT, PAD, 2), dash_tr(S_POMO_TITLE));
    ui.pomo_arc = lv_arc_create(t);
    lv_obj_set_size(ui.pomo_arc, 160, 160);
    lv_obj_set_pos(ui.pomo_arc, 40, 30);
    lv_arc_set_bg_angles(ui.pomo_arc, 0, 360);
    lv_arc_set_rotation(ui.pomo_arc, 270);
    lv_arc_set_range(ui.pomo_arc, 0, 1000);
    lv_arc_set_value(ui.pomo_arc, 0);
    lv_obj_remove_style(ui.pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(ui.pomo_arc, false);
    lv_obj_set_style_arc_width(ui.pomo_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ui.pomo_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ui.pomo_arc, hex(T_LINE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(ui.pomo_arc, hex(T_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(ui.pomo_arc, false, LV_PART_INDICATOR);

    if (dash_pix_create(&ui.pomo_cat, t, 64, 64)) {
        lv_obj_set_pos(ui.pomo_cat.img, 88, 50);
    }
    ui.pomo_cat_drawn = -1;
    if (dash_pix_create(&ui.pomo_time, t, 90, 21)) {
        lv_obj_set_pos(ui.pomo_time.img, 75, 120);
    }
    ui.pomo_time_drawn[0] = '\0';
    ui.pomo_phase_lbl = label_w(t, F.small, T_MUTED, 40, 146, 160, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(ui.pomo_phase_lbl, LV_TEXT_ALIGN_CENTER, 0);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *b = card(t, 24 + i * 102, 200, 90, 34, i == 0 ? T_ACCENT : T_CARD);
        lv_obj_set_clickable(b, true);
        lv_obj_add_event_cb(b, pomo_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui.pomo_btn_lbl[i] = label(b, F.body, i == 0 ? T_BG : T_TEXT, 0, 0);
        lv_obj_center(ui.pomo_btn_lbl[i]);
    }
    lv_label_set_text(ui.pomo_btn_lbl[1], dash_tr(S_POMO_RESET));
    ui.pomo_stats = label_w(t, F.small, T_MUTED, PAD, 248, INNER_W, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(ui.pomo_stats, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_mail(lv_obj_t *t)
{
    lv_label_set_text(label(t, F.title, T_TEXT, PAD, 2), dash_tr(S_MUSE_TITLE));
    ui.mail_status = label_w(t, F.small, T_MUTED, 130, 8, INNER_W - 120, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(ui.mail_status, LV_TEXT_ALIGN_RIGHT, 0);

    ui.mail_list = lv_obj_create(t);
    lv_obj_remove_style_all(ui.mail_list);
    lv_obj_set_pos(ui.mail_list, PAD, 32);
    lv_obj_set_size(ui.mail_list, INNER_W + 2, TILE_H - 36);
    lv_obj_set_flex_flow(ui.mail_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.mail_list, 8, 0);
    lv_obj_set_scroll_dir(ui.mail_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ui.mail_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < DASH_MAX_MUSE; ++i) {
        mail_row_t *m = &ui.mail[i];
        m->box = lv_obj_create(ui.mail_list);
        lv_obj_remove_style_all(m->box);
        lv_obj_set_width(m->box, INNER_W);
        lv_obj_set_height(m->box, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(m->box, hex(T_CARD), 0);
        lv_obj_set_style_bg_opa(m->box, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(m->box, T_RADIUS, 0);
        lv_obj_set_style_border_side(m->box, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(m->box, 4, 0);
        lv_obj_set_style_border_color(m->box, hex(T_ACCENT), 0);
        lv_obj_set_style_pad_all(m->box, 8, 0);
        lv_obj_set_style_pad_left(m->box, 12, 0);
        lv_obj_set_flex_flow(m->box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(m->box, 2, 0);
        lv_obj_set_scrollable(m->box, false);
        m->time = lv_label_create(m->box);
        lv_obj_set_style_text_font(m->time, F.small, 0);
        lv_obj_set_style_text_color(m->time, hex(T_MUTED), 0);
        m->text = lv_label_create(m->box);
        lv_obj_set_width(m->text, INNER_W - 22);
        lv_label_set_long_mode(m->text, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(m->text, &dash_font_zh16, 0);
        lv_obj_set_style_text_color(m->text, hex(T_TEXT), 0);
    }
    ui.mail_empty = label_w(t, F.body, T_MUTED, PAD, 110, INNER_W, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ui.mail_empty, LV_TEXT_ALIGN_CENTER, 0);
}

/* Mi Home: one card per device. Cards are 54 px so four fit without scrolling. */
#define MI_Y0 34
#define MI_H 54
#define MI_GAP 8

static void update_mihome(const dash_state_t *st);

/* Toggle lamp/switch i (card button or knob click). False if it cannot be switched now. */
static bool mi_toggle(int i)
{
    if (i < 0 || i >= ui.last.mi_count || ui.mi_pend[i].active) {
        return false;
    }
    const dash_mi_dev_t *d = &ui.last.mi[i];
    if (!ui.last.mi_ok || !d->online || (d->kind != DASH_MI_LAMP && d->kind != DASH_MI_SWITCH)) {
        return false;
    }
    ui.mi_pend[i].active = true;
    ui.mi_pend[i].want = !d->on;
    ui.mi_pend[i].deadline = tick_now() + 12000;
    ui.mi_pend[i].fail_until = 0;
    if (ui.mi_cb) {
        ui.mi_cb(d->id, !d->on, -1);
    }
    update_mihome(&ui.last);
    return true;
}

static void mi_btn_clicked(lv_event_t *e)
{
    mi_toggle((int)(intptr_t)lv_event_get_user_data(e));
}

static void build_mihome(lv_obj_t *t)
{
    lv_label_set_text(label(t, F.title, T_TEXT, PAD, 2), dash_tr(S_MI_TITLE));
    ui.mi_status = label_w(t, F.small, T_MUTED, 110, 8, INNER_W - 100, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(ui.mi_status, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < DASH_MAX_MI; ++i) {
        mi_card_t *c = &ui.mi[i];
        int32_t y = MI_Y0 + i * (MI_H + MI_GAP);
        c->shadow = box(t, PAD + 2, y + 2, INNER_W, MI_H, T_SHADOW);
        lv_obj_set_style_radius(c->shadow, T_RADIUS, 0);
        c->card = box(t, PAD, y, INNER_W, MI_H, T_CARD);
        lv_obj_set_style_radius(c->card, T_RADIUS, 0);
        c->icon = label(c->card, F.icon, T_GREY, 10, 16);
        c->name = label_w(c->card, F.body, T_TEXT, 40, 7, 100, LV_LABEL_LONG_MODE_DOTS);
        c->sub = label_w(c->card, F.small, T_MUTED, 40, 31, 110, LV_LABEL_LONG_MODE_DOTS);
        /* lamp / switch: an on-off button */
        c->btn = box(c->card, INNER_W - 64, 11, 54, 32, T_LINE);
        lv_obj_set_style_radius(c->btn, 8, 0);
        lv_obj_set_clickable(c->btn, true);
        lv_obj_set_ext_click_area(c->btn, 8);
        lv_obj_add_event_cb(c->btn, mi_btn_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        c->btn_lbl = label(c->btn, F.body, T_TEXT, 0, 0);
        lv_obj_center(c->btn_lbl);
        /* washer: state, progress bar, expected finish time */
        c->state = label_w(c->card, F.body, T_TEXT, 110, 7, INNER_W - 120, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(c->state, LV_TEXT_ALIGN_RIGHT, 0);
        c->bar = lv_bar_create(c->card);
        lv_obj_remove_style_all(c->bar);
        lv_obj_set_pos(c->bar, 40, 36);
        lv_obj_set_size(c->bar, 60, 8); /* leaves room for "预计 12:42 洗好" */
        lv_bar_set_range(c->bar, 0, 100);
        lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(c->bar, hex(T_LINE), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(c->bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(c->bar, hex(T_INFO), LV_PART_INDICATOR);
        lv_obj_set_style_radius(c->bar, 2, LV_PART_MAIN);
        lv_obj_set_style_radius(c->bar, 2, LV_PART_INDICATOR);
        c->left = label_w(c->card, F.small, T_MUTED, 104, 32, INNER_W - 114, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(c->left, LV_TEXT_ALIGN_RIGHT, 0);
        /* scale: 30-day trend */
        c->spark = lv_line_create(c->card);
        lv_obj_set_pos(c->spark, INNER_W - 10 - MI_SPARK_W, 32);
        lv_obj_set_size(c->spark, MI_SPARK_W, MI_SPARK_H);
        lv_obj_set_style_line_width(c->spark, 2, 0);
        lv_obj_set_style_line_rounded(c->spark, true, 0);
        lv_obj_set_style_line_color(c->spark, hex(T_ACCENT), 0);
        lv_obj_set_hidden(c->spark, true);
        c->npts = -1;
    }
    ui.mi_empty = label_w(t, F.body, T_MUTED, PAD, 110, INNER_W, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ui.mi_empty, LV_TEXT_ALIGN_CENTER, 0);
}

static void destroy_pix(void)
{
    dash_calendar_ui_destroy();
    dash_season_destroy();
    dash_flight_ui_destroy();
    dash_cat_destroy();
    dash_pix_destroy(&ui.clock);
    dash_pix_destroy(&ui.heat);
    dash_pix_destroy(&ui.pomo_cat);
    dash_pix_destroy(&ui.pomo_time);
}

static void build(void)
{
    pick_fonts(dash_i18n_get());
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, hex(T_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(scr, false);
    lv_obj_add_event_cb(scr, page_gesture, LV_EVENT_GESTURE, NULL);
    ui.scr = scr;

    build_top(scr);

    ui.tv = lv_tileview_create(scr);
    lv_obj_set_pos(ui.tv, 0, TOP_H);
    lv_obj_set_size(ui.tv, DASH_UI_W, TILE_H);
    lv_obj_set_style_bg_opa(ui.tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(ui.tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < DASH_UI_PAGES; ++i) {
        lv_dir_t dir = i == 0                   ? LV_DIR_RIGHT
                       : i == DASH_UI_PAGES - 1 ? LV_DIR_LEFT
                                                : (lv_dir_t)(LV_DIR_LEFT | LV_DIR_RIGHT);
        ui.tiles[i] = lv_tileview_add_tile(ui.tv, (uint8_t)i, 0, dir);
        lv_obj_set_scrollable(ui.tiles[i], false);
    }
    lv_obj_add_event_cb(ui.tv, tile_changed, LV_EVENT_VALUE_CHANGED, NULL);
    build_home(ui.tiles[0]);
    build_github(ui.tiles[1]);
    build_servers(ui.tiles[2]);
    build_pomodoro(ui.tiles[3]);
    build_mail(ui.tiles[4]);
    build_mihome(ui.tiles[5]);
    dash_flight_ui_create(ui.tiles[6], ui.tiles[0]);
    dash_calendar_ui_create(ui.tiles[7]);
    lv_obj_move_to_index(ui.night_veil, -1); /* the outdoor scene follows the same night dimming */

    lv_obj_t *dots = lv_obj_create(scr);
    lv_obj_remove_style_all(dots);
    lv_obj_set_pos(dots, 0, DASH_UI_H - DOTS_H);
    lv_obj_set_size(dots, DASH_UI_W, DOTS_H);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dots, 8, 0);
    for (int i = 0; i < DASH_UI_PAGES; ++i) {
        ui.dots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(ui.dots[i]);
        lv_obj_set_size(ui.dots[i], 6, 6); /* square pixel dots */
        lv_obj_set_style_bg_opa(ui.dots[i], LV_OPA_COVER, 0);
    }
    lv_tileview_set_tile_by_index(ui.tv, (uint32_t)ui.page, 0, LV_ANIM_OFF);
    update_dots();
}

static void rebuild(void)
{
    lv_obj_t *old = ui.scr;
    if (ui.popup_timer) {
        lv_timer_delete(ui.popup_timer);
        ui.popup_timer = NULL;
    }
    if (ui.popup) {
        lv_obj_delete(ui.popup);
        ui.popup = NULL;
    }
    destroy_pix();
    /* Do not keep two complete eight-page trees resident during a language
     * switch. A tiny temporary screen lets us delete the old active screen
     * safely. Both screen loads happen before the next refresh, so the panel
     * keeps its previous frame until the rebuilt screen is ready. */
    lv_obj_t *blank = lv_obj_create(NULL);
    lv_obj_remove_style_all(blank);
    lv_obj_set_style_bg_color(blank, hex(T_BG), 0);
    lv_obj_set_style_bg_opa(blank, LV_OPA_COVER, 0);
    lv_screen_load(blank);
    if (old) lv_obj_delete(old);
    ui.tv = NULL;
    build();
    lv_screen_load(ui.scr);
    lv_obj_delete(blank);
    if (ui.have_last) {
        dash_ui_update(&ui.last, ui.last_now);
    }
}

/* ------------------------------------------------------------------ update */

static void sky_for_hour(int hour, uint32_t *top, uint32_t *bottom, int *mode)
{
    if (hour >= 5 && hour < 7) {
        *top = 0xF6A97A, *bottom = 0xFCD9A8, *mode = 1; /* dawn */
    } else if (hour >= 7 && hour < 17) {
        *top = 0x5BB8F5, *bottom = 0xBDE8FF, *mode = 0; /* day */
    } else if (hour >= 17 && hour < 19) {
        *top = 0xE9795A, *bottom = 0xFFC48C, *mode = 1; /* dusk */
    } else {
        *top = 0x1B1F3B, *bottom = 0x3A2F63, *mode = 2; /* night */
    }
}

static void update_top(const dash_state_t *st, int64_t now, dash_freshness_t fresh)
{
    char buf[16];
    if (st->clock_valid) {
        dash_fmt_hhmm(now, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    set_text(ui.top_time, buf);
    uint32_t fc = T_LINE;
    const char *ft = dash_tr(S_NO_DATA);
    switch (fresh) {
    case DASH_FRESH_OK:
        fc = 0x2E6B45;
        ft = dash_tr(S_LIVE);
        break;
    case DASH_FRESH_STALE:
        fc = 0x7A5A12;
        ft = dash_tr(S_STALE);
        break;
    case DASH_FRESH_OFFLINE:
        fc = 0x8B2522;
        ft = dash_tr(S_OFFLINE);
        break;
    default:
        break;
    }
    set_text(ui.top_fresh, ft);
    set_color(ui.top_fresh, fc);
    set_text_color(ui.top_wifi, st->net == DASH_NET_ONLINE ? T_TEXT
                                : st->net == DASH_NET_DOWN ? T_BAD
                                                           : T_GREY);
    set_color(ui.top_mqtt, !st->mqtt_configured ? T_LINE : st->mqtt_connected ? T_GOOD : T_BAD);
}

static void update_home(const dash_state_t *st, int64_t now, dash_freshness_t fresh)
{
    char buf[DASH_MUSE_TEXT + 8];
    dash_lang_t lang = dash_i18n_get();
    int64_t rnow = routine_now();

    /* sky follows the (routine) clock */
    time_t lt = (time_t)(rnow + DASH_TZ_OFFSET_S);
    struct tm tm;
    gmtime_r(&lt, &tm);
    uint32_t top, bottom;
    int mode;
    sky_for_hour(st->clock_valid ? tm.tm_hour : 12, &top, &bottom, &mode);
    if (ui.debug_night == 1) {
        sky_for_hour(23, &top, &bottom, &mode);
    }
    set_color(ui.sky, top);
    if (!lv_color_eq(lv_obj_get_style_bg_grad_color(ui.sky, 0), hex(bottom))) {
        lv_obj_set_style_bg_grad_color(ui.sky, hex(bottom), 0);
    }
    for (int i = 0; i < 7; ++i) {
        show(ui.stars[i], mode == 2);
    }
    set_color(ui.sun, mode == 2 ? 0xF2E9DC : mode == 1 ? 0xFF9E5E : 0xFFD166);
    lv_obj_set_y(ui.sun, mode == 1 ? 34 : 8);
    show(ui.moon_bite, mode == 2);
    set_color(ui.moon_bite, top);

    ui.night = is_night();
    bool dim = ui.night && (int32_t)(tick_now() - ui.wake_until) >= 0;
    set_color(ui.wall, ui.night ? T_WALL_NIGHT : T_WALL);
    show(ui.night_veil, dim);
    dash_season_update(now, st->clock_valid, mode == 2);

    /* pixel clock */
    if (st->clock_valid) {
        dash_fmt_hhmm(now, buf, sizeof(buf));
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    if (ui.clock.px && strcmp(buf, ui.clock_drawn) != 0) {
        snprintf(ui.clock_drawn, sizeof(ui.clock_drawn), "%.7s", buf);
        dash_pix_clear(&ui.clock, 0);
        int cw = dash_pix_text_width(buf, 3), cx = ui.clock.w - cw - 4; /* right-aligned */
        dash_pix_text(&ui.clock, buf, cx + 2, 2, 3, dash_argb(T_SHADOW)); /* pixel drop shadow */
        dash_pix_text(&ui.clock, buf, cx, 0, 3, dash_argb(T_TEXT));
        dash_pix_commit(&ui.clock);
    }
    if (st->clock_valid) {
        dash_fmt_date(now, lang, buf, sizeof(buf));
        set_text(ui.date, buf);
    } else {
        set_text(ui.date, "");
    }
    if (st->sensor_valid) {
        snprintf(buf, sizeof(buf), lang == DASH_LANG_ZH ? "%.1f℃  %.0f%%" : "%.1f C  %.0f%%", st->temp_c,
                 st->humidity);
    } else {
        snprintf(buf, sizeof(buf), lang == DASH_LANG_ZH ? "--℃  --%%" : "-- C  --%%");
    }
    set_text(ui.env, buf);

    /* Kiki: routine + mood */
    char mood_line[96];
    int act = current_activity();
    dash_cat_mood_t mood = current_mood(st, now, mood_line, sizeof(mood_line));
    if (ui.last_activity == DASH_ACT_SLEEP && act != DASH_ACT_SLEEP && st->clock_valid) {
        dash_cat_react(DASH_CAT_REACT_MORNING);
        bubble_say(dash_tr(S_REACT_MORNING), 5000);
    }
    ui.last_activity = act;
    dash_cat_ctx_t ctx = {.activity = (dash_activity_t)act, .mood = mood, .pomo = pomo.phase};
    if (act == DASH_ACT_FREE && st->clock_valid) ctx.idle = dash_idle_at(rnow);
    dash_cat_set_ctx(&ctx);

    if ((int32_t)(tick_now() - ui.bubble_until) < 0) {
        set_text(ui.bubble_text, ui.bubble_override);
    } else if (mood != DASH_CAT_MOOD_HAPPY && act != DASH_ACT_SLEEP) {
        set_text(ui.bubble_text, mood_line);
    } else if (pomo.phase == DASH_POMO_FOCUS) {
        set_text(ui.bubble_text, dash_tr(S_REACT_POMO_START));
    } else if (pomo.phase == DASH_POMO_BREAK) {
        set_text(ui.bubble_text, dash_tr(S_POMO_BREAK));
    } else if (act == DASH_ACT_FREE) {
        _Static_assert(S_IDLE_YARN-S_IDLE_SIT+1 == DASH_IDLE_COUNT, "idle captions follow the selector");
        set_text(ui.bubble_text, dash_tr((dash_str_t)(S_IDLE_SIT+ctx.idle.kind)));
    } else {
        set_text(ui.bubble_text, dash_tr((dash_str_t)(S_ACT_SLEEP + act)));
    }

    /* chips */
    dash_run_state_t latest = dash_latest_run(st);
    bool trusted = fresh == DASH_FRESH_OK;
    bool has_run = st->have_hub && st->run_count;
    set_text(ui.chip_icon[0], has_run ? run_icon(latest) : LV_SYMBOL_GPS);
    set_text_color(ui.chip_icon[0], has_run && trusted ? run_color(latest) : T_GREY);
    set_text(ui.chip_val[0], has_run ? run_chip(latest) : "--");
    if (st->have_hub && st->server_count) {
        int up = dash_servers_online(st), all = dash_servers_known(st);
        bool down = false;
        for (int i = 0; i < st->server_count; ++i) {
            down |= st->servers[i].ever_reported && !st->servers[i].online;
        }
        snprintf(buf, sizeof(buf), "%d/%d", up, all);
        set_text(ui.chip_val[1], buf);
        set_text_color(ui.chip_icon[1], !trusted ? T_GREY : down ? T_BAD : up == all ? T_GOOD : T_WARN);
    } else {
        set_text(ui.chip_val[1], "--");
        set_text_color(ui.chip_icon[1], T_GREY);
    }
    snprintf(buf, sizeof(buf), "%d", st->muse_count);
    set_text(ui.chip_val[2], buf);
    set_text_color(ui.chip_icon[2], st->muse_count ? T_ACCENT : T_GREY);
    if (pomo.phase == DASH_POMO_IDLE) {
        snprintf(buf, sizeof(buf), "%d", pomo.count);
    } else {
        uint32_t s = pomo_remaining() / 1000;
        snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
    set_text(ui.chip_val[3], buf);
    set_text_color(ui.chip_icon[3], pomo.phase == DASH_POMO_FOCUS   ? T_ACCENT
                                    : pomo.phase == DASH_POMO_BREAK ? T_GOOD
                                                                    : T_GREY);
}

static void draw_heat(const dash_state_t *st)
{
    if (!ui.heat.px) {
        return;
    }
    if (ui.heat_valid && !memcmp(ui.heat_drawn, st->contrib, sizeof(ui.heat_drawn))) {
        return;
    }
    static const uint32_t lv[5] = {T_HEAT0, T_HEAT1, T_HEAT2, T_HEAT3, T_HEAT4};
    dash_pix_clear(&ui.heat, 0);
    for (int i = 0; i < DASH_CONTRIB_DAYS; ++i) {
        int col = i / 7, row = i % 7;
        uint8_t v = st->contrib[i] > 4 ? 4 : st->contrib[i];
        dash_pix_rect(&ui.heat, col * HEAT_CELL, row * HEAT_CELL, HEAT_CELL - 2, HEAT_CELL - 2, dash_argb(lv[v]));
    }
    dash_pix_commit(&ui.heat);
    memcpy(ui.heat_drawn, st->contrib, sizeof(ui.heat_drawn));
    ui.heat_valid = true;
}

static void update_github(const dash_state_t *st, int64_t now)
{
    char buf[96], dur[16], ago[24];
    bool has = st->have_hub && st->run_count > 0;
    if (has) {
        snprintf(buf, sizeof(buf), "%s  " LV_SYMBOL_SHUFFLE " %s", st->repo[0] ? st->repo : "ISBF-Lab/ISBFweb",
                 st->runs[0].branch);
        set_text(ui.gh_repo, buf);
    } else {
        set_text(ui.gh_repo, st->repo[0] ? st->repo : "ISBF-Lab/ISBFweb");
    }
    show(ui.gh_block, has);
    show(ui.gh_wf, has);
    show(ui.gh_meta, has);
    show(ui.gh_empty, !has);
    if (has) {
        const dash_run_t *r0 = &st->runs[0];
        set_color(ui.gh_block, run_color(r0->state));
        set_text(ui.gh_icon, run_icon(r0->state));
        set_text(ui.gh_state, run_text(r0->state));
        set_text(ui.gh_wf, r0->workflow);
        dash_fmt_duration(r0->duration_s, dur, sizeof(dur));
        dash_fmt_ago(now - r0->updated_at, dash_i18n_get(), ago, sizeof(ago));
        snprintf(buf, sizeof(buf), "#%d   " LV_SYMBOL_LOOP " %s   %s", r0->run_number, dur,
                 st->clock_valid ? ago : "");
        set_text(ui.gh_meta, buf);
    } else {
        set_text(ui.gh_empty, st->have_hub ? dash_tr(S_NO_RUNS) : dash_tr(S_NO_DATA));
    }
    for (int i = 0; i < 2; ++i) {
        run_row_t *row = &ui.runs[i];
        int idx = i + 1;
        if (!has || idx >= st->run_count) {
            show(row->row, false);
            continue;
        }
        const dash_run_t *r = &st->runs[idx];
        show(row->row, true);
        set_color(row->dot, run_color(r->state));
        set_text(row->name, r->workflow);
        dash_fmt_duration(r->duration_s, dur, sizeof(dur));
        snprintf(buf, sizeof(buf), "#%d %s", r->run_number, dur);
        set_text(row->right, buf);
    }

    if (ui.heat.img) {
        show(ui.heat.img, st->have_contrib);
    }
    show(ui.heat_none, !st->have_contrib);
    if (st->have_contrib) {
        draw_heat(st);
        snprintf(buf, sizeof(buf), "%d", st->contrib_week);
        set_text(ui.heat_week, buf);
        snprintf(buf, sizeof(buf), "%d", st->contrib_today);
        set_text(ui.heat_today, buf);
    } else {
        set_text(ui.heat_week, "--");
        set_text(ui.heat_today, "--");
    }
}

static void update_spark(srv_card_t *c, const dash_server_t *s)
{
    if (c->last_len == s->cpu_hist_len && !memcmp(c->last_hist, s->cpu_hist, (size_t)s->cpu_hist_len)) {
        return;
    }
    int n = s->cpu_hist_len;
    for (int i = 0; i < n; ++i) {
        c->pts[i].x = n > 1 ? (lv_value_precise_t)(i * (SPARK_W - 4) / (n - 1) + 2) : 2;
        c->pts[i].y = (lv_value_precise_t)(SPARK_H - s->cpu_hist[i] * SPARK_H / 100);
    }
    c->npts = n;
    lv_line_set_points(c->spark, c->pts, (uint32_t)n);
    uint8_t cur = n ? s->cpu_hist[n - 1] : 0;
    lv_obj_set_style_line_color(c->spark, hex(cur >= 85 ? T_BAD : cur >= 60 ? T_WARN : T_GOOD), 0);
    memcpy(c->last_hist, s->cpu_hist, (size_t)n);
    c->last_len = n;
}

static void update_servers(const dash_state_t *st, int64_t now)
{
    char buf[64], tmp[32];
    dash_lang_t lang = dash_i18n_get();
    for (int i = 0; i < DASH_MAX_SERVERS; ++i) {
        srv_card_t *c = &ui.srv[i];
        bool known = st->have_hub && i < st->server_count;
        if (!known) {
            set_text(c->name, "--");
            show(c->right, false);
            set_color(c->dot, T_GREY);
            show(c->spark_box, false);
            for (int k = 0; k < 3; ++k) {
                show(c->val[k], false);
            }
            show(c->svc_box, false);
            show(c->note, true);
            set_text(c->note, dash_tr(S_NO_DATA));
            set_text_color(c->note, T_MUTED);
            continue;
        }
        const dash_server_t *s = &st->servers[i];
        bool live = s->online;
        set_text(c->name, s->name);
        set_color(c->dot, live ? T_GOOD : s->ever_reported ? T_BAD : T_GREY);
        show(c->right, live || s->ever_reported);
        if (live) {
            dash_fmt_uptime(s->uptime_s, lang, tmp, sizeof(tmp));
            snprintf(buf, sizeof(buf), "%s %s", lang == DASH_LANG_EN ? dash_tr(S_UPTIME) : "", tmp);
        } else {
            dash_fmt_ago(now - s->reported_at, lang, buf, sizeof(buf));
        }
        set_text(c->right, buf);
        set_text_color(c->right, live ? T_MUTED : T_BAD);

        show(c->spark_box, live && s->cpu_hist_len > 1);
        if (live && s->cpu_hist_len > 1) {
            update_spark(c, s);
        }
        const float vals[3] = {s->cpu_pct, s->mem_pct, s->disk_pct};
        const dash_str_t names[3] = {S_CPU, S_MEM, S_DISK};
        for (int k = 0; k < 3; ++k) {
            show(c->val[k], live);
            int v = (int)(vals[k] + 0.5f);
            snprintf(buf, sizeof(buf), "%s %d%%", dash_tr(names[k]), v);
            set_text(c->val[k], buf);
            set_text_color(c->val[k], v >= 90 ? T_BAD : v >= 75 ? T_WARN : T_MUTED);
        }
        show(c->svc_box, live && s->service_count > 0);
        for (int k = 0; k < DASH_MAX_SERVICES; ++k) {
            if (k >= s->service_count) {
                show(c->svc[k], false);
                continue;
            }
            show(c->svc[k], true);
            snprintf(buf, sizeof(buf), "%s %s", s->services[k].ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                     s->services[k].name);
            set_text(c->svc[k], buf);
            set_text_color(c->svc[k], s->services[k].ok ? T_GOOD : T_BAD);
        }
        show(c->note, !live);
        set_text(c->note, s->ever_reported ? dash_tr(S_OFFLINE) : dash_tr(S_NOT_CONNECTED));
        set_text_color(c->note, s->ever_reported ? T_BAD : T_MUTED);
    }
}

static void update_pomodoro(void)
{
    char buf[64];
    uint32_t s = pomo_remaining() / 1000;
    if (pomo.phase == DASH_POMO_IDLE) {
        s = FOCUS_MS / 1000;
    }
    snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    if (ui.pomo_time.px && strcmp(buf, ui.pomo_time_drawn) != 0) {
        snprintf(ui.pomo_time_drawn, sizeof(ui.pomo_time_drawn), "%.7s", buf);
        dash_pix_clear(&ui.pomo_time, 0);
        int w = dash_pix_text_width(buf, 3);
        dash_pix_text(&ui.pomo_time, buf, (90 - w) / 2, 0, 3, dash_argb(T_TEXT));
        dash_pix_commit(&ui.pomo_time);
    }
    int32_t arc = pomo.phase == DASH_POMO_IDLE ? 0 : pomo_percent() * 10;
    if (lv_arc_get_value(ui.pomo_arc) != arc) {
        lv_arc_set_value(ui.pomo_arc, arc);
    }
    dash_pomo_phase_t ph = pomo.phase == DASH_POMO_PAUSED ? pomo.paused_from : pomo.phase;
    uint32_t ac = ph == DASH_POMO_BREAK ? T_GOOD : T_ACCENT;
    if (!lv_color_eq(lv_obj_get_style_arc_color(ui.pomo_arc, LV_PART_INDICATOR), hex(ac))) {
        lv_obj_set_style_arc_color(ui.pomo_arc, hex(ac), LV_PART_INDICATOR);
    }
    static const dash_str_t phase_str[] = {S_POMO_IDLE, S_POMO_FOCUS, S_POMO_BREAK, S_POMO_PAUSED};
    set_text(ui.pomo_phase_lbl, dash_tr(phase_str[pomo.phase]));
    set_text(ui.pomo_btn_lbl[0], dash_tr(pomo.phase == DASH_POMO_IDLE     ? S_POMO_START
                                         : pomo.phase == DASH_POMO_PAUSED ? S_POMO_RESUME
                                                                          : S_POMO_PAUSE));
    snprintf(buf, sizeof(buf), dash_tr(S_POMO_TODAY_FMT), pomo.count, pomo.minutes);
    set_text(ui.pomo_stats, buf);

    int look = ph == DASH_POMO_FOCUS   ? DASH_LOOK_FOCUS
               : ph == DASH_POMO_BREAK ? DASH_LOOK_YAWN
                                       : DASH_LOOK_IDLE;
    if (pomo.phase == DASH_POMO_FOCUS && (tick_now() / 400) % 2) {
        look = DASH_LOOK_FOCUS_2;
    }
    if (ui.pomo_cat.px && look != ui.pomo_cat_drawn) {
        dash_pix_clear(&ui.pomo_cat, 0);
        dash_cat_draw_look_into(&ui.pomo_cat, look, 0, 0, 2);
        dash_pix_commit(&ui.pomo_cat);
        ui.pomo_cat_drawn = look;
    }
}

static void update_mail(const dash_state_t *st, int64_t now)
{
    char buf[48];
    (void)buf;
    set_text(ui.mail_status, !st->mqtt_configured ? dash_tr(S_MUSE_NOT_SET)
                             : st->mqtt_connected ? "MQTT " LV_SYMBOL_OK
                                                  : "MQTT " LV_SYMBOL_CLOSE);
    show(ui.mail_empty, st->muse_count == 0);
    set_text(ui.mail_empty, dash_tr(S_MUSE_NONE)); /* Mi Home messages arrive even without MQTT */
    for (int i = 0; i < DASH_MAX_MUSE; ++i) {
        mail_row_t *m = &ui.mail[i];
        if (i >= st->muse_count) {
            show(m->box, false);
            continue;
        }
        show(m->box, true);
        const dash_muse_msg_t *msg = &st->muse[i];
        char hm[12], ago[24], head[96];
        dash_fmt_hhmm(msg->ts, hm, sizeof(hm));
        dash_fmt_ago(now - msg->ts, dash_i18n_get(), ago, sizeof(ago));
        if (msg->src == DASH_SRC_MUSE) {
            snprintf(head, sizeof(head), "%s  %s   %s", msg_icon(msg), hm, ago);
        } else {
            snprintf(head, sizeof(head), "%s  %s%s%s  %s", msg_icon(msg), src_name(msg->src),
                     msg->project[0] ? " · " : "", msg->project, hm);
        }
        set_text(m->time, head);
        if (msg->text[0]) {
            set_text(m->text, msg->text);
        } else {
            agent_title(msg, head, sizeof(head));
            set_text(m->text, head);
        }
        if (!lv_color_eq(lv_obj_get_style_border_color(m->box, 0), hex(msg_color(msg)))) {
            lv_obj_set_style_border_color(m->box, hex(msg_color(msg)), 0);
        }
    }
}

static const char *mi_name(const dash_mi_dev_t *d)
{
    const char *n = dash_i18n_get() == DASH_LANG_ZH ? d->name_zh : d->name_en;
    return n[0] ? n : d->id;
}

static uint32_t mi_washer_color(const dash_mi_dev_t *d)
{
    if (!d->online) {
        return T_GREY;
    }
    switch (d->state) {
    case DASH_MI_ST_DONE:
        return T_GOOD;
    case DASH_MI_ST_FAULT:
        return T_BAD;
    case DASH_MI_ST_PAUSE:
        return T_WARN;
    case DASH_MI_ST_WASH:
    case DASH_MI_ST_RINSE:
    case DASH_MI_ST_SPIN:
    case DASH_MI_ST_DRY:
        return T_INFO;
    default:
        return T_GREY;
    }
}

static void set_bar(lv_obj_t *bar, int v)
{
    if (lv_bar_get_value(bar) != v) {
        lv_bar_set_value(bar, v, LV_ANIM_OFF);
    }
}

/* 0.1 kg units -> "+0.3" / "-1.2" / "0.0" */
static void fmt_tenths(int v, char *buf, size_t n)
{
    int a = v < 0 ? -v : v;
    snprintf(buf, n, "%s%d.%d", v > 0 ? "+" : v < 0 ? "-" : "", a / 10, a % 10);
}

static void update_mi_water(mi_card_t *c, const dash_mi_dev_t *d, bool online)
{
    char buf[48];
    bool busy = online && (d->state == DASH_MI_ST_HEAT || d->state == DASH_MI_ST_POUR || d->state == DASH_MI_ST_CLEAN);
    bool fault = online && d->state == DASH_MI_ST_FAULT;
    set_text(c->icon, LV_SYMBOL_TINT);
    set_text_color(c->icon, !online ? T_GREY : fault ? T_BAD : T_INFO);
    /* top right: state + water temperature */
    const char *what = d->state != DASH_MI_ST_OTHER ? dash_tr((dash_str_t)(S_MI_ST_OTHER + d->state)) : "";
    if (!online) {
        snprintf(buf, sizeof(buf), "%s", dash_tr(S_MI_OFFLINE));
    } else if (d->temp_c != DASH_NONE16) {
        snprintf(buf, sizeof(buf), "%s%s%d°C", what, what[0] ? " " : "", d->temp_c);
    } else {
        snprintf(buf, sizeof(buf), "%s", what);
    }
    set_text(c->state, buf);
    set_text_color(c->state, !online ? T_MUTED : fault ? T_BAD : busy ? T_INFO : T_TEXT);
    /* bottom: filter life left, output TDS */
    int f0 = d->filter_pct[0], f1 = d->filter_pct[1];
    int low = f0 >= 0 && f1 >= 0 ? (f0 < f1 ? f0 : f1) : f0 >= 0 ? f0 : f1;
    buf[0] = '\0';
    if (online && f0 >= 0 && f1 >= 0) {
        snprintf(buf, sizeof(buf), dash_tr(S_MI_FILTER_FMT), f0, f1);
    } else if (online && low >= 0) {
        snprintf(buf, sizeof(buf), dash_tr(S_MI_FILTER1_FMT), low);
    }
    show(c->sub, buf[0]);
    set_text(c->sub, buf);
    set_text_color(c->sub, low >= 0 && low < 10 ? T_BAD : low >= 0 && low < 20 ? T_WARN : T_MUTED);
    buf[0] = '\0';
    if (online && d->tds != DASH_NONE16) {
        snprintf(buf, sizeof(buf), "TDS %d", d->tds);
    }
    show(c->left, buf[0]);
    set_text(c->left, buf);
    show(c->spark, false);
}

static void update_weight_spark(mi_card_t *c, const dash_state_t *st)
{
    int lo = INT_MAX, hi = INT_MIN, n = 0;
    for (int i = 0; i < DASH_WEIGHT_DAYS; ++i) {
        int v = st->weight_hist[i];
        if (v != DASH_NONE16) {
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            ++n;
        }
    }
    if (n < 2) {
        show(c->spark, false);
        return;
    }
    if (hi - lo < 6) { /* under 0.6 kg of change stays a near-flat line */
        int mid = (hi + lo) / 2;
        lo = mid - 3, hi = mid + 3;
    }
    lv_point_precise_t pts[DASH_WEIGHT_DAYS];
    int k = 0;
    for (int i = 0; i < DASH_WEIGHT_DAYS; ++i) {
        int v = st->weight_hist[i];
        if (v != DASH_NONE16) {
            pts[k].x = (lv_value_precise_t)(i * (MI_SPARK_W - 2) / (DASH_WEIGHT_DAYS - 1) + 1);
            pts[k].y = (lv_value_precise_t)((hi - v) * (MI_SPARK_H - 3) / (hi - lo) + 1);
            ++k;
        }
    }
    if (k != c->npts || memcmp(pts, c->pts, sizeof(pts[0]) * (size_t)k)) {
        memcpy(c->pts, pts, sizeof(pts[0]) * (size_t)k);
        c->npts = k;
        lv_line_set_points(c->spark, c->pts, (uint32_t)k);
    }
    show(c->spark, true);
}

/* The scale card shows changes only: the weight itself never reaches the device. */
static void update_mi_scale(mi_card_t *c, const dash_mi_dev_t *d, const dash_state_t *st, bool online)
{
    char buf[64], when[24], delta[16];
    bool have = d->weigh_at > 0;
    set_text(c->icon, "kg");
    set_text_color(c->icon, have && online ? T_ACCENT : T_GREY);
    if (have && d->d_last != DASH_NONE16) {
        fmt_tenths(d->d_last, delta, sizeof(delta));
        snprintf(buf, sizeof(buf), "%s kg", delta);
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    set_text(c->state, buf);
    set_text_color(c->state, have ? T_TEXT : T_MUTED);
    if (!have) {
        snprintf(buf, sizeof(buf), "%s", dash_tr(S_MI_WEIGH_NEVER));
    } else {
        int days = (int)(dash_local_day(ui.last_now) - dash_local_day(d->weigh_at));
        if (days <= 0) {
            snprintf(when, sizeof(when), "%s", dash_tr(S_MI_WEIGH_TODAY));
        } else if (days == 1) {
            snprintf(when, sizeof(when), "%s", dash_tr(S_MI_WEIGH_YDAY));
        } else {
            snprintf(when, sizeof(when), dash_tr(S_MI_WEIGH_DAYS_FMT), days);
        }
        if (d->d_30 != DASH_NONE16) {
            fmt_tenths(d->d_30, delta, sizeof(delta));
            snprintf(buf, sizeof(buf), dash_tr(S_MI_WEIGH_30_FMT), when, delta);
        } else {
            snprintf(buf, sizeof(buf), dash_tr(S_MI_WEIGH_WHEN_FMT), when);
        }
    }
    show(c->sub, true);
    set_text(c->sub, buf);
    set_text_color(c->sub, T_MUTED);
    show(c->left, false);
    update_weight_spark(c, st);
}

static void update_mihome(const dash_state_t *st)
{
    char buf[48];
    uint32_t now = tick_now();
    set_text(ui.mi_status, !st->have_mi ? "" : st->mi_ok ? dash_tr(S_MI_LINKED) : dash_tr(S_MI_LOST));
    set_text_color(ui.mi_status, st->mi_ok ? T_GOOD : T_BAD);
    show(ui.mi_empty, !st->have_mi || st->mi_count == 0);
    set_text(ui.mi_empty, dash_tr(S_MI_NONE));
    for (int i = 0; i < DASH_MAX_MI; ++i) {
        mi_card_t *c = &ui.mi[i];
        bool used = i < st->mi_count;
        show(c->shadow, used);
        show(c->card, used);
        if (!used) {
            ui.mi_pend[i].active = false;
            continue;
        }
        const dash_mi_dev_t *d = &st->mi[i];
        bool online = st->mi_ok && d->online;
        bool washer = d->kind == DASH_MI_WASHER;
        set_text(c->name, mi_name(d));
        set_text_color(c->name, online ? T_TEXT : T_MUTED);
        if (d->kind == DASH_MI_DISPENSER || d->kind == DASH_MI_SCALE) {
            ui.mi_pend[i].active = false;
            show(c->btn, false);
            show(c->bar, false);
            show(c->state, true);
            if (d->kind == DASH_MI_DISPENSER) {
                update_mi_water(c, d, online);
            } else {
                update_mi_scale(c, d, st, online);
            }
            continue;
        }
        show(c->spark, false);
        show(c->btn, !washer);
        show(c->state, washer);
        if (washer) {
            uint32_t col = online ? mi_washer_color(d) : T_GREY;
            set_text(c->icon, LV_SYMBOL_REFRESH); /* the drum; the water drop is the dispenser's */
            set_text_color(c->icon, col);
            set_text(c->state, online ? dash_tr((dash_str_t)(S_MI_ST_OTHER + d->state)) : dash_tr(S_MI_OFFLINE));
            set_text_color(c->state, online ? (d->state == DASH_MI_ST_IDLE || d->state == DASH_MI_ST_OFF ? T_MUTED : col)
                                            : T_MUTED);
            bool running = online && dash_mi_running(d);
            bool bar = running && d->pct >= 0;
            show(c->bar, bar);
            if (bar) {
                set_bar(c->bar, d->pct);
                if (!lv_color_eq(lv_obj_get_style_bg_color(c->bar, LV_PART_INDICATOR), hex(col))) {
                    lv_obj_set_style_bg_color(c->bar, hex(col), LV_PART_INDICATOR);
                }
            }
            buf[0] = '\0';
            if (running && d->state != DASH_MI_ST_PAUSE && d->eta > 0) {
                /* the finish time, not a countdown: the washer itself reports its time left
                 * only now and then, the hub dates the last report */
                if (d->eta - ui.last_now < 60) {
                    snprintf(buf, sizeof(buf), "%s", dash_tr(S_MI_ETA_SOON));
                } else {
                    char hm[8];
                    dash_fmt_hhmm(d->eta, hm, sizeof(hm));
                    snprintf(buf, sizeof(buf), dash_tr(S_MI_ETA_FMT), hm);
                }
            }
            show(c->left, bar && buf[0]);
            set_text(c->left, buf);
            show(c->sub, !bar && buf[0]);
            set_text(c->sub, buf);
            continue;
        }
        /* lamp / switch */
        show(c->bar, false);
        show(c->left, false);
        show(c->sub, true);
        if (ui.mi_pend[i].active && (d->on == ui.mi_pend[i].want || !online)) {
            ui.mi_pend[i].active = false;
        } else if (ui.mi_pend[i].active && (int32_t)(now - ui.mi_pend[i].deadline) > 0) {
            ui.mi_pend[i].active = false;
            ui.mi_pend[i].fail_until = now + 4000;
        }
        bool pending = ui.mi_pend[i].active;
        bool failed = ui.mi_pend[i].fail_until && (int32_t)(ui.mi_pend[i].fail_until - now) > 0;
        set_text(c->icon, d->kind == DASH_MI_SWITCH ? LV_SYMBOL_POWER : LV_SYMBOL_CHARGE);
        set_text_color(c->icon, online && d->on ? T_ACCENT : T_GREY);
        if (!online) {
            set_text(c->sub, dash_tr(S_MI_OFFLINE));
        } else if (failed) {
            set_text(c->sub, dash_tr(S_MI_FAILED));
        } else if (d->on && d->bri >= 0) {
            snprintf(buf, sizeof(buf), dash_tr(S_MI_BRI_FMT), d->bri);
            set_text(c->sub, buf);
        } else {
            set_text(c->sub, dash_tr(d->on ? S_MI_IS_ON : S_MI_IS_OFF));
        }
        set_text_color(c->sub, failed ? T_BAD : T_MUTED);
        set_color(c->btn, pending ? T_WARN : online && d->on ? T_ACCENT : T_LINE);
        set_text(c->btn_lbl, pending ? "..." : !online ? "--" : dash_tr(d->on ? S_MI_ON : S_MI_OFF));
        set_text_color(c->btn_lbl, pending || (online && d->on) ? T_BG : online ? T_TEXT : T_MUTED);
    }
}

/* ------------------------------------------------------------------ popups */

static void popup_close(void)
{
    if (ui.popup_timer) {
        lv_timer_delete(ui.popup_timer);
        ui.popup_timer = NULL;
    }
    if (ui.popup) {
        lv_obj_delete(ui.popup);
        ui.popup = NULL;
    }
}

static void popup_timeout(lv_timer_t *t)
{
    ui.popup_timer = NULL; /* auto-deleted (repeat count 1) */
    if (ui.popup) {
        lv_obj_delete(ui.popup);
        ui.popup = NULL;
    }
}

static void popup_clicked(lv_event_t *e)
{
    popup_close();
}

/* Bottom sheet, so Kiki's reaction above it stays visible. */
static void popup_open_f(uint32_t color, const char *icon, const char *title, const char *body, uint32_t ms,
                         const lv_font_t *title_font)
{
    popup_close();
    lv_obj_t *p = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, DASH_UI_W, DASH_UI_H);
    lv_obj_set_style_bg_color(p, hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_40, 0);
    /* The dim backdrop lets touches through, so swiping pages still works while a popup
     * is up; tapping the card closes it early, otherwise the timer does. */
    lv_obj_set_clickable(p, false);

    lv_obj_t *c = lv_obj_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_set_width(c, INNER_W);
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, hex(T_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, T_RADIUS, 0);
    lv_obj_set_style_border_side(c, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(c, 4, 0);
    lv_obj_set_style_border_color(c, hex(color), 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_set_style_outline_width(c, 2, 0);
    lv_obj_set_style_outline_color(c, hex(T_SHADOW), 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(c, false);
    lv_obj_set_clickable(c, true);
    lv_obj_add_event_cb(c, popup_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *head = lv_obj_create(c);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 8, 0);
    lv_obj_set_clickable(head, false);
    lv_obj_t *ic = lv_label_create(head);
    lv_obj_set_style_text_font(ic, F.icon, 0);
    lv_obj_set_style_text_color(ic, hex(color), 0);
    lv_label_set_text(ic, icon);
    lv_obj_t *tl = lv_label_create(head);
    lv_obj_set_style_text_font(tl, title_font, 0);
    lv_obj_set_style_text_color(tl, hex(color), 0);
    lv_obj_set_flex_grow(tl, 1); /* long titles wrap instead of being clipped */
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(tl, title);
    lv_obj_t *bl = lv_label_create(c);
    lv_obj_set_width(bl, LV_PCT(100));
    lv_label_set_long_mode(bl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(bl, &dash_font_zh16, 0);
    lv_obj_set_style_text_color(bl, hex(T_TEXT), 0);
    lv_label_set_text(bl, body);
    lv_obj_align(c, LV_ALIGN_BOTTOM_MID, 0, -20);

    ui.popup = p;
    if (ms) {
        ui.popup_timer = lv_timer_create(popup_timeout, ms, NULL);
        lv_timer_set_repeat_count(ui.popup_timer, 1);
    }
}

static void popup_open(uint32_t color, const char *icon, const char *title, const char *body, uint32_t ms)
{
    popup_open_f(color, icon, title, body, ms, F.title);
}

/* ------------------------------------------------------------------ API */

void dash_ui_create(lv_display_t *disp, dash_lang_t lang)
{
    dash_ui_lang_cb_t lcb = ui.lang_cb;
    dash_ui_pomo_cb_t pcb = ui.pomo_cb;
    dash_ui_mi_cb_t mcb = ui.mi_cb;
    memset(&ui, 0, sizeof(ui));
    ui.debug_activity = ui.debug_night = ui.last_activity = -1;
    ui.lang_cb = lcb;
    ui.pomo_cb = pcb;
    ui.mi_cb = mcb;
    if (disp) {
        lv_display_set_default(disp);
    }
    dash_i18n_set(lang);
    build();
    lv_screen_load(ui.scr);
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i)) {
        dash_ui_attach_indev(i); /* double-tap -> home; pointers created later call it themselves */
    }
}

void dash_ui_update(const dash_state_t *st, int64_t now)
{
    if (st != &ui.last) {
        ui.last = *st;
    }
    ui.have_last = true;
    ui.last_now = now;
    pomo_tick();
    dash_freshness_t fresh = dash_freshness(st, now);
    update_top(st, now, fresh);
    update_home(st, now, fresh);
    update_github(st, now);
    update_servers(st, now);
    update_pomodoro();
    update_mail(st, now);
    update_mihome(st);
    dash_calendar_ui_update(&ui.last.calendar, now, st->clock_valid);
    int64_t sky_now = routine_now();
    if (ui.debug_night == 1) sky_now += (23 - ((sky_now + DASH_TZ_OFFSET_S)/3600)%24)*3600;
    dash_ota_ui_update(&st->ota);
    if (dash_ota_ui_active()) {
        ui.wake_until = tick_now() + 10000;
        dash_flight_ui_interrupt();
    }
    dash_flight_ui_update(&ui.last.flights, now, sky_now,
                          ui.popup != NULL || dash_cat_reacting() != DASH_CAT_REACT_NONE || dash_ota_ui_active());
}

void dash_ui_on_event(const dash_event_t *e)
{
    dash_flight_ui_interrupt();
    char buf[128];
    dash_cat_react_t r = DASH_CAT_REACT_NONE;
    dash_str_t fmt = S_EVT_OTHER, title = S_EVT_OTHER;
    switch (e->kind) {
    case DASH_EVK_RUN_SUCCEEDED:
        r = DASH_CAT_REACT_CI_PASS, fmt = S_REACT_CI_PASS_FMT, title = S_EVT_RUN_SUCCEEDED;
        break;
    case DASH_EVK_RUN_FAILED:
        r = DASH_CAT_REACT_CI_FAIL, fmt = S_REACT_CI_FAIL_FMT, title = S_EVT_RUN_FAILED;
        break;
    case DASH_EVK_RUN_STARTED:
        r = DASH_CAT_REACT_CI_START, fmt = S_REACT_CI_START_FMT, title = S_EVT_RUN_STARTED;
        break;
    case DASH_EVK_SERVER_DOWN:
        r = DASH_CAT_REACT_SERVER_DOWN, fmt = S_REACT_SERVER_DOWN_FMT, title = S_EVT_SERVER_DOWN;
        break;
    case DASH_EVK_SERVER_UP:
        r = DASH_CAT_REACT_SERVER_UP, fmt = S_REACT_SERVER_UP_FMT, title = S_EVT_SERVER_UP;
        break;
    default:
        break;
    }
    if (r == DASH_CAT_REACT_NONE) {
        snprintf(buf, sizeof(buf), "%s", e->subject);
    } else {
        snprintf(buf, sizeof(buf), dash_tr(fmt), e->subject);
    }
    dash_cat_react(r);
    bubble_say(buf, 8000);
    if (ui.page != 0) {
        uint32_t color = e->level == DASH_EVT_ERROR ? T_BAD : e->level == DASH_EVT_SUCCESS ? T_GOOD : T_INFO;
        const char *icon = e->level == DASH_EVT_ERROR     ? LV_SYMBOL_WARNING
                           : e->level == DASH_EVT_SUCCESS ? LV_SYMBOL_OK
                                                          : LV_SYMBOL_BELL;
        popup_open(color, icon, dash_tr(title), e->subject, 3500);
    }
}

void dash_ui_show_event(const dash_event_t *e, uint32_t ms)
{
    (void)ms;
    dash_ui_on_event(e);
}

void dash_ui_show_muse(const char *utf8, int64_t ts, uint32_t ms)
{
    dash_flight_ui_interrupt();
    (void)ts;
    dash_cat_react(DASH_CAT_REACT_MUSE);
    bubble_say(utf8, ms + 2000);
    popup_open(T_ACCENT, LV_SYMBOL_ENVELOPE, dash_tr(S_MUSE_POPUP), utf8, ms);
}

void dash_ui_show_agent(const dash_muse_msg_t *m, uint32_t ms)
{
    dash_flight_ui_interrupt();
    char title[64], bubble[DASH_MUSE_TEXT + 32];
    agent_title(m, title, sizeof(title));
    dash_cat_react(m->kind == DASH_MSG_DONE        ? DASH_CAT_REACT_CI_PASS
                   : m->kind == DASH_MSG_ATTENTION ? DASH_CAT_REACT_CI_START
                   : m->kind == DASH_MSG_ERROR     ? DASH_CAT_REACT_CI_FAIL
                                                   : DASH_CAT_REACT_TOUCH);
    const char *what = m->text[0] ? m->text : title;
    if (m->project[0] && m->src != DASH_SRC_KIKI) { /* Kiki's diary: the title already says it */
        snprintf(bubble, sizeof(bubble), "%s：%s", m->project, what);
    } else {
        snprintf(bubble, sizeof(bubble), "%s", what);
    }
    bubble_say(bubble, ms + 2000);
    popup_open_f(msg_color(m), msg_icon(m), title, bubble, ms, &dash_font_zh16); /* fits \"Claude Code 出错了\" */
}

void dash_ui_set_lang(dash_lang_t lang)
{
    dash_i18n_set(lang);
    rebuild();
}

dash_lang_t dash_ui_get_lang(void)
{
    return dash_i18n_get();
}

void dash_ui_set_lang_cb(dash_ui_lang_cb_t cb)
{
    ui.lang_cb = cb;
}

void dash_ui_set_page(int page, bool anim)
{
    if (page < 0 || page >= DASH_UI_PAGES) {
        return;
    }
    ui.page = page;
    dash_flight_ui_page_changed();
    dash_calendar_ui_page_changed();
    lv_tileview_set_tile_by_index(ui.tv, (uint32_t)page, 0, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    update_dots();
}

void dash_ui_step_page(int delta)
{
    if (!ui.tv || delta == 0) {
        return;
    }
    int page = ((ui.page + delta) % DASH_UI_PAGES + DASH_UI_PAGES) % DASH_UI_PAGES;
    /* a wrap (first <-> last) jumps instead of sliding past every page in between */
    bool wrap = (delta > 0 && page < ui.page) || (delta < 0 && page > ui.page);
    dash_ui_set_page(page, (delta == 1 || delta == -1) && !wrap);
}

int dash_ui_get_page(void)
{
    return ui.page;
}

void dash_ui_set_pomo_cb(dash_ui_pomo_cb_t cb)
{
    ui.pomo_cb = cb;
}

void dash_ui_set_mi_cb(dash_ui_mi_cb_t cb)
{
    ui.mi_cb = cb;
}

void dash_ui_set_calendar_cb(dash_ui_calendar_cb_t cb)
{
    dash_calendar_ui_set_cb(cb);
}

bool dash_ui_mi_event_msg(const dash_state_t *st, const dash_event_t *e, dash_muse_msg_t *out)
{
    if (e->kind != DASH_EVK_MI_DONE) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const dash_mi_dev_t *d = dash_mi_find(st, e->subject);
    const char *name = d ? mi_name(d) : e->subject;
    out->src = DASH_SRC_MIHOME;
    out->kind = DASH_MSG_DONE;
    snprintf(out->project, sizeof(out->project), "%.23s", name);
    if (!d || d->kind == DASH_MI_WASHER) {
        snprintf(out->text, sizeof(out->text), "%s", dash_tr(S_MI_WASHER_DONE));
    } else {
        snprintf(out->text, sizeof(out->text), dash_tr(S_MI_DONE_FMT), out->project);
    }
    return true;
}

void dash_ui_pomo_week(dash_pomo_day_t out[DASH_DAYS])
{
    memcpy(out, pomo.week, sizeof(pomo.week));
}

void dash_ui_pomo_week_restore(const dash_pomo_day_t in[DASH_DAYS])
{
    memcpy(pomo.week, in, sizeof(pomo.week));
}

/* ------------------------------------------------------------------ Kiki's diary */

/* Append a clause: "今天" + "专注了…" + "，" + "提交…" */
static void diary_add(char *buf, size_t n, bool *first, const char *text)
{
    size_t len = strlen(buf);
    snprintf(buf + len, n - len, "%s%s", *first ? "" : dash_tr(S_DIARY_SEP), text);
    *first = false;
}

/* Copy without cutting a UTF-8 character in half. */
static void copy_utf8_cut(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n) {
        len = n - 1;
        while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) {
            --len;
        }
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void dash_ui_diary_msg(const dash_state_t *st, int64_t now, bool weekly, dash_muse_msg_t *out)
{
    char text[512], part[96], tmp[16];
    int32_t today = dash_local_day(now);
    /* today's pomodoros may not be in the ring yet (e.g. first day after an update) */
    if (pomo.yday == local_yday(now)) {
        dash_pomo_week_put(pomo.week, today, pomo.count, pomo.minutes);
    }

    /* the hub's per-day counts; the last entry is today if its date matches */
    char today_md[8];
    {
        time_t t = (time_t)(now + DASH_TZ_OFFSET_S);
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(today_md, sizeof(today_md), "%02u-%02u", (unsigned)(tm.tm_mon + 1) % 13u,
                 (unsigned)tm.tm_mday % 32u);
    }
    int pomos = 0, mins = 0, best_n = 0, best_wd = -1;
    unsigned ok = 0, fail = 0, wash = 0, down = 0, commits = 0;
    for (int i = 0; i < DASH_DAYS; ++i) {
        const dash_pomo_day_t *p = &pomo.week[i];
        if (p->day && p->day <= today && (weekly ? p->day > today - 7 : p->day == today)) {
            pomos += p->count;
            mins += p->minutes;
            if (p->count > best_n) {
                best_n = p->count, best_wd = (int)((p->day + 4) % 7);
            }
        }
    }
    bool have_today = st->day_count && !strcmp(st->days[st->day_count - 1].date, today_md);
    for (int i = 0; i < st->day_count; ++i) {
        const dash_day_t *d = &st->days[i];
        if (weekly ? have_today : (have_today && i == st->day_count - 1)) {
            ok += d->ci_ok, fail += d->ci_fail, wash += d->wash, down += d->down, commits += d->commits;
        }
    }
    if (!weekly && !have_today && st->have_contrib) {
        commits = (unsigned)st->contrib_today; /* older hub without "days" */
    }

    text[0] = '\0';
    bool first = true;
    if (pomos) {
        if (weekly) {
            snprintf(part, sizeof(part), dash_tr(S_DIARY_POMO_H_FMT), pomos, mins / 60, mins % 60 / 6);
        } else {
            snprintf(part, sizeof(part), dash_tr(S_DIARY_POMO_FMT), pomos, mins);
        }
        diary_add(text, sizeof(text), &first, part);
    }
    if (commits) {
        snprintf(part, sizeof(part), dash_tr(S_DIARY_COMMITS_FMT), commits);
        diary_add(text, sizeof(text), &first, part);
    }
    if (ok || fail) {
        snprintf(part, sizeof(part), dash_tr(S_DIARY_CI_FMT), ok, fail);
        diary_add(text, sizeof(text), &first, part);
    }
    if (wash) {
        snprintf(part, sizeof(part), dash_tr(S_DIARY_WASH_FMT), wash);
        diary_add(text, sizeof(text), &first, part);
    }
    if (down) {
        snprintf(part, sizeof(part), dash_tr(S_DIARY_DOWN_FMT), down);
        diary_add(text, sizeof(text), &first, part);
    }
    if (weekly && best_n > 1 && best_wd >= 0) {
        snprintf(part, sizeof(part), dash_tr(S_DIARY_BEST_FMT), dash_tr((dash_str_t)(S_WD_SUN + best_wd)), best_n);
        diary_add(text, sizeof(text), &first, part);
    }
    if (weekly) { /* weight change over the week: newest weighing vs the last one a week before */
        int last = -1, before = -1;
        for (int i = DASH_WEIGHT_DAYS - 1; i >= 0 && last < 0; --i) {
            last = st->weight_hist[i] != DASH_NONE16 ? i : -1;
        }
        for (int i = DASH_WEIGHT_DAYS - 8; i >= 0 && before < 0; --i) {
            before = st->weight_hist[i] != DASH_NONE16 ? i : -1;
        }
        if (last >= DASH_WEIGHT_DAYS - 7 && before >= 0) {
            fmt_tenths(st->weight_hist[last] - st->weight_hist[before], tmp, sizeof(tmp));
            snprintf(part, sizeof(part), dash_tr(S_DIARY_WEIGHT_FMT), tmp);
            diary_add(text, sizeof(text), &first, part);
        }
    }

    char full[640];
    if (first) {
        snprintf(full, sizeof(full), "%s", dash_tr(S_DIARY_NOTHING));
    } else {
        dash_str_t end = weekly                               ? S_DIARY_END_WEEK
                         : pomos >= 6                         ? S_DIARY_END_HARD
                         : fail > ok                          ? S_DIARY_END_CI
                         : down                               ? S_DIARY_END_DOWN
                         : pomos == 0 && commits == 0         ? S_DIARY_END_REST
                                                              : S_DIARY_END_OK;
        snprintf(full, sizeof(full), "%s%s%s%s", dash_tr(weekly ? S_DIARY_WEEK : S_DIARY_TODAY), text,
                 dash_tr(S_DIARY_END), dash_tr(end));
    }
    memset(out, 0, sizeof(*out));
    out->src = DASH_SRC_KIKI;
    out->kind = DASH_MSG_INFO;
    out->ts = now;
    snprintf(out->project, sizeof(out->project), "%s", dash_tr(weekly ? S_WEEKLY_TAG : S_DIARY_TAG));
    copy_utf8_cut(out->text, sizeof(out->text), full);
}

/* ------------------------------------------------------------------ rotary knob */

static struct {
    lv_obj_t *toast;
    lv_timer_t *toast_timer, *bri_timer;
    int bri_dev, bri;
    bool bri_active;
    uint32_t bri_last; /* lv tick of the last turn */
} knob = {.bri_dev = -1};

static void toast_hide(lv_timer_t *t)
{
    knob.toast_timer = NULL; /* one-shot: deleted by LVGL */
    if (knob.toast) {
        lv_obj_delete(knob.toast);
        knob.toast = NULL;
    }
}

/* A small label above the page dots, gone after 1.5 s. */
static void toast(const char *text)
{
    if (!knob.toast) {
        knob.toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_text_font(knob.toast, F.body, 0);
        lv_obj_set_style_text_color(knob.toast, hex(T_TEXT), 0);
        lv_obj_set_style_bg_color(knob.toast, hex(T_CARD), 0);
        lv_obj_set_style_bg_opa(knob.toast, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(knob.toast, T_RADIUS, 0);
        lv_obj_set_style_outline_width(knob.toast, 2, 0);
        lv_obj_set_style_outline_color(knob.toast, hex(T_SHADOW), 0);
        lv_obj_set_style_border_side(knob.toast, LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_width(knob.toast, 4, 0);
        lv_obj_set_style_border_color(knob.toast, hex(T_ACCENT), 0);
        lv_obj_set_style_pad_hor(knob.toast, 12, 0);
        lv_obj_set_style_pad_ver(knob.toast, 6, 0);
        lv_obj_align(knob.toast, LV_ALIGN_BOTTOM_MID, 0, -(DOTS_H + 8));
    }
    set_text(knob.toast, text);
    if (knob.toast_timer) {
        lv_timer_reset(knob.toast_timer);
    } else {
        knob.toast_timer = lv_timer_create(toast_hide, 1500, NULL);
        lv_timer_set_repeat_count(knob.toast_timer, 1);
    }
}

/* the first lamp (a switch if there is no lamp) */
static int knob_lamp(void)
{
    int sw = -1;
    for (int i = 0; i < ui.last.mi_count; ++i) {
        if (ui.last.mi[i].kind == DASH_MI_LAMP) {
            return i;
        }
        if (ui.last.mi[i].kind == DASH_MI_SWITCH && sw < 0) {
            sw = i;
        }
    }
    return sw;
}

static void knob_bri_commit(void)
{
    if (knob.bri_timer) {
        lv_timer_delete(knob.bri_timer);
        knob.bri_timer = NULL;
    }
    if (!knob.bri_active) {
        return;
    }
    knob.bri_active = false;
    int i = knob.bri_dev;
    if (i < 0 || i >= ui.last.mi_count) {
        return;
    }
    dash_mi_dev_t *d = &ui.last.mi[i];
    d->on = true; /* optimistic; the hub's answer replaces it */
    d->bri = (int8_t)knob.bri;
    if (ui.mi_cb) {
        ui.mi_cb(d->id, 1, knob.bri);
    }
    update_mihome(&ui.last);
}

static void knob_bri_idle(lv_timer_t *t)
{
    if (knob.bri_active && tick_now() - knob.bri_last >= 400) {
        knob_bri_commit();
    }
}

static void knob_bri_turn(int steps)
{
    char buf[64], pct[8];
    int i = knob_lamp();
    if (i < 0 || !ui.last.mi_ok || !ui.last.mi[i].online || ui.last.mi[i].kind != DASH_MI_LAMP) {
        toast(i < 0 ? dash_tr(S_KNOB_NO_LAMP) : dash_tr(S_MI_OFFLINE));
        return;
    }
    const dash_mi_dev_t *d = &ui.last.mi[i];
    if (!knob.bri_active || knob.bri_dev != i) {
        knob.bri_dev = i;
        knob.bri = d->on && d->bri > 0 ? d->bri : 0; /* from off: the first step lights it dimly */
    }
    knob.bri += steps * 5;
    knob.bri = knob.bri < 1 ? 1 : knob.bri > 100 ? 100 : knob.bri;
    knob.bri_active = true;
    knob.bri_last = tick_now();
    if (!knob.bri_timer) {
        knob.bri_timer = lv_timer_create(knob_bri_idle, 100, NULL);
    }
    snprintf(pct, sizeof(pct), "%d%%", knob.bri);
    snprintf(buf, sizeof(buf), dash_tr(S_KNOB_LAMP_FMT), mi_name(d), pct);
    toast(buf);
}

void dash_ui_knob(dash_knob_ev_t ev, int steps)
{
    char buf[64];
    ui.wake_until = tick_now() + 30000; /* any knob use lights a dimmed night screen */
    switch (ev) {
    case DASH_KNOB_TURN:
        knob_bri_commit();
        dash_ui_step_page(steps);
        break;
    case DASH_KNOB_HOLD_TURN:
        knob_bri_turn(steps);
        break;
    case DASH_KNOB_RELEASE:
        knob_bri_commit();
        break;
    case DASH_KNOB_CLICK: {
        knob_bri_commit();
        int i = knob_lamp();
        if (i < 0) {
            toast(dash_tr(S_KNOB_NO_LAMP));
            break;
        }
        const dash_mi_dev_t *d = &ui.last.mi[i];
        bool will_on = !d->on;
        if (mi_toggle(i)) {
            snprintf(buf, sizeof(buf), dash_tr(S_KNOB_LAMP_FMT), mi_name(d), dash_tr(will_on ? S_MI_ON : S_MI_OFF));
        } else {
            snprintf(buf, sizeof(buf), dash_tr(S_KNOB_LAMP_FMT), mi_name(d),
                     ui.mi_pend[i].active ? "..." : dash_tr(S_MI_OFFLINE));
        }
        toast(buf);
        break;
    }
    case DASH_KNOB_LONG: {
        knob_bri_commit();
        dash_ui_pomo_toggle();
        dash_str_t what = pomo.phase == DASH_POMO_FOCUS   ? S_POMO_FOCUS
                          : pomo.phase == DASH_POMO_BREAK ? S_POMO_BREAK
                          : pomo.phase == DASH_POMO_PAUSED ? S_POMO_PAUSED
                                                           : S_POMO_IDLE;
        snprintf(buf, sizeof(buf), dash_tr(S_KNOB_POMO_FMT), dash_tr(what));
        toast(buf);
        break;
    }
    }
    if (ui.have_last) {
        dash_ui_update(&ui.last, ui.last_now);
    }
}

void dash_ui_pomo_restore(int count, int minutes, int yday)
{
    pomo.count = count;
    pomo.minutes = minutes;
    pomo.yday = yday;
}

void dash_ui_get_status(bool *night, uint8_t *pomo_phase, uint8_t *pomo_pct)
{
    *night = ui.have_last && is_night();
    *pomo_phase = (uint8_t)pomo.phase;
    *pomo_pct = pomo_percent();
}

void dash_ui_debug_activity(int activity)
{
    ui.debug_activity = activity >= 0 && activity < DASH_ACT_COUNT ? activity : -1;
    if (ui.have_last) {
        dash_ui_update(&ui.last, ui.last_now);
    }
}

void dash_ui_debug_night(int mode)
{
    ui.debug_night = mode;
    if (ui.have_last) {
        dash_ui_update(&ui.last, ui.last_now);
    }
}

void dash_ui_debug_react(int reaction)
{
    dash_cat_react((dash_cat_react_t)reaction);
}

bool dash_ui_debug_look(const char *name)
{
    return dash_cat_force_look(name);
}

void dash_ui_debug_time_offset(int64_t seconds)
{
    ui.time_offset = seconds;
    if (ui.have_last) {
        dash_ui_update(&ui.last, ui.last_now);
    }
}

void dash_ui_debug_flyover(void)
{
    dash_flight_ui_debug();
}

bool dash_ui_alert_active(void)
{
    return ui.popup != NULL || dash_cat_reacting() != DASH_CAT_REACT_NONE || dash_ota_ui_active();
}

uint8_t dash_ui_ota_countdown_visible(void) { return dash_ota_ui_countdown(); }

void dash_ui_flight_bubble(bool playing)
{
    if (playing) {
        bubble_say(dash_tr(S_FLIGHT_LOOK), 6500);
        ui.flight_bubble = true;
    } else if (ui.flight_bubble) {
        ui.flight_bubble = false;
        ui.bubble_until = 0;
        if (ui.have_last) update_home(&ui.last, ui.last_now, dash_freshness(&ui.last, ui.last_now));
    }
}
