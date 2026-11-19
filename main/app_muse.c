/*
 * Mailbox alerts via Bemfa cloud MQTT over TLS. Muse and iPhone send plain
 * text, and agent-bemfa-notify sends JSON. The client ID is the Bemfa UID.
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

typedef struct {
    const char *name;
    const char *hash_key;
    uint32_t last_hash;
    bool have_hash;
    int subscription_id;
    int64_t subscribed_at_ms;
} bemfa_topic_t;

enum { TOPIC_MUSE, TOPIC_AGENT, TOPIC_IPHONE, TOPIC_COUNT };
static bemfa_topic_t topics[TOPIC_COUNT] = {
    [TOPIC_MUSE] = {.name = MUSE_TOPIC, .hash_key = "last_hash"},
    [TOPIC_AGENT] = {.name = AGENT_TOPIC, .hash_key = "agent_hash"},
    [TOPIC_IPHONE] = {.name = IPHONE_TOPIC, .hash_key = "iphone_hash"},
};

static esp_mqtt_client_handle_t client;
static bemfa_topic_t *frag_topic;
static char frag[DASH_MUSE_TEXT];

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

static bemfa_topic_t *find_topic(const char *name, size_t len)
{
    for (size_t i = 0; i < TOPIC_COUNT; ++i) {
        if (len == strlen(topics[i].name) && !memcmp(name, topics[i].name, len)) {
            return &topics[i];
        }
    }
    return NULL;
}

static void deliver(bemfa_topic_t *topic, const char *text, int len)
{
    bool agent = topic == &topics[TOPIC_AGENT];
    bool iphone = topic == &topics[TOPIC_IPHONE];
    if (iphone && !len) {
        return;
    }
    uint32_t h = fnv1a(text, len);
    int64_t now_ms = esp_timer_get_time() / 1000;
    /* Bemfa re-sends the topic's last message right after (re)subscribing. */
    if (topic->have_hash && topic->last_hash == h && now_ms - topic->subscribed_at_ms < 3000) {
        printf("dash: %s replay ignored\n", topic->name);
        return;
    }
    static dash_muse_msg_t m; /* keep it off the MQTT task stack */
    if (agent && !dash_agent_parse(text, (size_t)len, &m)) {
        printf("dash: agentalert payload is not agent-bemfa-notify JSON\n");
        return;
    }
    topic->last_hash = h;
    topic->have_hash = true;
    app_nvs_set_u32("muse", topic->hash_key, h);
    if (agent) {
        app_agent_arrived(&m, time(NULL));
    } else {
        app_text_alert_arrived(iphone ? DASH_SRC_IPHONE : DASH_SRC_MUSE, text, time(NULL));
    }

    if (iphone) {
        printf(CAMPUS_USB_PREFIX "iphone received=1\n");
        return;
    }
    printf(CAMPUS_USB_PREFIX "%s text=", agent ? "agent" : "muse");
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
        for (size_t i = 0; i < TOPIC_COUNT; ++i) {
            bemfa_topic_t *topic = &topics[i];
            topic->subscribed_at_ms = esp_timer_get_time() / 1000;
            topic->subscription_id = esp_mqtt_client_subscribe(client, topic->name, 0);
            if (topic->subscription_id < 0) {
                printf("dash: could not subscribe to %s\n", topic->name);
            }
        }
        set_mqtt_state(true, true);
        printf("dash: muse mqtt connected\n");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        for (size_t i = 0; i < TOPIC_COUNT; ++i) {
            bemfa_topic_t *topic = &topics[i];
            if (topic->subscription_id != ev->msg_id) {
                continue;
            }
            topic->subscription_id = -1;
            if (ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
                printf("dash: subscription rejected for %s\n", topic->name);
            } else {
                topic->subscribed_at_ms = esp_timer_get_time() / 1000;
                printf(CAMPUS_USB_PREFIX "muse subscribed=%s\n", topic->name);
            }
            break;
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        frag_topic = NULL;
        set_mqtt_state(true, false);
        printf("dash: muse mqtt disconnected\n");
        break;
    case MQTT_EVENT_DATA: {
        if (ev->current_data_offset == 0) {
            frag_topic = find_topic(ev->topic, (size_t)ev->topic_len);
        }
        if (!frag_topic || ev->total_data_len >= (int)sizeof(frag)) {
            break;
        }
        if (ev->data_len) {
            memcpy(frag + ev->current_data_offset, ev->data, (size_t)ev->data_len);
        }
        int frag_len = ev->current_data_offset + ev->data_len;
        if (frag_len == ev->total_data_len) {
            frag[frag_len] = '\0';
            deliver(frag_topic, frag, frag_len);
            frag_topic = NULL;
        }
        break;
    }
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
        for (size_t i = 0; i < TOPIC_COUNT; ++i) {
            bemfa_topic_t *topic = &topics[i];
            topic->have_hash = app_nvs_get_u32("muse", topic->hash_key, &topic->last_hash) == ESP_OK;
        }
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
