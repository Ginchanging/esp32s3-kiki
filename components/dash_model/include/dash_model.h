/*
 * Shared data model for the desk dashboard.
 * Pure C: compiled into the ESP32-S3 firmware AND the macOS simulator.
 * Never include ESP-IDF headers here.
 *
 * All timestamps are UTC epoch seconds. Display code adds +8h (China time)
 * itself; the process TZ stays UTC because certificate checks rely on it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "dash_flights.h"
#include "dash_calendar.h"
#include "dash_health.h"
#include "dash_idle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_MAX_RUNS 5
#define DASH_MAX_SERVERS 3
#define DASH_MAX_SERVICES 4
#define DASH_MAX_EVENTS 8
#define DASH_MAX_MUSE 5
#define DASH_MUSE_TEXT 260 /* Bemfa caps one message at 256 bytes */
#define DASH_CPU_HIST 60    /* one CPU sample per minute, last hour */
#define DASH_CONTRIB_DAYS 105 /* 15 weeks of GitHub contributions */
#define DASH_MAX_MI 4         /* Mi Home devices shown on the 米家 page */
#define DASH_WEIGHT_DAYS 30   /* scale trend: one value per day */
#define DASH_DAYS 7           /* per-day counts for Kiki's diary, oldest first, today last */
#define DASH_NONE16 INT16_MIN /* "unknown" for the int16 fields below */

#define DASH_STALE_AFTER_S 180      /* hub data older than this is STALE */
#define DASH_OFFLINE_AFTER_FAILS 3  /* consecutive failed fetches => OFFLINE */
#define DASH_TZ_OFFSET_S (8 * 3600) /* display time zone: UTC+8 */

typedef enum { DASH_LANG_EN = 0, DASH_LANG_ZH = 1 } dash_lang_t;

typedef enum {
    DASH_RUN_UNKNOWN = 0,
    DASH_RUN_QUEUED,
    DASH_RUN_RUNNING,
    DASH_RUN_SUCCESS,
    DASH_RUN_FAILURE,
    DASH_RUN_CANCELLED,
    DASH_RUN_SKIPPED,
} dash_run_state_t;

dash_run_state_t dash_run_state(const char *status, const char *conclusion);

typedef struct {
    int64_t id;
    char workflow[48];
    char branch[40];
    int run_number;
    int attempt;
    dash_run_state_t state;
    int duration_s;
    int64_t updated_at;
} dash_run_t;

typedef struct {
    char name[20];
    bool ok;
} dash_service_t;

typedef struct {
    char name[20];
    bool online;
    bool ever_reported;
    int64_t reported_at;
    float cpu_pct, mem_pct, disk_pct, load1;
    int64_t uptime_s;
    int service_count;
    dash_service_t services[DASH_MAX_SERVICES];
    int cpu_hist_len;
    uint8_t cpu_hist[DASH_CPU_HIST]; /* oldest first, percent */
} dash_server_t;

typedef enum { DASH_EVT_INFO = 0, DASH_EVT_SUCCESS, DASH_EVT_ERROR } dash_event_level_t;

/* The hub sends a kind + subject; the device renders it in the current language. */
typedef enum {
    DASH_EVK_OTHER = 0,
    DASH_EVK_RUN_FAILED,
    DASH_EVK_RUN_SUCCEEDED,
    DASH_EVK_SERVER_DOWN,
    DASH_EVK_SERVER_UP,
    DASH_EVK_RUN_STARTED,
    DASH_EVK_MI_DONE, /* a Mi Home washer finished; subject = device id */
} dash_event_kind_t;

typedef struct {
    int64_t id;
    dash_event_kind_t kind;
    dash_event_level_t level;
    char subject[80];
} dash_event_t;

/* Messages in Kiki's mailbox: Muse alerts and AI agent pushes (agentalert topic) */
typedef enum {
    DASH_SRC_MUSE = 0,
    DASH_SRC_CLAUDE,
    DASH_SRC_CODEX,
    DASH_SRC_OTHER,
    DASH_SRC_MIHOME,
    DASH_SRC_KIKI, /* Kiki's own diary / weekly report */
} dash_msg_src_t;
typedef enum { DASH_MSG_INFO = 0, DASH_MSG_DONE, DASH_MSG_ATTENTION, DASH_MSG_ERROR } dash_msg_kind_t;

typedef struct {
    int64_t ts;
    uint8_t src;  /* dash_msg_src_t */
    uint8_t kind; /* dash_msg_kind_t */
    char project[24];
    char text[DASH_MUSE_TEXT];
} dash_muse_msg_t;

/* Mi Home devices, read by the hub from its local Home Assistant (official Xiaomi Home integration) */
typedef enum { DASH_MI_LAMP = 0, DASH_MI_SWITCH, DASH_MI_WASHER, DASH_MI_DISPENSER, DASH_MI_SCALE } dash_mi_kind_t;
typedef enum {
    DASH_MI_ST_OTHER = 0,
    DASH_MI_ST_IDLE,
    DASH_MI_ST_WASH,
    DASH_MI_ST_RINSE,
    DASH_MI_ST_SPIN,
    DASH_MI_ST_DRY,
    DASH_MI_ST_DONE,
    DASH_MI_ST_PAUSE,
    DASH_MI_ST_FAULT,
    DASH_MI_ST_OFF,
    /* water dispenser */
    DASH_MI_ST_HEAT,
    DASH_MI_ST_POUR,
    DASH_MI_ST_CLEAN,
} dash_mi_state_t;

typedef struct {
    char id[16];
    char name_zh[24], name_en[24];
    uint8_t kind;  /* dash_mi_kind_t */
    uint8_t state; /* dash_mi_state_t (washer, dispenser) */
    bool online;
    bool on;          /* lamp / switch */
    int8_t bri;       /* lamp brightness 1..100, -1 unknown */
    int8_t pct;       /* washer progress 0..100, -1 unknown */
    int64_t eta;      /* washer: expected finish (UTC epoch s), 0 unknown */
    /* water dispenser (DASH_NONE16 / -1 = unknown) */
    int16_t temp_c, tds;
    int8_t filter_pct[2];
    /* scale: relative numbers only, in 0.1 kg; the hub keeps the weight itself */
    int64_t weigh_at;      /* last weighing, 0 = never */
    int16_t d_last, d_30;  /* change since the previous weighing / over 30 days */
} dash_mi_dev_t;

/* One day of Kiki's diary counts (from the hub's ledger). */
typedef struct {
    char date[6];  /* "11-14" (China date) */
    uint8_t wday;  /* 0 = Sunday */
    uint16_t ci_ok, ci_fail, wash, down, commits;
} dash_day_t;

/* Pomodoro totals per day, kept by the device (NVS) for the weekly report. */
typedef struct {
    int32_t day; /* dash_local_day(); 0 = empty slot */
    uint16_t count, minutes;
} dash_pomo_day_t;

typedef enum {
    DASH_NET_BOOTING = 0, /* not tried yet */
    DASH_NET_CONNECTING,
    DASH_NET_ONLINE,
    DASH_NET_DOWN,
} dash_net_t;

#include "dash_ota.h"

/* Everything the UI needs, in one plain struct. */
typedef struct {
    /* --- from the hub (/api/device/status) --- */
    bool have_hub;
    int64_t generated_at;
    char repo[48];
    int64_t github_fetched_at;
    int run_count;
    dash_run_t runs[DASH_MAX_RUNS];
    int server_count;
    dash_server_t servers[DASH_MAX_SERVERS];
    int event_count;
    dash_event_t events[DASH_MAX_EVENTS];
    /* GitHub contribution heatmap: levels 0..4, oldest first, Sunday-first weeks */
    bool have_contrib;
    char contrib_user[24];
    uint8_t contrib[DASH_CONTRIB_DAYS];
    int contrib_week, contrib_today;
    /* Mi Home: have_mi = the hub has it configured; mi_ok = Home Assistant answers */
    bool have_mi, mi_ok;
    int mi_count;
    dash_mi_dev_t mi[DASH_MAX_MI];
    /* scale trend, oldest first, relative to the first known day (0.1 kg); DASH_NONE16 = no weighing */
    int16_t weight_hist[DASH_WEIGHT_DAYS];
    /* per-day counts for the diary */
    int day_count;
    dash_day_t days[DASH_DAYS];

    /* Independent flight endpoint: its failures never change dashboard freshness. */
    dash_flights_t flights;
    dash_calendar_t calendar;
    dash_health_t health;
    dash_ota_status_t ota; /* device-owned; independent of hub dashboard freshness */

    /* --- fetch bookkeeping (device side) --- */
    int fetch_failures;
    int64_t last_fetch_ok;

    /* --- device --- */
    dash_net_t net;
    char ip[16];
    int rssi;
    bool clock_valid;
    bool mqtt_configured;
    bool mqtt_connected;
    bool sensor_valid;
    float temp_c, humidity;

    /* --- Muse alerts, newest first --- */
    int muse_count;
    dash_muse_msg_t muse[DASH_MAX_MUSE];
    uint32_t muse_seq; /* increments on every new alert */

    /* --- owned by the UI, mirrored here so the LED can follow them --- */
    bool night;           /* sleep time: LED off, dim screen */
    uint8_t pomo_phase;   /* dash_pomo_phase_t */
    uint8_t pomo_percent; /* progress of the current phase, 0..100 */
} dash_state_t;

typedef enum { DASH_POMO_IDLE = 0, DASH_POMO_FOCUS, DASH_POMO_BREAK, DASH_POMO_PAUSED } dash_pomo_phase_t;

/* --- Kiki's diary: written at 23:00 (China time), a weekly report on Sundays --- */
typedef enum { DASH_DIARY_NONE = 0, DASH_DIARY_DAY, DASH_DIARY_WEEK } dash_diary_t;

/* Days since 1970-01-01 in China time: a day key that keeps counting across years. */
int32_t dash_local_day(int64_t utc);
/* What to write now, given the day the last diary was written (0 = never). */
dash_diary_t dash_diary_due(int64_t utc, int32_t last_written_day);
/* Record today's pomodoro totals in a 7-slot ring (replaces the oldest slot on a new day). */
void dash_pomo_week_put(dash_pomo_day_t week[DASH_DAYS], int32_t day, int count, int minutes);

/* --- Kiki's daily routine (UTC+8). Weekends start later and swap class for sunbathing. --- */
typedef enum {
    DASH_ACT_SLEEP = 0,
    DASH_ACT_EXERCISE,
    DASH_ACT_BREAKFAST,
    DASH_ACT_CLASS,
    DASH_ACT_LUNCH,
    DASH_ACT_NAP,
    DASH_ACT_CODE,
    DASH_ACT_DINNER,
    DASH_ACT_FREE,
    DASH_ACT_WASH,
    DASH_ACT_SUNBATHE,
    DASH_ACT_COUNT
} dash_activity_t;

/* Current activity; *next / *next_min (minutes after local midnight) describe what comes next. */
dash_activity_t dash_schedule_at(int64_t utc, dash_activity_t *next, int *next_min);
bool dash_schedule_is_night(int64_t utc);

typedef enum {
    DASH_FRESH_NONE = 0, /* never got hub data */
    DASH_FRESH_OK,
    DASH_FRESH_STALE,
    DASH_FRESH_OFFLINE,
} dash_freshness_t;

void dash_state_init(dash_state_t *st);

/*
 * Parse the hub JSON into the hub section of *st. Device/Muse fields are kept.
 * Returns false (and leaves *st untouched) on malformed input.
 */
bool dash_model_parse(dash_state_t *st, const char *json, size_t len);

/* Just the "mihome" object of a hub reply (the answer to POST /api/device/mi). */
bool dash_model_parse_mihome(dash_state_t *st, const char *json, size_t len);
/* Device by id, or NULL. */
const dash_mi_dev_t *dash_mi_find(const dash_state_t *st, const char *id);
/* Is the washer in a running phase (wash/rinse/spin/dry/pause)? */
bool dash_mi_running(const dash_mi_dev_t *d);

dash_freshness_t dash_freshness(const dash_state_t *st, int64_t now);

/* Latest run (runs[0]) state, or UNKNOWN. */
dash_run_state_t dash_latest_run(const dash_state_t *st);

int dash_servers_online(const dash_state_t *st);
int dash_servers_known(const dash_state_t *st);

/* Muse alerts: push newest to the front, keep DASH_MAX_MUSE. */
void dash_muse_push(dash_state_t *st, const char *utf8, int64_t ts);
/* Any mailbox message (Muse or an AI agent). */
void dash_msg_push(dash_state_t *st, dash_msg_src_t src, dash_msg_kind_t kind, const char *project,
                   const char *utf8, int64_t ts);
/*
 * Parse an agentalert payload {"v":1,"s":"claude","k":"done","p":"proj","h":"host","m":"text"}
 * into *out (ts not set). Returns false if it isn't one.
 */
bool dash_agent_parse(const char *json, size_t len, dash_muse_msg_t *out);

/*
 * Event tracking. The first call after boot only records a baseline and
 * returns 0, so a reboot never replays old popups. Afterwards returns how
 * many events are new and copies them (oldest first) to out[].
 */
typedef struct {
    bool have_baseline;
    int64_t last_id;
} dash_event_cursor_t;

int dash_events_take_new(dash_event_cursor_t *cursor, const dash_state_t *st,
                         dash_event_t *out, int max_out);

/* --- LED rules (shared by the firmware LED and the simulator's virtual LED) --- */
typedef enum {
    DASH_LED_OFF = 0,
    DASH_LED_MUSE_FLASH, /* white: 6 fast blinks then 3 s solid */
    DASH_LED_RED,
    DASH_LED_YELLOW,
    DASH_LED_GREEN_FLASH, /* green blink for 5 s after a new success */
    DASH_LED_OFFLINE,     /* dim blue slow blink */
    DASH_LED_POMO_FOCUS,  /* pomodoro focus: orange -> red with progress */
    DASH_LED_POMO_BREAK,  /* pomodoro break: soft green breathing */
    DASH_LED_AGENT_DONE,  /* AI agent finished: green blink */
    DASH_LED_AGENT_ATTN,  /* AI agent needs you: yellow fast blink */
    DASH_LED_AGENT_ERROR, /* AI agent failed: red fast blink */
} dash_led_pattern_t;

typedef struct {
    int64_t muse_until_ms;    /* set to now+4800 when a Muse alert arrives */
    int64_t success_until_ms; /* set to now+5000 on a new success event */
    int64_t agent_until_ms;   /* AI agent push: blink until then */
    uint8_t agent_kind;       /* dash_msg_kind_t of that push */
} dash_led_timers_t;

dash_led_pattern_t dash_led_pattern(const dash_state_t *st, const dash_led_timers_t *t,
                                    int64_t now_s, int64_t now_ms);

/* RGB (0..255 each) for a pattern at a given moment; blinking is time based. */
void dash_led_color(dash_led_pattern_t p, const dash_led_timers_t *t, int64_t now_ms,
                    uint8_t *r, uint8_t *g, uint8_t *b);
/* Same, with the pomodoro progress (0..100) for DASH_LED_POMO_FOCUS. */
void dash_led_color_ex(dash_led_pattern_t p, const dash_led_timers_t *t, int64_t now_ms,
                       uint8_t pomo_percent, uint8_t *r, uint8_t *g, uint8_t *b);

/* --- time formatting (UTC+8) --- */
void dash_fmt_hhmm(int64_t utc, char *buf, size_t n);     /* "12:30" */
void dash_fmt_hhmmss(int64_t utc, char *buf, size_t n);   /* "12:30:05" */
void dash_fmt_date(int64_t utc, dash_lang_t lang, char *buf, size_t n);
void dash_fmt_duration(int64_t seconds, char *buf, size_t n); /* "2m 18s", "1h 05m" */
void dash_fmt_ago(int64_t seconds, dash_lang_t lang, char *buf, size_t n);
void dash_fmt_uptime(int64_t seconds, dash_lang_t lang, char *buf, size_t n);

#ifdef __cplusplus
}
#endif
