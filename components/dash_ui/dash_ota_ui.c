#include "dash_ota_ui.h"
#include "dash_cat.h"
#include "dash_i18n.h"
#include "dash_pix.h"
#include "dash_theme.h"
#include "sprites/dash_sprites.h"
#include <stdio.h>
#include <string.h>

LV_FONT_DECLARE(dash_font_zh16)
LV_FONT_DECLARE(dash_font_zh20)

static struct {
    lv_obj_t *root, *title, *speech, *status, *bar, *number;
    dash_pix_t cat;
    lv_timer_t *timer;
    dash_ota_status_t state;
    dash_lang_t lang;
    uint32_t since;
    int look;
    bool dismissed;
} view;

static void text(lv_obj_t *label, const char *s)
{
    if (strcmp(lv_label_get_text(label), s)) lv_label_set_text(label, s);
}

static bool terminal(dash_ota_phase_t p)
{
    return p == DASH_OTA_SUCCEEDED || p == DASH_OTA_FAILED || p == DASH_OTA_ROLLED_BACK;
}

static void draw(lv_timer_t *unused)
{
    (void)unused;
    if (!view.root) return;
    uint32_t elapsed = lv_tick_get() - view.since;
    if (terminal(view.state.phase) && elapsed >= 8000) {
        view.dismissed = true;
        dash_ota_ui_destroy();
        return;
    }
    int look = DASH_LOOK_CODE;
    int y = 0;
    if (view.state.phase == DASH_OTA_COUNTDOWN) {
        look = DASH_LOOK_MUSE;
        y = (elapsed / 450 % 2) ? -2 : 0;
    } else if (view.state.phase == DASH_OTA_SUCCEEDED) {
        look = elapsed < 1800 ? (elapsed / 160 % 2 ? DASH_LOOK_CI_PASS_2 : DASH_LOOK_CI_PASS) : DASH_LOOK_HAPPY;
        if (elapsed < 1800) y = elapsed / 160 % 2 ? -6 : 0;
    } else if (view.state.phase == DASH_OTA_FAILED || view.state.phase == DASH_OTA_ROLLED_BACK) {
        look = elapsed / 1100 % 2 ? DASH_LOOK_WORRIED : DASH_LOOK_CONFUSED;
    } else {
        look = elapsed / 320 % 2 ? DASH_LOOK_CODE_2 : DASH_LOOK_CODE;
    }
    int frame = look * 16 + y + 8;
    if (frame != view.look) {
        dash_pix_clear(&view.cat, 0);
        dash_cat_draw_look_into(&view.cat, look, 0, 8 + y, 4);
        dash_pix_commit(&view.cat);
        view.look = frame;
    }
}

static lv_obj_t *label(lv_obj_t *parent, int y, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 10, y);
    lv_obj_set_size(l, 216, 26);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    return l;
}

static bool create(void)
{
    view.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(view.root);
    lv_obj_set_size(view.root, 240, 320);
    lv_obj_set_style_bg_color(view.root, lv_color_hex(T_BG), 0);
    lv_obj_set_style_bg_opa(view.root, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(view.root, false);
    lv_obj_set_clickable(view.root, true);
    view.title = label(view.root, 20, &dash_font_zh20, T_ACCENT);
    view.speech = label(view.root, 51, &dash_font_zh16, T_TEXT);
    if (!dash_pix_create(&view.cat, view.root, 128, 144)) {
        lv_obj_delete(view.root);
        view.root = NULL;
        return false;
    }
    lv_obj_set_pos(view.cat.img, 56, 82);
    view.status = label(view.root, 229, &dash_font_zh16, T_TEXT);
    view.number = label(view.root, 249, &lv_font_montserrat_48, T_ACCENT);
    lv_obj_set_height(view.number, 56);
    view.bar = lv_bar_create(view.root);
    lv_obj_set_pos(view.bar, 32, 309);
    lv_obj_set_size(view.bar, 176, 7);
    lv_obj_set_style_bg_color(view.bar, lv_color_hex(T_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_color(view.bar, lv_color_hex(T_ACCENT), LV_PART_INDICATOR);
    view.look = -1;
    view.lang = (dash_lang_t)-1;
    view.timer = lv_timer_create(draw, 80, NULL);
    return true;
}

void dash_ota_ui_update(const dash_ota_status_t *s)
{
    bool changed = s->phase != view.state.phase;
    if (changed) {
        view.since = lv_tick_get();
        view.dismissed = false;
        view.look = -1;
    }
    view.state = *s;
    if (s->phase == DASH_OTA_IDLE) {
        if (view.root) dash_ota_ui_destroy();
        return;
    }
    if (view.dismissed) return;
    if (!view.root && !create()) return;
    lv_obj_set_hidden(view.root, false);
    lv_obj_move_foreground(view.root);
    dash_lang_t lang = dash_i18n_get();
    if (view.lang != lang) {
        bool en = lang == DASH_LANG_EN;
        lv_obj_set_style_text_font(view.title, en ? &lv_font_montserrat_20 : &dash_font_zh20, 0);
        lv_obj_set_style_text_font(view.speech, en ? &lv_font_montserrat_16 : &dash_font_zh16, 0);
        lv_obj_set_style_text_font(view.status, en ? &lv_font_montserrat_16 : &dash_font_zh16, 0);
        view.lang = lang;
    }
    text(view.title, dash_tr(S_OTA_TITLE));
    dash_str_t status = S_OTA_DOWNLOAD, speech = S_OTA_LEARN;
    switch (s->phase) {
    case DASH_OTA_VERIFYING: status = S_OTA_VERIFY; break;
    case DASH_OTA_COUNTDOWN: status = S_OTA_RESTART; speech = S_OTA_BACK_SOON; break;
    case DASH_OTA_VALIDATING: status = S_OTA_VALIDATE; speech = S_OTA_CHECKING; break;
    case DASH_OTA_SUCCEEDED: status = S_OTA_SUCCESS; speech = S_OTA_HAPPY; break;
    case DASH_OTA_FAILED: status = S_OTA_FAILURE; speech = S_OTA_STAY; break;
    case DASH_OTA_ROLLED_BACK: status = S_OTA_ROLLBACK; speech = S_OTA_OLD; break;
    default: break;
    }
    text(view.speech, dash_tr(speech));
    text(view.status, dash_tr(status));
    char n[16] = "";
    unsigned pct = s->total ? (unsigned)((uint64_t)s->received * 100 / s->total) : 0;
    if (pct > 100) pct = 100;
    if (s->phase == DASH_OTA_COUNTDOWN) snprintf(n, sizeof(n), "%u", s->countdown);
    else if (s->phase == DASH_OTA_DOWNLOADING) snprintf(n, sizeof(n), "%u%%", pct);
    text(view.number, n);
    lv_bar_set_value(view.bar, s->phase == DASH_OTA_DOWNLOADING ? (int)pct : 100, LV_ANIM_OFF);
    draw(NULL);
}

bool dash_ota_ui_active(void) { return view.root && !view.dismissed; }
uint8_t dash_ota_ui_countdown(void)
{
    if (dash_ota_ui_active() && view.state.phase == DASH_OTA_COUNTDOWN) {
        lv_obj_move_foreground(view.root);
        return view.state.countdown;
    }
    return 0;
}

void dash_ota_ui_destroy(void)
{
    if (view.timer) lv_timer_delete(view.timer);
    if (view.root) lv_obj_delete(view.root);
    dash_pix_destroy(&view.cat);
    view.timer = NULL;
    view.root = NULL;
}
