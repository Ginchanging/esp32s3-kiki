#pragma once
#include "dash_ui.h"

void dash_health_ui_create(lv_obj_t *tile);
void dash_health_ui_destroy(void);
void dash_health_ui_update(const dash_health_t *st, int64_t now, bool clock_valid);
