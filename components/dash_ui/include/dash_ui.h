/*
 * Desk dashboard UI (240x320 portrait) starring Kiki the pixel cat.
 * Pure LVGL: the same code runs in the macOS simulator and on the ESP32-S3.
 * Call everything from the LVGL thread.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dash_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_UI_W 240
#define DASH_UI_H 320
#define DASH_UI_PAGES 6 /* 0 Kiki's room, 1 GitHub, 2 servers, 3 pomodoro, 4 mailbox, 5 Mi Home */

typedef void (*dash_ui_lang_cb_t)(dash_lang_t lang);
typedef void (*dash_ui_pomo_cb_t)(int count, int minutes, int yday);
/* Mi Home control request from a tap: on = 0/1 (-1 keep), bri = 1..100 (-1 keep). The app sends it
 * to the hub; the card shows "..." until the device state follows, or "failed" after 12 s. */
typedef void (*dash_ui_mi_cb_t)(const char *dev, int on, int bri);

void dash_ui_create(lv_display_t *disp, dash_lang_t lang);

/* Refresh every widget from *st. Call about once a second and after changes. */
void dash_ui_update(const dash_state_t *st, int64_t now_utc);

/* A new hub event: Kiki reacts; a popup appears when Kiki is not on screen. */
void dash_ui_on_event(const dash_event_t *e);
void dash_ui_show_event(const dash_event_t *e, uint32_t ms); /* = dash_ui_on_event */
/* A Muse alert: Kiki holds up a sign and the message pops up. */
void dash_ui_show_muse(const char *utf8, int64_t ts, uint32_t ms);
/* An AI agent push (Claude Code / Codex via agent-bemfa-notify): Kiki reacts by kind. */
void dash_ui_show_agent(const dash_muse_msg_t *m, uint32_t ms);

void dash_ui_set_lang(dash_lang_t lang); /* rebuilds the screen */
dash_lang_t dash_ui_get_lang(void);
void dash_ui_set_lang_cb(dash_ui_lang_cb_t cb); /* called when the user taps the switch */

void dash_ui_set_page(int page, bool anim);
/* Next/previous page(s); wraps around (the first page's "previous" is the last page). */
void dash_ui_step_page(int delta);
int dash_ui_get_page(void);

/* Pomodoro: 25 min focus / 5 min break. Today's totals are persisted by the app. */
void dash_ui_set_pomo_cb(dash_ui_pomo_cb_t cb);
void dash_ui_pomo_restore(int count, int minutes, int yday);
void dash_ui_pomo_toggle(void); /* start / pause / resume */

/* Mi Home */
void dash_ui_set_mi_cb(dash_ui_mi_cb_t cb);
/* A DASH_EVK_MI_DONE event -> a mailbox message in the current language (false for other events). */
bool dash_ui_mi_event_msg(const dash_state_t *st, const dash_event_t *e, dash_muse_msg_t *out);

/* Pomodoro totals of the last 7 days (for the weekly report); the app keeps them in NVS. */
void dash_ui_pomo_week(dash_pomo_day_t out[DASH_DAYS]);
void dash_ui_pomo_week_restore(const dash_pomo_day_t in[DASH_DAYS]);

/* Kiki's diary (weekly = Sunday's weekly report) as a mailbox message in the current language.
 * The app decides when (dash_diary_due), pushes it into the mailbox and shows it with
 * dash_ui_show_agent(). Counts come from st->days, pomodoros from the week above. */
void dash_ui_diary_msg(const dash_state_t *st, int64_t now, bool weekly, dash_muse_msg_t *out);

/* Rotary knob (EC11). TURN = page; HOLD_TURN (pressed while turning) = lamp brightness,
 * sent once on RELEASE or after a short pause; CLICK = lamp on/off; LONG = pomodoro. */
typedef enum {
    DASH_KNOB_TURN = 0,
    DASH_KNOB_HOLD_TURN,
    DASH_KNOB_RELEASE,
    DASH_KNOB_CLICK,
    DASH_KNOB_LONG,
} dash_knob_ev_t;
void dash_ui_knob(dash_knob_ev_t ev, int steps);

/* What the status LED should know about. */
void dash_ui_get_status(bool *night, uint8_t *pomo_phase, uint8_t *pomo_percent);

/* Testing hooks (CAT serial command, simulator keys). -1 = automatic. */
void dash_ui_debug_activity(int activity);
void dash_ui_debug_night(int mode);
void dash_ui_debug_react(int reaction); /* dash_cat_react_t */
bool dash_ui_debug_look(const char *name);
void dash_ui_debug_time_offset(int64_t seconds); /* shift the routine clock */

#ifdef __cplusplus
}
#endif
