#pragma once
#include "dash_model.h"
#include "lvgl.h"

void dash_flight_ui_create(lv_obj_t *tile, lv_obj_t *home);
void dash_flight_ui_destroy(void);
void dash_flight_ui_update(const dash_flights_t *st, int64_t now, int64_t scene_now, bool busy);
void dash_flight_ui_notify(const dash_flyover_t *event);
void dash_flight_ui_interrupt(void);
void dash_flight_ui_page_changed(void);
void dash_flight_ui_step(int delta);
bool dash_flight_ui_scene_active(void);
void dash_flight_ui_debug(void);
bool dash_ui_alert_active(void); /* queried by the animation timer, without one-second latency */
void dash_ui_flight_bubble(bool playing);
