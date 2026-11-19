/* LD2402 receive-only protocol and desk-presence timing, using monotonic ms. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DASH_LD2402_BAUD 115200
#define DASH_LD2402_LEGACY_BAUD 921600
#define DASH_LD2402_MAX_CM 1000
#define DASH_SIT_RANGE_CM 120
#define DASH_SIT_ENTER_MS 3000
#define DASH_SIT_MISS_GRACE_MS 10000
#define DASH_SIT_LEAVE_MS 30000
#define DASH_SIT_STALE_MS 3000
#define DASH_SIT_REMIND_MS 3600000

typedef struct {
    char line[24];
    uint8_t used, streak;
    bool dropping, locked, legacy;
    uint64_t since_ms; /* rate probe start, then timestamp of the last valid report */
} dash_ld2402_t;

void dash_ld2402_start(dash_ld2402_t *rx, uint64_t now_ms);
int dash_ld2402_baud(const dash_ld2402_t *rx);
/* True when the UART must discard buffered bytes and apply the new probe rate. */
bool dash_ld2402_poll(dash_ld2402_t *rx, uint64_t now_ms);
/* -1 = no accepted report, 0 = OFF, 1..1000 = target distance in cm. */
int dash_ld2402_feed(dash_ld2402_t *rx, uint8_t byte, uint64_t now_ms);

typedef enum {
    DASH_SIT_IDLE, DASH_SIT_ENTERING, DASH_SIT_PRESENT, DASH_SIT_AWAY,
} dash_sit_phase_t;

typedef enum {
    DASH_SIT_LINK_WAITING, DASH_SIT_LINK_LIVE, DASH_SIT_LINK_STALE, DASH_SIT_LINK_ERROR,
} dash_sit_link_t;

/* Local snapshot for display and diagnostics. A live distance of zero means OFF. */
typedef struct {
    uint32_t elapsed_s;
    uint16_t distance_cm, range_cm;
    dash_sit_phase_t phase;
    dash_sit_link_t link;
    bool enabled, reminded;
} dash_sit_status_t;

typedef struct {
    uint64_t last_ms, phase_ms, elapsed_ms;
    uint64_t missing_since_ms; /* First OFF or out-of-range report during a confirmed stay, or zero. */
    unsigned distance_cm;
    dash_sit_phase_t phase;
    bool have_report, reminded;
} dash_sit_t;

/* Clear the session while keeping the last report for diagnostics. */
void dash_sit_reset(dash_sit_t *sit);
bool dash_sit_fresh(const dash_sit_t *sit, uint64_t now_ms);
void dash_sit_poll(dash_sit_t *sit, uint64_t now_ms);
/* A zero range disables timing and still records radar diagnostics. */
void dash_sit_report(dash_sit_t *sit, uint64_t now_ms, unsigned distance_cm, unsigned range_cm);
uint64_t dash_sit_elapsed(const dash_sit_t *sit);
/* Call when the UI can show the reminder. A short absence keeps it pending. */
bool dash_sit_take(dash_sit_t *sit, uint64_t now_ms);
