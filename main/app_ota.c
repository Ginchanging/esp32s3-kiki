/* Application-only OTA. The fetch task owns the downloader and its small buffer.
 * A short-lived verifier watches the UI independently of network requests. */
#include "app.h"
#include "campus_net.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define OTA_LAYOUT "dual6-v1"
#define OTA_SLOT_SIZE 0x600000
#define OTA_CHECK_URL DASH_HUB_BASE "/api/device/ota/check"
#define OTA_REPORT_URL DASH_HUB_BASE "/api/device/ota/report"
#define RECORD_MAGIC 0x3141544f

extern const char hub_pem_start[] asm("_binary_hub_pem_start");

typedef struct {
    uint32_t magic;
    char job[33], target[65], error[48];
    uint8_t phase; /* 1 = download, 2 = switching, otherwise terminal dash_ota_phase_t + 16 */
    bool acknowledged;
} ota_record_t;

static ota_record_t record;
static bool record_loaded, resolving_boot;
static atomic_bool verified, hub_ok;
static atomic_uint ui_heartbeat, presented;
static char elf_sha[65], device_id[13];
static int64_t next_check;
static bool boot_pending;

static void hex(const uint8_t *src, size_t n, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) {
        dst[i * 2] = digits[src[i] >> 4];
        dst[i * 2 + 1] = digits[src[i] & 15];
    }
    dst[n * 2] = 0;
}

static bool hex_string(const char *s, size_t n)
{
    if (!s || strlen(s) != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return false;
    return true;
}

static void state(dash_ota_phase_t phase, uint32_t bytes, uint32_t total, uint8_t countdown)
{
    dash_state_t *s = app_lock();
    s->ota = (dash_ota_status_t){phase, bytes, total, countdown};
    app_unlock(true);
}

static bool save_record(void)
{
    return app_nvs_set_blob("dash", "ota_job", &record, sizeof(record)) == ESP_OK;
}

static void finish(dash_ota_phase_t phase, const char *error)
{
    record.phase = (uint8_t)phase + 16;
    record.acknowledged = false;
    snprintf(record.error, sizeof(record.error), "%s", error ? error : "");
    save_record();
    state(phase, 0, 0, 0);
    next_check = 0;
    printf("dash: ota result=%d error=%s\n", (int)phase, record.error);
}

static void verifier(void *unused)
{
    (void)unused;
    int64_t start = esp_timer_get_time() / 1000;
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        bool ui_alive = atomic_load(&ui_heartbeat) &&
                        (uint32_t)now - atomic_load(&ui_heartbeat) < 2000;
        bool healthy = now - start >= 10000 && campus_net_storage_ready() &&
                       ui_alive && atomic_load(&hub_ok);
#ifdef DASH_OTA_TEST_FAIL_BOOT
        healthy = false;
#endif
        if (healthy && esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
            atomic_store(&verified, true);
            state(DASH_OTA_SUCCEEDED, 0, 0, 0);
            printf("dash: ota boot confirmed\n");
            vTaskDelete(NULL);
        }
#ifdef DASH_OTA_TEST_FAIL_BOOT
        bool expired = now - start >= 15000; /* explicitly marked verification build */
#else
        bool expired = now - start >= 120000;
#endif
        if (expired) {
            printf("dash: ota boot verification failed; rolling back\n");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void app_ota_init(void)
{
    hex(esp_app_get_description()->app_elf_sha256, 32, elf_sha);
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) hex(mac, 6, device_id);
    esp_ota_img_states_t image_state;
    boot_pending = esp_ota_get_state_partition(esp_ota_get_running_partition(), &image_state) == ESP_OK &&
                   image_state == ESP_OTA_IMG_PENDING_VERIFY;
    atomic_store(&verified, !boot_pending);
    if (boot_pending) {
        state(DASH_OTA_VALIDATING, 0, 0, 0);
        if (xTaskCreate(verifier, "ota_verify", 3072, NULL, 2, NULL) != pdPASS) {
            esp_ota_mark_app_invalid_rollback_and_reboot();
            esp_restart();
        }
    }
}

void app_ota_hub_ok(void) { atomic_store(&hub_ok, true); }
void app_ota_ui_tick(void) { atomic_store(&ui_heartbeat, (uint32_t)(esp_timer_get_time() / 1000)); }
void app_ota_ui_presented(uint8_t number) { atomic_store(&presented, number); }

static const char *string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

static void device_json(char *out, size_t cap)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    snprintf(out, cap, "\"device\":\"%s\",\"layout\":\"" OTA_LAYOUT "\","
             "\"elf_sha256\":\"%s\",\"slot\":\"%s\",\"verified\":%s",
             device_id, elf_sha, running ? running->label : "unknown", atomic_load(&verified) ? "true" : "false");
}

static int report(const char *token, const char *phase, const char *error, char *scratch, int cap)
{
    char dev[240], req[448];
    device_json(dev, sizeof(dev));
    snprintf(req, sizeof(req), "{%s,\"job_id\":\"%s\",\"stage\":\"%s\",\"error\":\"%.47s\"}",
             dev, record.job, phase, error ? error : "");
    int len;
    return app_hub_json(OTA_REPORT_URL, token, req, scratch, cap, &len);
}

static bool countdown(uint32_t total)
{
    for (unsigned n = 3; n; --n) {
        atomic_store(&presented, 0);
        state(DASH_OTA_COUNTDOWN, total, total, (uint8_t)n);
        int64_t until = esp_timer_get_time() / 1000 + 5000;
        while (atomic_load(&presented) != n) {
            if (esp_timer_get_time() / 1000 >= until) return false;
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        printf("dash: ota countdown=%u presented\n", n);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return true;
}

static void download(const char *token, const char *digest, const char *target_elf,
                     uint32_t size, char *buffer, int capacity)
{
    const char *error = "transport";
    esp_ota_handle_t handle = 0;
    bool begun = false;
    esp_http_client_handle_t client = NULL;
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || partition->size < size || partition == esp_ota_get_running_partition()) {
        error = "partition";
        goto done;
    }
    if (report(token, "downloading", "", buffer, capacity) != 200) {
        error = "task_unavailable";
        goto done;
    }
    record.phase = 1;
    if (!save_record()) { error = "storage"; goto done; }
    state(DASH_OTA_DOWNLOADING, 0, size, 0);
    char url[160], auth[96];
    snprintf(url, sizeof(url), DASH_HUB_BASE "/api/device/ota/images/%s.bin", digest);
    esp_http_client_config_t cfg = {.url = url, .cert_pem = hub_pem_start,
        .common_name = DASH_HUB_CN, .timeout_ms = 15000, .buffer_size = 2048,
        .disable_auto_redirect = true};
    client = esp_http_client_init(&cfg);
    if (!client) { error = "memory"; goto done; }
    snprintf(auth, sizeof(auth), "Bearer %s", token);
    esp_http_client_set_header(client, "Authorization", auth);
    volatile char *secret = auth;
    for (size_t i = 0; i < sizeof(auth); ++i) secret[i] = 0;
    if (esp_http_client_open(client, 0) != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(client) != size || esp_http_client_get_status_code(client) != 200 ||
        esp_http_client_is_chunked_response(client)) { error = "http_length"; goto done; }
    int prefix = 0;
    const int header_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    while (prefix < header_size) {
        int n = esp_http_client_read(client, buffer + prefix, header_size - prefix);
        if (n <= 0) goto done;
        prefix += n;
    }
    esp_image_header_t image;
    esp_app_desc_t desc;
    memcpy(&image, buffer, sizeof(image));
    memcpy(&desc, buffer + sizeof(image) + sizeof(esp_image_segment_header_t), sizeof(desc));
    char image_elf[65];
    hex(desc.app_elf_sha256, 32, image_elf);
    if (image.magic != ESP_IMAGE_HEADER_MAGIC || image.chip_id != ESP_CHIP_ID_ESP32S3 ||
        image.spi_size != ESP_IMAGE_FLASH_SIZE_16MB || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(desc.project_name, "esp_dash\0", 9) || memcmp(desc.version, "ota1-", 5) ||
        strcmp(image_elf, target_elf)) { error = "incompatible_image"; goto done; }
    if (esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) { error = "flash_begin"; goto done; }
    begun = true;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        error = "hash_setup";
        goto done;
    }
    uint32_t received = 0;
    int amount = prefix;
    int64_t started = esp_timer_get_time() / 1000, last_ui = 0;
    while (received < size) {
        if (amount <= 0 || (uint32_t)amount > size - received) { error = "truncated"; goto done; }
        if (psa_hash_update(&hash, (const unsigned char *)buffer, (size_t)amount) != PSA_SUCCESS) {
            error = "hash_update";
            goto done;
        }
        if (esp_ota_write(handle, buffer, (size_t)amount) != ESP_OK) { error = "flash_write"; goto done; }
        received += (uint32_t)amount;
        int64_t now = esp_timer_get_time() / 1000;
        if (now - last_ui >= 100 || received == size) {
            state(DASH_OTA_DOWNLOADING, received, size, 0);
            last_ui = now;
        }
        if (now - started > 300000) { error = "download_timeout"; goto done; }
        if (received == size) break;
        unsigned want = size - received;
        if (want > 4096) want = 4096;
        if (want > (unsigned)capacity) want = (unsigned)capacity;
        amount = esp_http_client_read(client, buffer, (int)want);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!esp_http_client_is_complete_data_received(client)) { error = "truncated"; goto done; }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    client = NULL;
    state(DASH_OTA_VERIFYING, size, size, 0);
    uint8_t sum[32];
    char sum_hex[65];
    size_t sum_size = 0;
    if (psa_hash_finish(&hash, sum, sizeof(sum), &sum_size) != PSA_SUCCESS || sum_size != sizeof(sum)) {
        error = "hash_finish";
        goto done;
    }
    hex(sum, 32, sum_hex);
    if (strcmp(sum_hex, digest)) { error = "sha256"; goto done; }
    esp_err_t end = esp_ota_end(handle);
    begun = false;
    if (end != ESP_OK) { error = "image_validation"; goto done; }
    report(token, "countdown", "", buffer, capacity);
    if (!countdown(size)) { error = "display_timeout"; goto done; }
    record.phase = 2;
    if (!save_record()) { error = "storage"; goto done; }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) { error = "boot_selection"; goto done; }
    printf("dash: ota switching to %s\n", partition->label);
    esp_restart();
done:
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    if (begun) esp_ota_abort(handle);
    psa_hash_abort(&hash);
    finish(DASH_OTA_FAILED, error);
}

void app_ota_poll(const char *token, char *scratch, int capacity)
{
    if (!record_loaded) {
        size_t n = sizeof(record);
        bool found = app_nvs_get_blob("dash", "ota_job", &record, &n) == ESP_OK;
        record.job[32] = record.target[64] = record.error[47] = 0;
        if (!found || n != sizeof(record) || record.magic != RECORD_MAGIC || !hex_string(record.job, 32) ||
            !hex_string(record.target, 64)) memset(&record, 0, sizeof(record));
        record_loaded = true;
        resolving_boot = record.phase == 1 || record.phase == 2;
        if (resolving_boot && strcmp(record.target, elf_sha)) {
            const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
            esp_app_desc_t desc;
            char invalid_elf[65] = "";
            if (invalid && esp_ota_get_partition_description(invalid, &desc) == ESP_OK)
                hex(desc.app_elf_sha256, 32, invalid_elf);
            finish(!strcmp(invalid_elf, record.target) ? DASH_OTA_ROLLED_BACK : DASH_OTA_FAILED,
                   !strcmp(invalid_elf, record.target) ? "boot_rollback" : "interrupted");
            resolving_boot = false;
        }
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (resolving_boot && atomic_load(&verified)) {
        finish(DASH_OTA_SUCCEEDED, "");
        resolving_boot = false;
    }
    if (now < next_check || !device_id[0]) return;
    next_check = now + 30000;
    if (hex_string(record.job, 32) && record.phase >= 16 && !record.acknowledged) {
        const char *stage = record.phase == DASH_OTA_SUCCEEDED + 16 ? "succeeded" :
                            record.phase == DASH_OTA_ROLLED_BACK + 16 ? "rolled_back" : "failed";
        int result = report(token, stage, record.error, scratch, capacity);
        if (result == 200 || result == 409) {
            record.acknowledged = true;
            save_record();
        } else { next_check = now + 5000; return; }
    }
    if (boot_pending && !atomic_load(&verified) && hex_string(record.job, 32))
        report(token, "validating", "", scratch, capacity);
    char dev[240], req[256];
    device_json(dev, sizeof(dev));
    snprintf(req, sizeof(req), "{%s}", dev);
    int len = 0;
    if (app_hub_json(OTA_CHECK_URL, token, req, scratch, capacity, &len) != 200) return;
    cJSON *root = cJSON_ParseWithLength(scratch, (size_t)len);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "update");
    if (!cJSON_IsObject(m) || !atomic_load(&verified)) { cJSON_Delete(root); return; }
    const char *job = string(m, "job_id"), *digest = string(m, "sha256"), *elf = string(m, "elf_sha256");
    const cJSON *size = cJSON_GetObjectItemCaseSensitive(m, "size");
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(m, "schema");
    bool valid = hex_string(job, 32) && hex_string(digest, 64) && hex_string(elf, 64) &&
        cJSON_IsNumber(schema) && schema->valuedouble == 1 &&
        !strcmp(string(m, "layout"), OTA_LAYOUT) && !strcmp(string(m, "chip"), "esp32s3") &&
        !strcmp(string(m, "project"), "esp_dash") && !strncmp(string(m, "version"), "ota1-", 5) &&
        cJSON_IsNumber(size) && size->valuedouble == size->valueint && size->valueint >= 320 &&
        size->valueint <= OTA_SLOT_SIZE;
    if (!valid || !strcmp(job, record.job) || !strcmp(elf, elf_sha)) { cJSON_Delete(root); return; }
    char digest_copy[65], elf_copy[65];
    memcpy(digest_copy, digest, 65);
    memcpy(elf_copy, elf, 65);
    uint32_t bytes = (uint32_t)size->valueint;
    memset(&record, 0, sizeof(record));
    record.magic = RECORD_MAGIC;
    memcpy(record.job, job, 33);
    memcpy(record.target, elf, 65);
    cJSON_Delete(root);
    download(token, digest_copy, elf_copy, bytes, scratch, capacity);
}

void app_ota_print_status(const char *prefix)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    dash_state_t *s = app_lock();
    dash_ota_status_t ota = s->ota;
    app_unlock(false);
    printf("%sota layout=" OTA_LAYOUT " slot=%s size=%lu elf=%s verified=%d phase=%d bytes=%lu total=%lu\n",
           prefix, p ? p->label : "unknown", p ? (unsigned long)p->size : 0,
           elf_sha, atomic_load(&verified), (int)ota.phase, (unsigned long)ota.received, (unsigned long)ota.total);
}
