/*
 * Muse alerts via Bemfa cloud MQTT over TLS (bemfa.com:9503, client id = Bemfa UID,
 * topic "musealert"). Same behaviour as Muse's esp32_bemfa_alert.ino, but on
 * the campus WPA2-Enterprise connection and with replay protection.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "campus_net.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

static esp_mqtt_client_handle_t client;
static int64_t subscribed_at_ms;
static uint32_t last_hash;       /* musealert */
static uint32_t last_agent_hash; /* agentalert */
static char frag_topic[24];
static char frag[DASH_MUSE_TEXT];
static int frag_len;

static uint32_t fnv1a(const char *p, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; ++i) {
        h = (h ^ (uint8_t)p[i]) * 16777619u;
    }
    return h;
}

static void set_mqtt_state(bool configured, bool connected)
{
    dash_state_t *st = app_lock();
    bool changed = st->mqtt_configured != configured || st->mqtt_connected != connected;
    st->mqtt_configured = configured;
    st->mqtt_connected = connected;
    app_unlock(changed);
}

static void deliver_agent(const char *text, int len)
{
    uint32_t h = fnv1a(text, len);
    if (h == last_agent_hash && esp_timer_get_time() / 1000 - subscribed_at_ms < 3000) {
        printf("dash: agent replay ignored\n");
        return;
    }
    static dash_muse_msg_t m; /* ~300 B, keep it off the MQTT task stack */
    if (!dash_agent_parse(text, (size_t)len, &m)) {
        printf("dash: agentalert payload is not agent-bemfa-notify JSON\n");
        return;
    }
    last_agent_hash = h;
    app_nvs_set_u32("muse", "agent_hash", h);
    app_agent_arrived(&m, time(NULL));
    printf(CAMPUS_USB_PREFIX "agent text=");
    for (int i = 0; i < len; ++i) {
        printf("%02x", (uint8_t)text[i]);
    }
    printf("\n");
}

static void deliver(const char *text, int len)
{
    uint32_t h = fnv1a(text, len);
    int64_t now_ms = esp_timer_get_time() / 1000;
    /* Bemfa re-sends the topic's last message right after (re)subscribing. */
    if (h == last_hash && now_ms - subscribed_at_ms < 3000) {
        printf("dash: muse replay ignored\n");
        return;
    }
    last_hash = h;
    app_nvs_set_u32("muse", "last_hash", h);
    app_muse_arrived(text, time(NULL));
    printf(CAMPUS_USB_PREFIX "muse text=");
    for (int i = 0; i < len; ++i) {
        printf("%02x", (uint8_t)text[i]);
    }
    printf("\n");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t ev = data;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        esp_mqtt_client_subscribe(client, MUSE_TOPIC, 0);
        esp_mqtt_client_subscribe(client, AGENT_TOPIC, 0);
        set_mqtt_state(true, true);
        printf("dash: muse mqtt connected\n");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        subscribed_at_ms = esp_timer_get_time() / 1000;
        printf(CAMPUS_USB_PREFIX "muse subscribed=%s\n", MUSE_TOPIC);
        break;
    case MQTT_EVENT_DISCONNECTED:
        set_mqtt_state(true, false);
        printf("dash: muse mqtt disconnected\n");
        break;
    case MQTT_EVENT_DATA:
        if (ev->current_data_offset == 0) {
            frag_len = 0;
            int tl = ev->topic_len < (int)sizeof(frag_topic) - 1 ? ev->topic_len : (int)sizeof(frag_topic) - 1;
            memcpy(frag_topic, ev->topic, (size_t)(tl > 0 ? tl : 0));
            frag_topic[tl > 0 ? tl : 0] = '\0';
        }
        if (ev->total_data_len >= (int)sizeof(frag)) {
            break; /* longer than Bemfa's 256-byte limit: ignore */
        }
        memcpy(frag + ev->current_data_offset, ev->data, (size_t)ev->data_len);
        frag_len = ev->current_data_offset + ev->data_len;
        if (frag_len == ev->total_data_len) {
            frag[frag_len] = '\0';
            if (!strcmp(frag_topic, AGENT_TOPIC)) {
                deliver_agent(frag, frag_len);
            } else {
                deliver(frag, frag_len);
            }
        }
        break;
    case MQTT_EVENT_ERROR:
        printf("dash: muse mqtt error type=%d\n", ev->error_handle ? ev->error_handle->error_type : -1);
        break;
    default:
        break;
    }
}

static void stop_client(void)
{
    if (client) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
    }
}

void muse_task(void *arg)
{
    static char uid[80];
    for (;;) {
        campus_net_wait_online(portMAX_DELAY);
        if (!campus_net_storage_ready()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        app_nvs_get_u32("muse", "last_hash", &last_hash);
        app_nvs_get_u32("muse", "agent_hash", &last_agent_hash);
        app_nvs_get_str("muse", "uid", uid, sizeof(uid));
        if (!uid[0]) {
            set_mqtt_state(false, false);
            while (!app_take_bemfa_changed()) {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }
        esp_mqtt_client_config_t cfg = {
            .broker.address.uri = MUSE_MQTT_URI,
            .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
            .credentials.client_id = uid,
            .session.keepalive = 60,
            .network.reconnect_timeout_ms = 5000,
            .network.timeout_ms = 10000,
        };
        client = esp_mqtt_client_init(&cfg);
        if (!client) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }
        esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, on_event, NULL);
        set_mqtt_state(true, false);
        esp_mqtt_client_start(client); /* reconnects by itself after Wi-Fi drops */
        while (!app_take_bemfa_changed()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        stop_client(); /* UID changed or cleared: start over */
    }
}
