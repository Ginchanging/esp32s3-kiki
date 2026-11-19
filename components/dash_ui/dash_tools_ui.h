#pragma once

#include "dash_ui.h"

#define DASH_TOOL_TITLE_X 50

void dash_tools_ui_create(lv_obj_t *parent);
void dash_sit_ui_create(lv_obj_t *parent);
void dash_sit_ui_update(const dash_sit_status_t *status);
void dash_sit_ui_destroy(void);
void dash_tools_ui_destroy(void);
