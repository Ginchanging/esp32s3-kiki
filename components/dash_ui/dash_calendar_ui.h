#pragma once
#include "dash_ui.h"

void dash_calendar_ui_home(lv_obj_t *home);
void dash_calendar_ui_create(lv_obj_t *tile);
void dash_calendar_ui_destroy(void);
void dash_calendar_ui_update(const dash_calendar_t *st, int64_t now, bool clock_valid);
void dash_calendar_ui_page_changed(void);
void dash_calendar_ui_set_cb(dash_ui_calendar_cb_t cb);
