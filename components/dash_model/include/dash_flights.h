/* Bounded flight data shared by the ESP32 and simulator. UTC seconds, metres, degrees. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DASH_MAX_FLIGHTS 8
#define DASH_MAX_FLYOVERS 3
#define DASH_FLIGHT_UNKNOWN (-32768)

typedef struct {
    char hex[7], callsign[9], registration[13], model[9], flight[9];
    char airline[40], origin[5], destination[5], origin_city[48], destination_city[48];
    int64_t observed_at;
    int32_t distance_m, altitude_m;
    int16_t bearing, heading;
    bool eligible;
} dash_flight_t;

typedef struct {
    int64_t id, at;
    dash_flight_t flight;
    bool demo; /* local UI/simulator testing only; never supplied by the live service */
} dash_flyover_t;

typedef struct {
    bool have, configured, ok;
    char place[48];
    int64_t fetched_at;
    int32_t radius_m, alert_radius_m;
    int fetch_failures, count, event_count;
    dash_flight_t aircraft[DASH_MAX_FLIGHTS];
    dash_flyover_t events[DASH_MAX_FLYOVERS];
} dash_flights_t;

typedef enum {
    DASH_FLIGHT_WAIT = 0, DASH_FLIGHT_LIVE, DASH_FLIGHT_STALE, DASH_FLIGHT_ERROR, DASH_FLIGHT_DISABLED
} dash_flight_status_t;

typedef struct { bool baseline; int64_t last_id; } dash_flight_cursor_t;

/* Accepts a flight endpoint reply, or a fixture's "flights" object. Atomic on malformed input. */
bool dash_flights_parse(dash_flights_t *st, const char *json, size_t len);
dash_flight_status_t dash_flights_status(const dash_flights_t *st, int64_t now);
/* First valid snapshot establishes a baseline; stale events are never replayed. */
int dash_flights_take_new(dash_flight_cursor_t *cursor, const dash_flights_t *st, int64_t now,
                          dash_flyover_t *out, int cap);
