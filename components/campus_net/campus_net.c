#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_crt_bundle.h"
#include "esp_eap_client.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "campus_net.h"

#define SSID "SYSU-SECURE-2.4G"
#define RADIUS_HOST "sam.sysu.edu.cn"
#define HTTPS_URL "https://www.baidu.com/robots.txt"
#define NVS_PARTITION CAMPUS_NVS_PARTITION
#define MAX_CREDENTIAL 127
#define USB_PREFIX CAMPUS_USB_PREFIX
#define MIN_EPOCH INT64_C(1704067200)
#define MAX_EPOCH INT64_C(4102444800)

#define WIFI_STOPPED BIT0
#define WIFI_ASSOCIATED BIT1
#define WIFI_GOT_IP BIT2
#define WIFI_DISCONNECTED BIT3
#define LINK_EVENTS (WIFI_ASSOCIATED | WIFI_GOT_IP | WIFI_DISCONNECTED)

typedef enum {
    CHECK_IDLE,
    CHECK_SCAN_ERROR,
    CHECK_NO_AP,
    CHECK_AP_SECURITY,
    CHECK_NEEDS_SETUP,
    CHECK_CLOCK,
    CHECK_AUTH,
    CHECK_DHCP,
    CHECK_HTTPS,
    CHECK_ONLINE,
} check_stage_t;

typedef struct {
    uint32_t id;
    char netid[MAX_CREDENTIAL + 1];
    char password[MAX_CREDENTIAL + 1];
} profile_t;

typedef struct {
    check_stage_t stage;
    esp_err_t error;
    esp_ip4_addr_t ip;
    int reason;
    int rssi;
    int channel;
    int http_code;
    int tls_error;
    int tls_flags;
} check_t;

typedef struct {
    uint32_t boot_id;
    uint32_t profile_id;
    check_t result;
    esp_reset_reason_t reset;
} cold_boot_t;

static EventGroupHandle_t wifi_events;
static esp_netif_t *station;
static nvs_handle_t profiles;
static nvs_handle_t diagnostics;
static bool storage_ready;
static bool station_started;
static bool have_profile;
static bool retry_after_drop;
static bool usb_seen;
static int disconnect_reason;
static uint32_t boot_id;
static esp_reset_reason_t reset_reason;
static int64_t saved_clock;
static profile_t profile;
static check_t current;

/* --- additions for the dashboard firmware --- */
static campus_net_hooks_t hooks;
static volatile bool connecting;
static volatile bool sntp_started;
static volatile bool sntp_synced;

static void notify_status(bool online, uint32_t ip, int rssi)
{
    if (!hooks.on_status) {
        return;
    }
    campus_net_status_t status = {
        .have_profile = have_profile,
        .connecting = connecting,
        .online = online,
        .ip = online ? ip : 0,
        .rssi = rssi,
        .clock_valid = time(NULL) >= MIN_EPOCH,
    };
    hooks.on_status(&status);
}

static void sntp_synced_cb(struct timeval *tv)
{
    sntp_synced = true;
}

static void start_sntp_once(void)
{
    if (sntp_started) {
        return;
    }
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.sync_cb = sntp_synced_cb;
    if (esp_netif_sntp_init(&config) == ESP_OK) {
        sntp_started = true;
    }
}

static void wipe(void *buffer, size_t length)
{
    volatile unsigned char *bytes = buffer;
    while (length--) {
        *bytes++ = 0;
    }
}

static const char *stage_name(check_stage_t stage)
{
    switch (stage) {
    case CHECK_IDLE:        return "idle";
    case CHECK_SCAN_ERROR:  return "scan_error";
    case CHECK_NO_AP:       return "no_ap";
    case CHECK_AP_SECURITY: return "ap_security";
    case CHECK_NEEDS_SETUP: return "needs_setup";
    case CHECK_CLOCK:       return "clock";
    case CHECK_AUTH:        return "auth";
    case CHECK_DHCP:        return "dhcp";
    case CHECK_HTTPS:       return "https";
    case CHECK_ONLINE:      return "online";
    }
    return "unknown";
}

static void print_progress(const char *stage)
{
    printf(USB_PREFIX "progress stage=%s\n", stage);
}

static const char *power_on_reason(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power_on";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return NULL;
    }
}

static void print_result(const char *label, uint32_t id, const check_t *result)
{
    printf(USB_PREFIX "%s boot=%08" PRIx32 " stage=%s ssid=%s channel=%d rssi=%d "
           "ip=" IPSTR " http=%d reason=%d err=%s tls=%d flags=0x%x\n",
           label, id, stage_name(result->stage), SSID, result->channel, result->rssi,
           IP2STR(&result->ip), result->http_code, result->reason,
           esp_err_to_name(result->error), result->tls_error, (unsigned)result->tls_flags);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_STOP:
            xEventGroupSetBits(wifi_events, WIFI_STOPPED);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            xEventGroupSetBits(wifi_events, WIFI_ASSOCIATED);
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            disconnect_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
            xEventGroupClearBits(wifi_events, WIFI_ASSOCIATED | WIFI_GOT_IP);
            xEventGroupSetBits(wifi_events, WIFI_DISCONNECTED);
            notify_status(false, 0, 0);
            break;
        default:
            break;
        }
    } else if (id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_GOT_IP);
        const ip_event_got_ip_t *got = data;
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        notify_status(true, got->ip_info.ip.addr, rssi);
    }
}

static EventBits_t wait_for_wifi(EventBits_t bits, int timeout_ms)
{
    // Sample USB while waiting so a PC-connected attempt cannot replace cold-boot evidence.
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    EventBits_t events;
    do {
        usb_seen |= usb_serial_jtag_is_connected();
        events = xEventGroupWaitBits(wifi_events, bits, pdFALSE, pdFALSE, pdMS_TO_TICKS(250));
    } while (!(events & bits) && esp_timer_get_time() < deadline);
    return events;
}

static esp_err_t start_station(void)
{
    if (station_started) {
        xEventGroupClearBits(wifi_events, WIFI_STOPPED);
        esp_err_t error = esp_wifi_stop();
        if (error != ESP_OK) {
            return error;
        }
        station_started = false;
        if (!(wait_for_wifi(WIFI_STOPPED, 2000) & WIFI_STOPPED)) {
            return ESP_ERR_TIMEOUT;
        }
    }

    esp_err_t error = esp_wifi_sta_enterprise_disable();
    if (error != ESP_OK) {
        return error;
    }
    xEventGroupClearBits(wifi_events, LINK_EVENTS);
    error = esp_wifi_start();
    station_started = error == ESP_OK;
    return error;
}

static check_t scan_campus(void)
{
    check_t result = {.stage = CHECK_SCAN_ERROR};
    wifi_scan_config_t scan = {.ssid = (uint8_t *)SSID};
    result.error = esp_wifi_scan_start(&scan, true);
    if (result.error != ESP_OK) {
        return result;
    }

    uint16_t count = 0;
    result.error = esp_wifi_scan_get_ap_num(&count);
    if (result.error == ESP_OK) {
        result.stage = count ? CHECK_AP_SECURITY : CHECK_NO_AP;
        for (uint16_t index = 0; index < count; ++index) {
            wifi_ap_record_t ap;
            result.error = esp_wifi_scan_get_ap_record(&ap);
            if (result.error != ESP_OK) {
                result.stage = CHECK_SCAN_ERROR;
                break;
            }
            result.channel = ap.primary;
            result.rssi = ap.rssi;
            if (ap.authmode == WIFI_AUTH_WPA2_ENTERPRISE ||
                ap.authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE) {
                result.stage = CHECK_NEEDS_SETUP;
                break;
            }
        }
    }
    esp_wifi_clear_ap_list();
    return result;
}

static bool valid_epoch(int64_t epoch)
{
    return epoch >= MIN_EPOCH && epoch < MAX_EPOCH;
}

static bool set_clock(int64_t epoch)
{
    if (!valid_epoch(epoch)) {
        return false;
    }
    struct timeval value = {.tv_sec = (time_t)epoch};
    return settimeofday(&value, NULL) == 0;
}

static esp_err_t remember_clock(void)
{
    int64_t now = (int64_t)time(NULL);
    if (!storage_ready || !valid_epoch(now)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (now <= saved_clock) {
        return ESP_OK;
    }
    esp_err_t error = nvs_set_i64(diagnostics, "clock", now);
    if (error == ESP_OK) {
        error = nvs_commit(diagnostics);
    }
    if (error == ESP_OK) {
        saved_clock = now;
    }
    return error;
}

static esp_err_t configure_peap(const profile_t *credentials)
{
    wifi_config_t config = {
        .sta = {
            .ssid = SSID,
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold = {.rssi = -127, .authmode = WIFI_AUTH_WPA2_ENTERPRISE},
            .pmf_cfg = {.capable = true},
        },
    };
    esp_err_t error = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (error == ESP_OK) {
        error = esp_eap_client_use_default_cert_bundle(true);
    }
    if (error == ESP_OK) {
        error = esp_eap_client_set_domain_name(RADIUS_HOST);
    }
    if (error == ESP_OK) {
        error = esp_eap_client_set_disable_time_check(false);
    }
    if (error == ESP_OK) {
        error = esp_eap_client_set_eap_methods(ESP_EAP_TYPE_PEAP);
    }
    if (error == ESP_OK) {
        const unsigned char *id = (const unsigned char *)credentials->netid;
        error = esp_eap_client_set_identity(id, (int)strlen(credentials->netid));
        if (error == ESP_OK) {
            error = esp_eap_client_set_username(id, (int)strlen(credentials->netid));
        }
    }
    if (error == ESP_OK) {
        error = esp_eap_client_set_password((const unsigned char *)credentials->password,
                                            (int)strlen(credentials->password));
    }
    if (error == ESP_OK) {
        error = esp_wifi_sta_enterprise_enable();
    }
    return error;
}

static time_t parse_http_date(const char *date)
{
    static const char *const months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char month[4] = "";
    struct tm parsed = {0};
    int year;
    if (sscanf(date, "%*3s %d %3s %d %d:%d:%d", &parsed.tm_mday, month, &year,
               &parsed.tm_hour, &parsed.tm_min, &parsed.tm_sec) != 6 ||
        year < 2024 || year >= 2100 || parsed.tm_mday < 1 || parsed.tm_mday > 31 ||
        parsed.tm_hour < 0 || parsed.tm_hour > 23 || parsed.tm_min < 0 || parsed.tm_min > 59 ||
        parsed.tm_sec < 0 || parsed.tm_sec > 60) {
        return 0;
    }
    parsed.tm_year = year - 1900;
    for (int index = 0; index < 12; ++index) {
        if (strcmp(month, months[index]) == 0) {
            parsed.tm_mon = index;
            return mktime(&parsed);
        }
    }
    return 0;
}

static esp_err_t https_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(event->header_key, "Date") == 0) {
        *(time_t *)event->user_data = parse_http_date(event->header_value);
    }
    return ESP_OK;
}

static void check_https(check_t *result)
{
    print_progress("https");
    time_t date = 0;
    esp_http_client_config_t config = {
        .url = HTTPS_URL,
        .method = HTTP_METHOD_HEAD,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .timeout_ms = 8000,
        .event_handler = https_header,
        .user_data = &date,
    };
    result->stage = CHECK_HTTPS;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        result->error = ESP_ERR_NO_MEM;
        return;
    }
    result->error = esp_http_client_perform(client);
    result->http_code = esp_http_client_get_status_code(client);
    if (result->error == ESP_OK && result->http_code >= 200 && result->http_code < 300) {
        result->stage = CHECK_ONLINE;
        if (date > time(NULL)) {
            set_clock(date);
        }
        esp_err_t saved = remember_clock();
        if (saved != ESP_OK) {
            printf(USB_PREFIX "storage err=%s\n", esp_err_to_name(saved));
        }
    } else {
        int socket_error = esp_http_client_get_errno(client);
        esp_err_t transport_error = esp_http_client_get_and_clear_last_tls_error(
            client, &result->tls_error, &result->tls_flags);
        if (result->error != ESP_OK) {
            printf(USB_PREFIX "https_error transport=0x%x errno=%d\n",
                   (unsigned)transport_error, socket_error);
        }
    }
    esp_http_client_cleanup(client);
}

static check_t run_check_inner(const profile_t *credentials);

static check_t run_check(const profile_t *credentials)
{
    connecting = true;
    notify_status(false, 0, 0);
    check_t result = run_check_inner(credentials);
    connecting = false;
    bool online = result.ip.addr != 0 && (xEventGroupGetBits(wifi_events) & WIFI_GOT_IP);
    if (online) {
        start_sntp_once();
    }
    notify_status(online, result.ip.addr, result.rssi);
    return result;
}

static check_t run_check_inner(const profile_t *credentials)
{
    print_progress("scan");
    esp_err_t error = start_station();
    if (error != ESP_OK) {
        return (check_t){.stage = CHECK_SCAN_ERROR, .error = error};
    }
    check_t result = scan_campus();
    if (result.stage != CHECK_NEEDS_SETUP || !credentials) {
        return result;
    }
    if (!valid_epoch(time(NULL))) {
        result.stage = CHECK_CLOCK;
        return result;
    }

    result.stage = CHECK_AUTH;
    print_progress("auth");
    result.error = configure_peap(credentials);
    if (result.error == ESP_OK) {
        result.error = esp_wifi_connect();
    }
    if (result.error != ESP_OK) {
        esp_wifi_sta_enterprise_disable();
        return result;
    }

    EventBits_t events = wait_for_wifi(WIFI_ASSOCIATED | WIFI_DISCONNECTED, 20000);
    if ((events & WIFI_ASSOCIATED) && !(events & WIFI_DISCONNECTED)) {
        result.stage = CHECK_DHCP;
        print_progress("dhcp");
        events = wait_for_wifi(WIFI_GOT_IP | WIFI_DISCONNECTED, 15000);
    }
    if ((events & WIFI_GOT_IP) && !(events & WIFI_DISCONNECTED)) {
        esp_netif_ip_info_t ip_info;
        result.error = esp_netif_get_ip_info(station, &ip_info);
        if (result.error == ESP_OK && ip_info.ip.addr != 0) {
            result.ip = ip_info.ip;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                result.channel = ap.primary;
                result.rssi = ap.rssi;
            }
            check_https(&result);
            return result;
        }
    }
    if (events & WIFI_DISCONNECTED) {
        result.reason = disconnect_reason;
    } else if (result.error == ESP_OK) {
        result.error = ESP_ERR_TIMEOUT;
    }
    esp_wifi_disconnect();
    esp_wifi_sta_enterprise_disable();
    return result;
}

static void load_storage(void)
{
    esp_err_t error = nvs_flash_init_partition(NVS_PARTITION);
    if (error == ESP_OK) {
        error = nvs_open_from_partition(NVS_PARTITION, "profile", NVS_READWRITE_PURGE, &profiles);
    }
    if (error == ESP_OK) {
        error = nvs_open_from_partition(NVS_PARTITION, "diagnostics", NVS_READWRITE, &diagnostics);
    }
    if (error != ESP_OK) {
        printf(USB_PREFIX "storage err=%s\n", esp_err_to_name(error));
        return;
    }
    storage_ready = true;
    size_t length = sizeof(profile);
    if (nvs_get_blob(profiles, "campus", &profile, &length) == ESP_OK &&
        length == sizeof(profile) && profile.id != 0 && profile.netid[0] &&
        profile.password[0] && profile.netid[MAX_CREDENTIAL] == 0 &&
        profile.password[MAX_CREDENTIAL] == 0) {
        have_profile = true;
    } else {
        wipe(&profile, sizeof(profile));
    }
    if (nvs_get_i64(diagnostics, "clock", &saved_clock) == ESP_OK) {
        set_clock(saved_clock);
    }
}

static void save_cold_boot(void)
{
    if (!storage_ready || !have_profile || !power_on_reason(reset_reason) ||
        usb_seen || usb_serial_jtag_is_connected()) {
        return;
    }
    cold_boot_t record = {
        .boot_id = boot_id, .profile_id = profile.id, .result = current, .reset = reset_reason,
    };
    esp_err_t error = nvs_set_blob(diagnostics, "cold_boot", &record, sizeof(record));
    if (error == ESP_OK) {
        error = nvs_commit(diagnostics);
    }
    if (error != ESP_OK) {
        printf(USB_PREFIX "storage err=%s\n", esp_err_to_name(error));
    }
}

static void print_status(void)
{
    printf(USB_PREFIX "status profile=%d storage=%d reset=%d usb=%d\n",
           have_profile, storage_ready, reset_reason, usb_serial_jtag_is_connected());
    print_result("current", boot_id, &current);
    // Records made by the first firmware always came from ESP_RST_POWERON.
    cold_boot_t record = {.reset = ESP_RST_POWERON};
    size_t length = sizeof(record);
    if (storage_ready && have_profile &&
        nvs_get_blob(diagnostics, "cold_boot", &record, &length) == ESP_OK &&
        (length == sizeof(record) || length == offsetof(cold_boot_t, reset)) &&
        record.profile_id == profile.id && power_on_reason(record.reset)) {
        print_result("cold_boot", record.boot_id, &record.result);
        printf(USB_PREFIX "cold_power reset=%s\n", power_on_reason(record.reset));
    } else {
        printf(USB_PREFIX "cold_boot none\n");
    }
}

static int hex_value(char byte)
{
    if (byte >= '0' && byte <= '9') {
        return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
        return byte - 'a' + 10;
    }
    return -1;
}

static bool decode_credential(const char *encoded, char *plain)
{
    if (!encoded) {
        return false;
    }
    size_t length = strlen(encoded);
    if (length == 0 || length % 2 || length > MAX_CREDENTIAL * 2) {
        return false;
    }
    for (size_t index = 0; index < length; index += 2) {
        int high = hex_value(encoded[index]);
        int low = hex_value(encoded[index + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        int byte = high * 16 + low;
        if (byte < 32 || byte == 127) {
            return false;
        }
        plain[index / 2] = (char)byte;
    }
    plain[length / 2] = 0;
    return true;
}

static bool read_clock(const char *value)
{
    if (!value || !*value) {
        return false;
    }
    char *end = NULL;
    int64_t epoch = strtoll(value, &end, 10);
    return *end == '\0' && set_clock(epoch);
}

static void run_setup(char *arguments)
{
    profile_t candidate = {0};
    char *next = NULL;
    size_t length = strlen(arguments);
    char *epoch = strtok_r(arguments, " ", &next);
    char *netid = strtok_r(NULL, " ", &next);
    char *password = strtok_r(NULL, " ", &next);
    bool valid = decode_credential(netid, candidate.netid) &&
                 decode_credential(password, candidate.password) &&
                 strtok_r(NULL, " ", &next) == NULL && read_clock(epoch);
    wipe(arguments, length);
    if (!storage_ready || !valid) {
        printf(USB_PREFIX "setup error=%s\n", storage_ready ? "invalid_input" : "storage");
    } else {
        // Prove the new credentials before replacing an existing working profile.
        do {
            candidate.id = esp_random();
        } while (!candidate.id || candidate.id == profile.id);
        current = run_check(&candidate);
        print_result("current", boot_id, &current);
        if (current.ip.addr != 0) {
            esp_err_t error = remember_clock();
            if (error == ESP_OK) {
                error = nvs_set_blob(profiles, "campus", &candidate, sizeof(candidate));
            }
            if (error == ESP_OK) {
                error = nvs_commit(profiles);
            }
            if (error == ESP_OK) {
                wipe(&profile, sizeof(profile));
                profile = candidate;
                have_profile = true;
                retry_after_drop = true;
                printf(USB_PREFIX "setup saved=1\n");
            } else {
                printf(USB_PREFIX "setup error=%s\n", esp_err_to_name(error));
            }
        } else {
            printf(USB_PREFIX "setup saved=0\n");
        }
    }
    wipe(&candidate, sizeof(candidate));
}

static void clear_profile(void)
{
    esp_err_t error = storage_ready ? nvs_erase_all(profiles) : ESP_ERR_INVALID_STATE;
    if (error == ESP_OK) {
        error = nvs_commit(profiles);
    }
    if (error == ESP_OK) {
        wipe(&profile, sizeof(profile));
        have_profile = false;
        retry_after_drop = false;
        current = (check_t){.stage = CHECK_IDLE, .error = start_station()};
    }
    printf(USB_PREFIX "clear err=%s\n", esp_err_to_name(error));
}

static void handle_command(char *line)
{
    clearerr(stdout);
    if (strcmp(line, "HELLO") == 0) {
        printf(USB_PREFIX "hello v=1\n");
        return;
    }
    if (strcmp(line, "STATUS") == 0) {
        print_status();
    } else if (strcmp(line, "SCAN") == 0) {
        print_progress("scan");
        check_t scan = scan_campus();
        print_result("scan", boot_id, &scan);
    } else if (strncmp(line, "SETUP ", 6) == 0) {
        retry_after_drop = false;
        run_setup(line + 6);
    } else if (strncmp(line, "CLOCK ", 6) == 0) {
        if (read_clock(line + 6)) {
            printf(USB_PREFIX "clock ok=1\n");
            if (have_profile) {
                current = run_check(&profile);
                retry_after_drop = current.ip.addr != 0;
                print_result("current", boot_id, &current);
            }
        } else {
            printf(USB_PREFIX "clock ok=0\n");
        }
    } else if (strcmp(line, "CLEAR") == 0) {
        clear_profile();
    } else if (!(hooks.on_command && hooks.on_command(line))) {
        printf(USB_PREFIX "command error=unknown\n");
    }
    printf(USB_PREFIX "done\n");
}

static void campus_task(void *arg)
{
    // TX buffer enlarged from 1024 for the dashboard's live mirror frames.
    usb_serial_jtag_driver_config_t serial = {.tx_buffer_size = 4096, .rx_buffer_size = 1024};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&serial));
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);
    esp_log_level_set("wpa", ESP_LOG_WARN);
    setenv("TZ", "UTC0", 1);
    tzset();
    reset_reason = esp_reset_reason();

    // Give the PC time to enumerate the device after power-on.
    vTaskDelay(pdMS_TO_TICKS(100));
    for (int sample = 0; sample < 20; ++sample) {
        usb_seen |= usb_serial_jtag_is_connected();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    usb_seen |= usb_serial_jtag_is_connected();

    load_storage();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    station = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(station ? ESP_OK : ESP_ERR_NO_MEM);
    wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_country_code("CN", false));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));

    boot_id = esp_random();
    current = run_check(have_profile ? &profile : NULL);
    if (!have_profile) {
        notify_status(false, 0, 0);
    }
    print_result("current", boot_id, &current);
    save_cold_boot();
    retry_after_drop = have_profile && current.ip.addr != 0;

    char line[2 * MAX_CREDENTIAL * 2 + 32] = {0};
    size_t length = 0;
    bool overflow = false;
    int64_t retry_at = 0;
    int backoff_s = 15;
    for (;;) {
        usb_seen |= usb_serial_jtag_is_connected();
        unsigned char byte;
        int received = usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(100));
        if (received == 1) {
            if (byte == '\n') {
                if (!overflow && length) {
                    line[length] = '\0';
                    handle_command(line);
                } else if (overflow) {
                    printf(USB_PREFIX "command error=too_long\n" USB_PREFIX "done\n");
                }
                wipe(line, sizeof(line));
                length = 0;
                overflow = false;
                retry_at = 0;
            } else if (byte != '\r') {
                if (length < sizeof(line) - 1 && byte != 0) {
                    line[length++] = (char)byte;
                } else {
                    overflow = true;
                }
            }
        }

        if (sntp_synced) {
            sntp_synced = false;
            esp_err_t saved = remember_clock();
            if (saved != ESP_OK) {
                printf(USB_PREFIX "storage err=%s\n", esp_err_to_name(saved));
            }
            notify_status(campus_net_online(), current.ip.addr, current.rssi);
        }

        // Dashboard change: keep retrying whenever a profile exists and the link is down.
        // Backoff 15 s -> 10 min; auth rejections (wrong password / device limit) wait 30 min
        // so a bad password never hammers the RADIUS server.
        bool offline = !(xEventGroupGetBits(wifi_events) & WIFI_GOT_IP);
        if (have_profile && offline && (retry_after_drop || current.stage != CHECK_IDLE)) {
            int64_t now = esp_timer_get_time();
            if (!retry_at) {
                retry_at = now + (int64_t)backoff_s * 1000000;
            } else if (now >= retry_at) {
                current = run_check(&profile);
                print_result("current", boot_id, &current);
                save_cold_boot();
                bool ok = current.ip.addr != 0;
                retry_after_drop = retry_after_drop || ok;
                if (ok) {
                    backoff_s = 15;
                } else if (current.stage == CHECK_AUTH && current.reason == 23) {
                    backoff_s = 1800;
                } else {
                    backoff_s = backoff_s * 2 > 600 ? 600 : backoff_s * 2;
                }
                retry_at = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------ public API */

void campus_net_start(const campus_net_hooks_t *h)
{
    if (h) {
        hooks = *h;
    }
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(wifi_events ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreatePinnedToCore(campus_task, "campus", 8192, NULL, 5, NULL, 0);
}

bool campus_net_storage_ready(void)
{
    return storage_ready;
}

bool campus_net_online(void)
{
    return wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_GOT_IP);
}

bool campus_net_wait_online(uint32_t timeout_ms)
{
    if (!wifi_events) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return false;
    }
    return xEventGroupWaitBits(wifi_events, WIFI_GOT_IP, pdFALSE, pdFALSE,
                               pdMS_TO_TICKS(timeout_ms)) & WIFI_GOT_IP;
}
