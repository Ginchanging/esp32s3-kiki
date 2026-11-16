/* Device-side OTA presentation state; shared with the simulator. No ESP headers. */
#pragma once
#include <stdint.h>

typedef enum {
    DASH_OTA_IDLE = 0,
    DASH_OTA_DOWNLOADING,
    DASH_OTA_VERIFYING,
    DASH_OTA_COUNTDOWN,
    DASH_OTA_VALIDATING,
    DASH_OTA_SUCCEEDED,
    DASH_OTA_FAILED,
    DASH_OTA_ROLLED_BACK,
} dash_ota_phase_t;

typedef struct {
    dash_ota_phase_t phase;
    uint32_t received, total;
    uint8_t countdown;
} dash_ota_status_t;
