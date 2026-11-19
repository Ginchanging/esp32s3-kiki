#include "dash_tools_ui.h"

#include <stdio.h>
#include <string.h>

#include "dash_i18n.h"
#include "dash_pix.h"
#include "dash_theme.h"

LV_FONT_DECLARE(dash_font_zh12)
LV_FONT_DECLARE(dash_font_zh16)
LV_FONT_DECLARE(dash_font_zh20)

enum { ICON_SIZE = 16, CARD_W = 106, CARD_H = 106, SEGMENTS = 6, SEGMENT_W = 33 };

/* One silhouette per tool at native pixel scale. q is the chair's radar echo. */
static const char pixels[DASH_TOOL_COUNT][ICON_SIZE][ICON_SIZE + 1] = {
    [DASH_TOOL_POMODORO] = {
        "................", ".........g......", "........gg......", "....gggggggg....",
        "...rrrgggrrrr...", "..rrrrrrrrrrrr..", "..rRrrrrrrrrrr..", ".rRRrrrwrrrrrsr.",
        ".rRrrrrwrrrrrsr.", ".rrrrrrwwwrrssr.", ".rrrrrrrrrrrssr.", "..rrrrrrrrrssr..",
        "..srrrrrrrssr...", "...ssssssssr....", ".....sssss......", "................",
    },
    [DASH_TOOL_CALENDAR] = {
        "................", "....B......B....", "....B......B....", "..bbBbbbbbbBbb..",
        "..bBBBBBBBBBBb..", "..bBBBBBBBBBBb..", "..bwwwwwwwwwwb..", "..bwwwwwwwwwwb..",
        "..bwwbBwbBwwwb..", "..bwwbBwbBwwwb..", "..bwwwwwwwwwwb..", "..bwwBBrRwwwwb..",
        "..bwwBBrRwwwwb..", "..bwwwwwwwwwwb..", "..bbbbbbbbbbbb..", "................",
    },
    [DASH_TOOL_SIT] = {
        "................", "................", "..dddd..........", "..dccd......q...",
        "..dCcd.......q..", "..dCcd....q...q.", "..dCcd.....q..q.", "..dCcd......q.q.",
        "..dCccccccd.q...", "..dCCCCCCCCd....", "..dddddddddd....", "...dd....dd.....",
        "...dd....dd.....", "...d......d.....", "................", "................",
    },
    [DASH_TOOL_HEALTH] = {
        "................", "................", "....hh....hh....", "...hhhh..hhhh...",
        "..hHhhhhhhhhhh..", "..hHhhwhhhhhhh..", "..hhhwhwhhwhhh..", "..hwwhhwhwhwwh..",
        "...hhhhhwphhp...", "....hhhhhhhpp...", ".....hhhhpp.....", "......hhpp......",
        ".......hp.......", "................", "................", "................",
    },
};

static const dash_str_t names[DASH_TOOL_COUNT] = {
    [DASH_TOOL_POMODORO] = S_POMO_TITLE,
    [DASH_TOOL_CALENDAR] = S_TOOL_CALENDAR,
    [DASH_TOOL_SIT] = S_TOOL_SIT,
    [DASH_TOOL_HEALTH] = S_TOOL_HEALTH,
};

static const uint32_t tile_ink[DASH_TOOL_COUNT] = {
    [DASH_TOOL_POMODORO] = T_BAD,
    [DASH_TOOL_CALENDAR] = T_INFO,
    [DASH_TOOL_SIT] = T_GOOD,
    [DASH_TOOL_HEALTH] = 0xC2A7FA,
};

static dash_pix_t menu_icons[DASH_TOOL_COUNT];

static struct {
    lv_obj_t *phase, *elapsed_label, *progress, *reminder, *link, *link_dot, *distance, *range;
    dash_pix_t chair, elapsed;
    uint32_t echo;
    int filled;
    char last_elapsed[24];
} seat;

static lv_obj_t *block(lv_obj_t *p, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *caption(lv_obj_t *p, int x, int y, int w, const lv_font_t *font,
                         uint32_t ink, lv_text_align_t align, const char *text)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(ink), 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_label_set_text(l, text);
    return l;
}

static void set_value(lv_obj_t *l, const char *text)
{
    if (strcmp(lv_label_get_text(l), text)) lv_label_set_text(l, text);
}

static void draw_icon(dash_pix_t *pic, dash_tool_t tool, int scale, uint32_t echo)
{
    static const uint32_t palette[128] = {
        ['g'] = T_GOOD, ['r'] = T_BAD, ['R'] = 0xFFB39D, ['s'] = 0xB74754,
        ['B'] = T_INFO, ['b'] = 0x3D6B8E, ['w'] = T_TEXT,
        ['C'] = 0x9EE5C1, ['c'] = 0x64B69A, ['d'] = 0x377461,
        ['H'] = 0xDEC9FF, ['h'] = 0xB48CFF, ['p'] = 0x8056B6,
    };
    dash_pix_clear(pic, 0);
    for (int y = 0; y < ICON_SIZE; ++y) {
        for (int x = 0; x < ICON_SIZE; ++x) {
            unsigned char c = (unsigned char)pixels[tool][y][x];
            uint32_t color = c == 'q' ? echo : palette[c];
            if (color) dash_pix_rect(pic, x * scale, y * scale, scale, scale, dash_argb(color));
        }
    }
    dash_pix_commit(pic);
}

static void tool_clicked(lv_event_t *e)
{
    dash_ui_open_tool((dash_tool_t)(intptr_t)lv_event_get_user_data(e));
}

void dash_tools_ui_create(lv_obj_t *parent)
{
    bool zh = dash_i18n_get() == DASH_LANG_ZH;
    caption(parent, 10, 3, 220, zh ? &dash_font_zh20 : &lv_font_montserrat_20,
            T_TEXT, LV_TEXT_ALIGN_LEFT, dash_tr(S_TOOLS_TITLE));
    for (int i = DASH_TOOL_POMODORO; i < DASH_TOOL_COUNT; ++i) {
        int index = i - DASH_TOOL_POMODORO;
        int x = 10 + (index % 2) * 114, y = 39 + (index / 2) * 118;
        block(parent, x + 2, y + 2, CARD_W, CARD_H, T_SHADOW, T_RADIUS);
        lv_obj_t *b = block(parent, x, y, CARD_W, CARD_H, T_CARD, T_RADIUS);
        lv_obj_set_style_bg_color(b, lv_color_hex(T_LINE), LV_STATE_PRESSED);
        lv_obj_set_clickable(b, true);
        lv_obj_add_event_cb(b, tool_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        dash_pix_t *icon = &menu_icons[i];
        if (dash_pix_create(icon, b, ICON_SIZE * 3, ICON_SIZE * 3)) {
            lv_obj_set_pos(icon->img, (CARD_W - ICON_SIZE * 3) / 2, 15);
            draw_icon(icon, (dash_tool_t)i, 3, T_GOOD);
        }
        caption(b, 2, 77, CARD_W - 4, zh ? &dash_font_zh16 : &lv_font_montserrat_12,
                tile_ink[i], LV_TEXT_ALIGN_CENTER, dash_tr(names[i]));
    }
}

static void progress_rect(lv_layer_t *layer, int x, int y, int w, uint32_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.radius = 2;
    lv_area_t a = {x, y, x + w - 1, y + 12};
    lv_draw_rect(layer, &dsc, &a);
}

static void draw_progress(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_coords(seat.progress, &area);
    for (int i = 0; i < SEGMENTS; ++i) {
        int x = area.x1 + i * (SEGMENT_W + 4);
        int filled = seat.filled - i * SEGMENT_W;
        if (filled < SEGMENT_W) progress_rect(layer, x, area.y1, SEGMENT_W, T_LINE);
        if (filled > 0) {
            progress_rect(layer, x, area.y1, filled < SEGMENT_W ? filled : SEGMENT_W, T_ACCENT);
        }
    }
}

void dash_sit_ui_create(lv_obj_t *parent)
{
    bool zh = dash_i18n_get() == DASH_LANG_ZH;
    const lv_font_t *small = zh ? &dash_font_zh12 : &lv_font_montserrat_12;
    const lv_font_t *body = zh ? &dash_font_zh16 : &lv_font_montserrat_14;
    caption(parent, DASH_TOOL_TITLE_X, 3, 180, zh ? &dash_font_zh20 : &lv_font_montserrat_20,
            T_TEXT, LV_TEXT_ALIGN_LEFT, dash_tr(S_TOOL_SIT));

    block(parent, 12, 44, 220, 114, T_SHADOW, T_RADIUS);
    lv_obj_t *hero = block(parent, 10, 42, 220, 114, T_CARD, T_RADIUS);
    if (dash_pix_create(&seat.chair, hero, ICON_SIZE * 5, ICON_SIZE * 5)) {
        lv_obj_set_pos(seat.chair.img, 8, 17);
    }
    if (dash_pix_create(&seat.elapsed, hero, 122, 32)) {
        lv_obj_set_pos(seat.elapsed.img, 88, 30);
    } else {
        seat.elapsed_label = caption(hero, 88, 32, 122, &lv_font_montserrat_20, T_TEXT, LV_TEXT_ALIGN_RIGHT, "");
    }
    seat.phase = caption(hero, 88, 76, 122, body, T_MUTED, LV_TEXT_ALIGN_RIGHT, "");

    seat.progress = block(parent, 11, 172, 218, 13, T_BG, 0);
    lv_obj_add_event_cb(seat.progress, draw_progress, LV_EVENT_DRAW_MAIN, NULL);
    seat.reminder = caption(parent, 11, 190, 95, small, T_ACCENT, LV_TEXT_ALIGN_LEFT, "");
    char goal[24];
    snprintf(goal, sizeof(goal), dash_tr(S_SIT_TARGET_FMT), (unsigned)(DASH_SIT_REMIND_MS / 60000));
    caption(parent, 118, 190, 110, small, T_MUTED, LV_TEXT_ALIGN_RIGHT, goal);

    block(parent, 12, 217, 220, 55, T_SHADOW, T_RADIUS);
    lv_obj_t *radar = block(parent, 10, 215, 220, 55, T_CARD, T_RADIUS);
    caption(radar, 11, 8, 78, &lv_font_montserrat_12, T_MUTED, LV_TEXT_ALIGN_LEFT, "LD2402");
    seat.link_dot = block(radar, 98, 13, 6, 6, T_GREY, 2);
    seat.link = caption(radar, 113, 8, 96, small, T_TEXT, LV_TEXT_ALIGN_LEFT, "");
    block(radar, 11, 27, 198, 1, T_LINE, 0);
    seat.distance = caption(radar, 11, 32, 88, body, T_TEXT, LV_TEXT_ALIGN_LEFT, "");
    seat.range = caption(radar, 102, 34, 107, small, T_MUTED, LV_TEXT_ALIGN_RIGHT, "");
}

static void update_elapsed(uint32_t seconds, bool valid)
{
    char elapsed[sizeof(seat.last_elapsed)];
    unsigned min = (unsigned)(seconds / 60), sec = (unsigned)(seconds % 60);
    if (!valid) snprintf(elapsed, sizeof(elapsed), "--:--");
    else if (min >= 60) snprintf(elapsed, sizeof(elapsed), "%u:%02u:%02u", min / 60, min % 60, sec);
    else snprintf(elapsed, sizeof(elapsed), "%02u:%02u", min, sec);
    if (!strcmp(seat.last_elapsed, elapsed)) return;
    strcpy(seat.last_elapsed, elapsed);
    if (!seat.elapsed.px) {
        set_value(seat.elapsed_label, elapsed);
        return;
    }
    int scale = 4;
    while (dash_pix_text_width(elapsed, scale) > seat.elapsed.w - 2 && scale > 1) --scale;
    int x = seat.elapsed.w - dash_pix_text_width(elapsed, scale) - 2;
    int y = (seat.elapsed.h - 7 * scale) / 2 - 1;
    dash_pix_clear(&seat.elapsed, 0);
    dash_pix_text(&seat.elapsed, elapsed, x + 2, y + 2, scale, dash_argb(T_SHADOW));
    dash_pix_text(&seat.elapsed, elapsed, x, y, scale, dash_argb(T_TEXT));
    dash_pix_commit(&seat.elapsed);
}

void dash_sit_ui_update(const dash_sit_status_t *status)
{
    static const dash_str_t link_text[] = {S_SIT_WAITING, S_SIT_ONLINE, S_SIT_STALE, S_SIT_ERROR};
    static const uint32_t link_colors[] = {T_GREY, T_GOOD, T_WARN, T_BAD};
    bool live = status->link == DASH_SIT_LINK_LIVE;
    bool timing = live && status->enabled;
    update_elapsed(status->elapsed_s, timing);

    const char *phase = "";
    uint32_t echo = T_LINE;
    if (!status->enabled) phase = dash_tr(S_SIT_TIMER_OFF);
    else if (live) {
        switch (status->phase) {
        case DASH_SIT_PRESENT: phase = dash_tr(S_SIT_IN_RANGE); echo = T_GOOD; break;
        case DASH_SIT_ENTERING: phase = dash_tr(S_SIT_CHECKING); echo = T_INFO; break;
        case DASH_SIT_AWAY: phase = dash_tr(S_SIT_AWAY); echo = T_WARN; break;
        case DASH_SIT_IDLE:
            if (status->distance_cm) phase = dash_tr(S_SIT_OUTSIDE);
            break;
        }
    }
    set_value(seat.phase, phase);
    if (seat.chair.px && seat.echo != echo) {
        seat.echo = echo;
        draw_icon(&seat.chair, DASH_TOOL_SIT, 5, echo);
    }

    unsigned target = DASH_SIT_REMIND_MS / 1000;
    unsigned elapsed = timing ? status->elapsed_s : 0;
    if (elapsed > target) elapsed = target;
    int filled = (int)(elapsed * SEGMENTS * SEGMENT_W / target);
    if (filled != seat.filled) {
        seat.filled = filled;
        lv_obj_invalidate(seat.progress);
    }
    set_value(seat.reminder, timing && status->reminded ? dash_tr(S_SIT_REMINDED) : "");

    set_value(seat.link, dash_tr(link_text[status->link]));
    lv_color_t ink = lv_color_hex(link_colors[status->link]);
    if (!lv_color_eq(lv_obj_get_style_bg_color(seat.link_dot, 0), ink)) {
        lv_obj_set_style_bg_color(seat.link_dot, ink, 0);
    }
    char value[30];
    if (live && status->distance_cm) {
        snprintf(value, sizeof(value), "%u cm", (unsigned)status->distance_cm);
        set_value(seat.distance, value);
    } else {
        set_value(seat.distance, live ? dash_tr(S_SIT_NO_TARGET) : "-");
    }
    snprintf(value, sizeof(value), dash_tr(S_SIT_RANGE_FMT), (unsigned)status->range_cm);
    set_value(seat.range, value);
}

void dash_sit_ui_destroy(void)
{
    dash_pix_destroy(&seat.chair);
    dash_pix_destroy(&seat.elapsed);
    memset(&seat, 0, sizeof(seat));
}

void dash_tools_ui_destroy(void)
{
    for (int i = DASH_TOOL_POMODORO; i < DASH_TOOL_COUNT; ++i) dash_pix_destroy(&menu_icons[i]);
}
