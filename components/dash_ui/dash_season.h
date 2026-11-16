#pragma once
#include "dash_ui.h"

void dash_season_create(lv_obj_t *home, lv_obj_t *sky, lv_obj_t *plant);
void dash_season_foreground(void);
void dash_season_update(int64_t now, bool clock_valid, bool night);
void dash_season_destroy(void);
