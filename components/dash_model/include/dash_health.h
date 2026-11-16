/* Apple Health observations, shared by the firmware and simulator. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DASH_HEALTH_METRICS 9
#define DASH_HEALTH_DAYS 7
#define DASH_HEALTH_WEIGHTS 30
#define DASH_HEALTH_NONE INT32_MIN
#define DASH_HEALTH_WEIGHT_NONE INT16_MIN

typedef enum {
    DASH_HEALTH_STEPS, DASH_HEALTH_DISTANCE, DASH_HEALTH_EXERCISE, DASH_HEALTH_ENERGY,
    DASH_HEALTH_SLEEP, DASH_HEALTH_CORE, DASH_HEALTH_DEEP, DASH_HEALTH_REM, DASH_HEALTH_FAT,
} dash_health_metric_t;

typedef struct {
    bool have, configured, ok, error;
    int32_t day;
    int64_t fetched_at, since, weight_at; /* HA read / onboarding / weight observation, NOT measurement dates */
    int32_t values[DASH_HEALTH_METRICS]; /* distance: m; duration: min; energy: kcal; fat: 0.1% */
    int64_t source_at[DASH_HEALTH_METRICS]; /* HA last_reported, never a HealthKit sample date */
    int32_t activity[DASH_HEALTH_DAYS][2]; /* oldest first: steps, exercise minutes */
    int16_t weight_hist[DASH_HEALTH_WEIGHTS]; /* relative 0.1 kg only; gaps stay gaps */
    int16_t weight_delta, weight_change;
    uint8_t weight_span;
} dash_health_t;

struct cJSON;
void dash_health_init(dash_health_t *st);
bool dash_health_read(dash_health_t *out, const struct cJSON *object);
bool dash_health_parse(dash_health_t *out, const char *json, size_t len);
/* Never carry yesterday's activity into today's cards, including when offline. */
int32_t dash_health_value(const dash_health_t *st, dash_health_metric_t metric, int64_t now);
