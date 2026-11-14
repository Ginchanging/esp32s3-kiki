/*
 * Extra "@sysu" serial commands (campus.py adds matching subcommands).
 * Secrets arrive hex-encoded so any byte survives the line protocol.
 *
 *   TOKEN <hex>|clear     device token for the hub  (NVS dash/token)
 *   BEMFA <hex>|clear     Bemfa UID for Muse alerts (NVS muse/uid)
 *   MUSE <hex>            inject a local Muse alert (UTF-8 text)
 *   AGENT <hex>           inject an agent-bemfa-notify JSON payload
 *   LANG zh|en            UI language               (NVS dash/lang)
 *   DEMO ok|gh_failed|gh_running|server_down|off
 *   LED <gpio>|off        onboard WS2812 pin        (NVS dash/led_gpio)
 *   DASH                  print dashboard status
 *   MIRROR on|off|full    stream rendered frames to scripts/mirror.py
 *   TOUCH <x> <y> <0|1>   touch from the mirror window
 *   CAT act <0..10> | react <1..9> | look <name> | night on|off | time <+hours> | pomo | auto
 *   MI <dev> on|off|bri <1..100>   Mi Home control through the hub (e.g. MI lamp on)
 *   KNOB cw|ccw [n] | hold cw|ccw [n] | press | long | release | rev
 *                         act like the EC11 knob (rev flips its direction, NVS dash/knob_rev)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "campus_net.h"
#include "dash_ui.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define P CAMPUS_USB_PREFIX

static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Hex -> bytes; rejects control characters. Returns length or -1. */
static int unhex(const char *in, char *out, size_t cap)
{
    size_t n = strlen(in);
    if (n == 0 || n % 2 || n / 2 >= cap) {
        return -1;
    }
    for (size_t i = 0; i < n; i += 2) {
        int hi = hexval(in[i]), lo = hexval(in[i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        unsigned char b = (unsigned char)(hi * 16 + lo);
        if (b < 0x20 || b == 0x7F) {
            return -1;
        }
        out[i / 2] = (char)b;
    }
    out[n / 2] = '\0';
    return (int)(n / 2);
}

static void wipe(void *p, size_t n)
{
    volatile unsigned char *b = p;
    while (n--) {
        *b++ = 0;
    }
}

static bool cmd_secret(const char *arg, const char *ns, const char *key, const char *label,
                       size_t min_len)
{
    esp_err_t err;
    if (!strcmp(arg, "clear")) {
        err = app_nvs_erase(ns, key);
        printf(P "%s cleared=%d\n", label, err == ESP_OK);
        return err == ESP_OK;
    }
    char value[128];
    int len = unhex(arg, value, sizeof(value));
    if (len < (int)min_len) {
        printf(P "%s error=invalid_input\n", label);
        wipe(value, sizeof(value));
        return false;
    }
    err = app_nvs_set_str(ns, key, value);
    wipe(value, sizeof(value));
    printf(P "%s saved=%d err=%s\n", label, err == ESP_OK, esp_err_to_name(err));
    return err == ESP_OK;
}

static const char *fresh_name(dash_freshness_t f)
{
    switch (f) {
    case DASH_FRESH_OK:
        return "live";
    case DASH_FRESH_STALE:
        return "stale";
    case DASH_FRESH_OFFLINE:
        return "offline";
    default:
        return "none";
    }
}

bool app_cmd_handle(char *line)
{
    if (!strncmp(line, "TOKEN ", 6)) {
        if (cmd_secret(line + 6, "dash", "token", "token", 16)) {
            app_notify_token_changed();
        }
        wipe(line, strlen(line));
        return true;
    }
    if (!strncmp(line, "BEMFA ", 6)) {
        if (cmd_secret(line + 6, "muse", "uid", "bemfa", 8)) {
            app_notify_bemfa_changed();
        }
        wipe(line, strlen(line));
        return true;
    }
    if (!strncmp(line, "MUSE ", 5)) {
        char text[DASH_MUSE_TEXT];
        if (unhex(line + 5, text, sizeof(text)) <= 0) {
            printf(P "muse error=invalid_input\n");
        } else {
            app_muse_arrived(text, time(NULL));
            printf(P "muse injected=1\n");
        }
        return true;
    }
    if (!strncmp(line, "AGENT ", 6)) {
        char json[DASH_MUSE_TEXT + 40];
        static dash_muse_msg_t m;
        int n = unhex(line + 6, json, sizeof(json));
        if (n <= 0 || !dash_agent_parse(json, (size_t)n, &m)) {
            printf(P "agent error=invalid_input\n");
        } else {
            app_agent_arrived(&m, time(NULL));
            printf(P "agent injected=1\n");
        }
        return true;
    }
    if (!strncmp(line, "LANG ", 5)) {
        const char *v = line + 5;
        if (strcmp(v, "zh") && strcmp(v, "en")) {
            printf(P "lang error=invalid_input\n");
            return true;
        }
        dash_lang_t lang = !strcmp(v, "en") ? DASH_LANG_EN : DASH_LANG_ZH;
        app_request_lang(lang); /* the UI task applies and saves it */
        printf(P "lang set=%s\n", v);
        return true;
    }
    if (!strncmp(line, "DEMO ", 5)) {
        const char *v = line + 5;
        if (!strcmp(v, "off")) {
            app_demo_off();
            printf(P "demo active=0\n");
        } else if (app_demo_load(v)) {
            printf(P "demo active=1 fixture=%s\n", v);
        } else {
            printf(P "demo error=unknown_fixture\n");
        }
        return true;
    }
    if (!strncmp(line, "LED ", 4)) {
        const char *v = line + 4;
        uint32_t gpio = !strcmp(v, "off") ? 0 : (uint32_t)atoi(v);
        if (strcmp(v, "off") && (gpio == 0 || gpio > 48)) {
            printf(P "led error=invalid_input\n");
            return true;
        }
        esp_err_t err = app_nvs_set_u32("dash", "led_gpio", gpio);
        app_notify_led_changed();
        printf(P "led gpio=%u saved=%d\n", (unsigned)gpio, err == ESP_OK);
        return true;
    }
    if (!strncmp(line, "CAT ", 4)) {
        app_cat_request(line + 4);
        printf(P "cat request=%s\n", line + 4);
        return true;
    }
    if (!strncmp(line, "MI ", 3)) {
        char dev[16], op[8];
        int bri = -1;
        int got = sscanf(line + 3, "%15s %7s %d", dev, op, &bri);
        int on = got >= 2 && !strcmp(op, "on") ? 1 : got >= 2 && !strcmp(op, "off") ? 0 : -1;
        bool is_bri = got == 3 && !strcmp(op, "bri") && bri >= 1 && bri <= 100;
        if (on < 0 && !is_bri) {
            printf(P "mi error=invalid_input\n");
            return true;
        }
        app_request_mi(dev, on, is_bri ? bri : -1);
        printf(P "mi request=%s %s\n", dev, is_bri ? "bri" : op);
        return true;
    }
    if (!strncmp(line, "KNOB ", 5)) {
        char a[8] = "", b[8] = "";
        int n = 1;
        int got = sscanf(line + 5, "%7s %7s %d", a, b, &n);
        bool hold = !strcmp(a, "hold");
        const char *dir = hold ? b : a;
        if (!hold && got >= 2) {
            n = atoi(b); /* "cw 3" */
        }
        n = n < 1 ? 1 : n > 20 ? 20 : n;
        if (!strcmp(dir, "cw") || !strcmp(dir, "ccw")) {
            app_knob_push(hold ? DASH_KNOB_HOLD_TURN : DASH_KNOB_TURN, !strcmp(dir, "cw") ? n : -n);
        } else if (!strcmp(a, "press")) {
            app_knob_push(DASH_KNOB_CLICK, 0);
        } else if (!strcmp(a, "long")) {
            app_knob_push(DASH_KNOB_LONG, 0);
        } else if (!strcmp(a, "release")) {
            app_knob_push(DASH_KNOB_RELEASE, 0);
        } else if (!strcmp(a, "rev")) {
            uint32_t rev = 0;
            app_nvs_get_u32("dash", "knob_rev", &rev);
            rev = !rev;
            esp_err_t err = app_nvs_set_u32("dash", "knob_rev", rev);
            app_knob_set_reverse(rev != 0);
            printf(P "knob reversed=%u saved=%d\n", (unsigned)rev, err == ESP_OK);
            return true;
        } else {
            printf(P "knob error=invalid_input\n");
            return true;
        }
        printf(P "knob event=%s\n", line + 5);
        return true;
    }
    if (!strncmp(line, "TOUCH ", 6)) {
        int x, y, p;
        if (sscanf(line + 6, "%d %d %d", &x, &y, &p) == 3) {
            app_touch_set(x, y, p != 0);
        }
        return true;
    }
    if (!strncmp(line, "MIRROR ", 7)) {
        const char *v = line + 7;
        bool on = !strcmp(v, "on") || !strcmp(v, "full");
        if (!on && strcmp(v, "off")) {
            printf(P "mirror error=invalid_input\n");
            return true;
        }
        app_mirror_request(on);
        printf(P "mirror active=%d\n", on);
        return true;
    }
    if (!strcmp(line, "DASH")) {
        char tmp[80];
        bool token = app_nvs_get_str("dash", "token", tmp, sizeof(tmp)) == ESP_OK && tmp[0];
        bool uid = app_nvs_get_str("muse", "uid", tmp, sizeof(tmp)) == ESP_OK && tmp[0];
        wipe(tmp, sizeof(tmp));
        uint32_t led = LED_DEFAULT_GPIO;
        app_nvs_get_u32("dash", "led_gpio", &led);
        dash_state_t *st = app_lock();
        int64_t now = time(NULL);
        printf(P "dash token=%d bemfa=%d mqtt=%d demo=%d fresh=%s failures=%d "
                 "last_ok_ago=%lld runs=%d servers=%d muse=%d mi=%d mi_ok=%d led=%u heap=%u min_heap=%u "
                 "psram=%u uptime=%lld\n",
               token, uid, st->mqtt_connected, app_demo_active(),
               fresh_name(dash_freshness(st, now)), st->fetch_failures,
               app_fetch_ok_age_ms() < 0 ? -1LL : (long long)(app_fetch_ok_age_ms() / 1000),
               st->run_count,
               st->server_count, st->muse_count, st->have_mi ? st->mi_count : -1, st->mi_ok,
               (unsigned)led,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (long long)(esp_timer_get_time() / 1000000));
        app_unlock(false);
        return true;
    }
    return false;
}
