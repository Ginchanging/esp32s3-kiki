#include "dash_sit.h"

#include <stddef.h>
#include <string.h>

#define LD2402_PROBE_MS 2000
#define LD2402_LOCK_REPORTS 3

void dash_ld2402_start(dash_ld2402_t *rx, uint64_t now_ms)
{
    *rx = (dash_ld2402_t){.since_ms = now_ms};
}

int dash_ld2402_baud(const dash_ld2402_t *rx)
{
    return rx->legacy ? DASH_LD2402_LEGACY_BAUD : DASH_LD2402_BAUD;
}

bool dash_ld2402_poll(dash_ld2402_t *rx, uint64_t now_ms)
{
    uint64_t timeout = rx->locked ? DASH_SIT_STALE_MS : LD2402_PROBE_MS;
    if (now_ms - rx->since_ms < timeout) return false;
    bool legacy = !rx->locked && !rx->legacy;
    *rx = (dash_ld2402_t){.since_ms = now_ms, .legacy = legacy};
    return true;
}

static int parse_line(const char *line, size_t len)
{
    if (len && line[len - 1] == '\r') --len;
    if (len == 3 && !memcmp(line, "OFF", len)) return 0;

    const char prefix[] = "distance:";
    size_t i = sizeof(prefix) - 1;
    if (len <= i || memcmp(line, prefix, i)) return -1;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;
    unsigned cm = 0;
    for (; i < len; ++i) {
        if (line[i] < '0' || line[i] > '9') return -1;
        cm = cm * 10 + (unsigned)(line[i] - '0');
        if (cm > DASH_LD2402_MAX_CM) return -1;
    }
    return cm ? (int)cm : -1;
}

int dash_ld2402_feed(dash_ld2402_t *rx, uint8_t byte, uint64_t now_ms)
{
    if (byte != '\n') {
        if (rx->used == sizeof(rx->line)) rx->dropping = true;
        if (!rx->dropping) rx->line[rx->used++] = (char)byte;
        return -1;
    }

    int cm = rx->dropping ? -1 : parse_line(rx->line, rx->used);
    rx->used = 0;
    rx->dropping = false;
    if (!rx->locked) {
        rx->streak = cm < 0 ? 0 : rx->streak + 1;
        if (rx->streak < LD2402_LOCK_REPORTS) return -1;
        rx->locked = true;
    }
    if (cm >= 0) rx->since_ms = now_ms;
    return cm;
}

void dash_sit_reset(dash_sit_t *sit)
{
    sit->phase = DASH_SIT_IDLE;
    sit->elapsed_ms = 0;
    sit->missing_since_ms = 0;
    sit->reminded = false;
}

bool dash_sit_fresh(const dash_sit_t *sit, uint64_t now_ms)
{
    return sit->have_report && now_ms - sit->last_ms < DASH_SIT_STALE_MS;
}

void dash_sit_poll(dash_sit_t *sit, uint64_t now_ms)
{
    uint64_t missing_ms = sit->missing_since_ms ? now_ms - sit->missing_since_ms : 0;
    if (!dash_sit_fresh(sit, now_ms) || missing_ms >= DASH_SIT_LEAVE_MS) {
        dash_sit_reset(sit);
    } else if (sit->phase == DASH_SIT_PRESENT && missing_ms >= DASH_SIT_MISS_GRACE_MS) {
        sit->elapsed_ms += sit->missing_since_ms + DASH_SIT_MISS_GRACE_MS - sit->phase_ms;
        sit->phase = DASH_SIT_AWAY;
    }
}

void dash_sit_report(dash_sit_t *sit, uint64_t now_ms, unsigned distance_cm, unsigned range_cm)
{
    dash_sit_poll(sit, now_ms);
    sit->have_report = true;
    sit->last_ms = now_ms;
    sit->distance_cm = distance_cm;
    if (!range_cm) {
        dash_sit_reset(sit);
        return;
    }

    bool near = distance_cm && distance_cm <= range_cm;
    switch (sit->phase) {
    case DASH_SIT_IDLE:
        if (near) {
            sit->phase = DASH_SIT_ENTERING;
            sit->phase_ms = now_ms;
        }
        break;
    case DASH_SIT_ENTERING:
        if (!near) dash_sit_reset(sit);
        else if (now_ms - sit->phase_ms >= DASH_SIT_ENTER_MS) {
            sit->phase = DASH_SIT_PRESENT;
            sit->phase_ms = now_ms;
        }
        break;
    case DASH_SIT_PRESENT:
        if (near) sit->missing_since_ms = 0;
        else if (!sit->missing_since_ms) sit->missing_since_ms = now_ms;
        break;
    case DASH_SIT_AWAY:
        if (near) {
            sit->phase = DASH_SIT_PRESENT;
            sit->phase_ms = now_ms;
            sit->missing_since_ms = 0;
        }
        break;
    }
}

uint64_t dash_sit_elapsed(const dash_sit_t *sit)
{
    return sit->elapsed_ms + (sit->phase == DASH_SIT_PRESENT ? sit->last_ms - sit->phase_ms : 0);
}

bool dash_sit_take(dash_sit_t *sit, uint64_t now_ms)
{
    dash_sit_poll(sit, now_ms);
    if (sit->phase != DASH_SIT_PRESENT || sit->missing_since_ms || sit->reminded ||
        dash_sit_elapsed(sit) < DASH_SIT_REMIND_MS) return false;
    sit->reminded = true;
    return true;
}
