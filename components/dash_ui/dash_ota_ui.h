#pragma once
#include "dash_ota.h"
#include <stdbool.h>

void dash_ota_ui_update(const dash_ota_status_t *status);
bool dash_ota_ui_active(void);
uint8_t dash_ota_ui_countdown(void);
void dash_ota_ui_destroy(void);
