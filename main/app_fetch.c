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
static int hub_request(const char *url, const char *token, const char *post, int *len)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .cert_pem = hub_pem_start,
        .common_name = DASH_HUB_CN,
        .timeout_ms = 10000,
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
    int post_len = post ? (int)strlen(post) : 0;
    if (post) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }

    int status = -1;
    *len = 0;
    if (esp_http_client_open(c, post_len) == ESP_OK &&
        (!post || esp_http_client_write(c, post, post_len) == post_len)) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int n;
        while (*len < BODY_MAX &&
               (n = esp_http_client_read(c, body + *len, BODY_MAX - *len)) > 0) {
            *len += n;
        }
    }
    body[*len] = '\0';
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return status;
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
    for (;;) {
        if (!campus_net_wait_online(5000)) {
            continue;
        }
        if (!token_loaded || app_take_token_changed()) {
            app_nvs_get_str("dash", "token", token, sizeof(token));
            token_loaded = campus_net_storage_ready();
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

        int len = 0;
        int64_t t0 = esp_timer_get_time();
        int status = hub_request(DASH_HUB_STATUS_URL, token, NULL, &len);
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        bool ok = false;
        dash_state_t *st = app_lock();
        if (status == 200 && !app_demo_active()) {
            ok = dash_model_parse(st, body, (size_t)len);
        }
        if (ok) {
            st->fetch_failures = 0;
            st->last_fetch_ok = time(NULL);
            app_fetch_ok_now();
        } else if (!app_demo_active()) {
            st->fetch_failures++;
        }
        int runs = st->run_count, servers = st->server_count;
        app_unlock(true);
        if (ok) {
            printf("dash: fetch ok http=200 bytes=%d ms=%d runs=%d servers=%d\n", len, ms, runs,
                   servers);
        } else {
            printf("dash: fetch failed http=%d bytes=%d ms=%d%s\n", status, len, ms,
                   status == 401 ? " (token rejected)" : "");
        }

        /* sleep, but wake early when the token changes; Mi Home taps go out right away */
        for (int waited = 0; waited < (ok ? DASH_FETCH_PERIOD_MS : 10000); waited += 500) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (app_take_mi_request(mi_dev, sizeof(mi_dev), &mi_on, &mi_bri)) {
                mi_control(token, mi_dev, mi_on, mi_bri);
            }
            if (app_take_token_changed()) {
                app_nvs_get_str("dash", "token", token, sizeof(token));
                break;
            }
            if (app_demo_active()) {
                break;
            }
        }
    }
}
