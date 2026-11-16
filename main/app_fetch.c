/*
 * Polls the hub (/api/device/status) over HTTPS with a pinned certificate, and
 * sends Mi Home control requests (/api/device/mi) on the same task, one at a time.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "campus_net.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern const char hub_pem_start[] asm("_binary_hub_pem_start");

#define BODY_MAX 8192

static char body[BODY_MAX + 1];

static void wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--) {
        *b++ = 0;
    }
}

/* GET (post == NULL) or POST a JSON body. Returns HTTP status (or -1 on transport
 * error); the reply lands in body/len. */
int app_hub_json(const char *url, const char *token, const char *post, char *out, int cap, int *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .cert_pem = hub_pem_start,
        .common_name = DASH_HUB_CN,
        .timeout_ms = !strcmp(url, DASH_HUB_FLIGHTS_URL) ? 5000 : 10000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(c, "Authorization", auth);
    wipe(auth, sizeof(auth));
    esp_http_client_set_header(c, "Accept", "application/json");
    /* Piggyback non-sensitive capacity counters on the existing status poll.
     * This lets the 24-hour check continue after the USB cable is disconnected. */
    if (!strcmp(url, DASH_HUB_STATUS_URL)) {
        char metric[24];
        snprintf(metric, sizeof(metric), "%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        esp_http_client_set_header(c, "X-Dash-Heap-Free", metric);
        snprintf(metric, sizeof(metric), "%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        esp_http_client_set_header(c, "X-Dash-Heap-Min", metric);
        snprintf(metric, sizeof(metric), "%lld", (long long)(esp_timer_get_time()/1000000));
        esp_http_client_set_header(c, "X-Dash-Uptime", metric);
    }
    int post_len = post ? (int)strlen(post) : 0;
    if (post) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }

    int status = -1;
    *len = 0;
    if (esp_http_client_open(c, post_len) == ESP_OK &&
        (!post || esp_http_client_write(c, post, post_len) == post_len)) {
        int64_t declared_len = esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        if (declared_len > cap || declared_len < 0) status = -1;
        int n;
        while (status >= 0 && *len < cap &&
               (n = esp_http_client_read(c, out + *len, cap - *len)) > 0) {
            *len += n;
        }
        if (status >= 0 && !esp_http_client_is_complete_data_received(c)) status = -1;
    }
    out[*len] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
}

static int hub_request(const char *url, const char *token, const char *post, int *len)
{
    return app_hub_json(url, token, post, body, BODY_MAX, len);
}

/* Demo mode has no hub: flip the demo state so the 米家 page still reacts. */
static void mi_demo(const char *dev, int on, int bri)
{
    dash_state_t *st = app_lock();
    for (int i = 0; i < st->mi_count; ++i) {
        dash_mi_dev_t *d = &st->mi[i];
        if (!strcmp(d->id, dev)) {
            if (on >= 0) {
                d->on = on != 0;
            }
            if (bri > 0) {
                d->bri = (int8_t)bri;
            }
        }
    }
    app_unlock(true);
}

static void mi_control(const char *token, const char *dev, int on, int bri)
{
    if (app_demo_active()) {
        mi_demo(dev, on, bri);
        return;
    }
    char req[96];
    int n = snprintf(req, sizeof(req), "{\"dev\":\"%.15s\"", dev);
    if (on >= 0) {
        n += snprintf(req + n, sizeof(req) - n, ",\"on\":%s", on ? "true" : "false");
    }
    if (bri > 0) {
        n += snprintf(req + n, sizeof(req) - n, ",\"bri\":%d", bri > 100 ? 100 : bri);
    }
    snprintf(req + n, sizeof(req) - n, "}");
    int len = 0;
    int64_t t0 = esp_timer_get_time();
    int status = hub_request(DASH_HUB_MI_URL, token, req, &len);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    bool ok = false;
    if (status == 200) {
        dash_state_t *st = app_lock();
        ok = dash_model_parse_mihome(st, body, (size_t)len);
        app_unlock(ok);
    }
    printf("dash: mi %s on=%d bri=%d http=%d ms=%d%s\n", dev, on, bri, status, ms, ok ? "" : " (failed)");
}

void fetch_task(void *arg)
{
    char token[80] = "";
    bool token_loaded = false;
    bool warned_no_token = false;
    char mi_dev[16];
    int mi_on, mi_bri;
    int64_t next_status = 0, next_flight = 0, next_calendar = 0;
    for (;;) {
        if (!campus_net_wait_online(500)) {
            continue;
        }
        bool token_changed = app_take_token_changed();
        if (!token_loaded || token_changed) {
            app_nvs_get_str("dash", "token", token, sizeof(token));
            token_loaded = campus_net_storage_ready();
            next_status = next_flight = 0;
            next_calendar = 0;
        }
        if (app_demo_active()) {
            if (app_take_mi_request(mi_dev, sizeof(mi_dev), &mi_on, &mi_bri)) {
                mi_demo(mi_dev, mi_on, mi_bri);
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (!token[0]) {
            if (!warned_no_token) {
                printf("dash: no hub token yet (run: campus.py token)\n");
                warned_no_token = true;
            }
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        warned_no_token = false;

        app_ota_poll(token, body, BODY_MAX);

        if (app_take_mi_request(mi_dev, sizeof(mi_dev), &mi_on, &mi_bri)) {
            mi_control(token, mi_dev, mi_on, mi_bri);
        }
        int64_t now_ms = esp_timer_get_time() / 1000;
        int32_t day;
        uint16_t offset;
        uint32_t seq;
        bool calendar_pending = app_take_calendar_request(&day, &offset, &seq);
        if ((calendar_pending || now_ms >= next_calendar) && time(NULL) >= 1700000000) {
            if (!day) day = dash_local_day(time(NULL));
            time_t local_day = (time_t)((int64_t)day*86400);
            struct tm tm;
            gmtime_r(&local_day, &tm);
            char url[180], date[16];
            strftime(date, sizeof(date), "%Y-%m-%d", &tm);
            snprintf(url, sizeof(url), DASH_HUB_CALENDAR_URL "?day=%s&offset=%u", date, (unsigned)offset);
            int calendar_len = 0;
            int http = hub_request(url, token, NULL, &calendar_len);
            dash_state_t *s = app_lock();
            bool current = app_calendar_current_locked(seq) && !app_demo_active();
            bool success = current && http == 200 && dash_calendar_parse(&s->calendar, body, (size_t)calendar_len);
            if (current) {
                if (success) s->calendar.fetch_failures = 0;
                else ++s->calendar.fetch_failures;
            }
            app_unlock(current);
            printf("dash: calendar %s http=%d bytes=%d\n", !current ? "superseded" : success ? "ok" : "failed", http, calendar_len);
            next_calendar = esp_timer_get_time()/1000+(success ? 60000 : 10000);
            /* Keep the normal status/flight deadlines: an agenda request must
             * not starve either one, even during rapid date selections. */
            now_ms = esp_timer_get_time()/1000;
        }
        bool flight = now_ms < next_status;
        if (flight && now_ms < next_flight) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int len = 0;
        int64_t t0 = esp_timer_get_time();
        int status = hub_request(flight ? DASH_HUB_FLIGHTS_URL : DASH_HUB_STATUS_URL, token, NULL, &len);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        bool ok = false;
        dash_state_t *st = app_lock();
        if (status == 200 && !app_demo_active()) {
            ok = flight ? dash_flights_parse(&st->flights, body, (size_t)len)
                        : dash_model_parse(st, body, (size_t)len);
        }
        if (flight) {
            if (ok) st->flights.fetch_failures = 0;
            else if (!app_demo_active()) ++st->flights.fetch_failures;
        } else if (ok) {
            st->fetch_failures = 0;
            st->last_fetch_ok = time(NULL);
            app_fetch_ok_now();
            app_ota_hub_ok();
        } else if (!app_demo_active()) {
            st->fetch_failures++;
        }
        int runs = st->run_count, servers = st->server_count;
        int aircraft = st->flights.count;
        app_unlock(true);
        if (flight) {
            printf("dash: flights %s http=%d bytes=%d ms=%d aircraft=%d\n", ok ? "ok" : "failed",
                   status, len, ms, aircraft);
        } else if (ok) {
            printf("dash: fetch ok http=200 bytes=%d ms=%d runs=%d servers=%d\n", len, ms, runs,
                   servers);
        } else {
            printf("dash: fetch failed http=%d bytes=%d ms=%d%s\n", status, len, ms,
                   status == 401 ? " (token rejected)" : "");
        }

        int64_t done_ms = esp_timer_get_time() / 1000;
        if (flight) next_flight = done_ms + DASH_FLIGHT_PERIOD_MS;
        else next_status = done_ms + (ok ? DASH_FETCH_PERIOD_MS : 10000);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
