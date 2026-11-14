/*
 * Campus Wi-Fi (SYSU-SECURE-2.4G, WPA2-Enterprise PEAP) + the "@sysu" USB
 * serial command protocol used by campus.py. Extracted from the original
 * campus.c; NVS layout (partition "campus_nvs", namespaces "profile" and
 * "diagnostics") is unchanged so saved NetID credentials survive reflashing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMPUS_NVS_PARTITION "campus_nvs"
#define CAMPUS_USB_PREFIX "@sysu "

typedef struct {
    bool have_profile; /* NetID saved */
    bool connecting;   /* a connection attempt is running */
    bool online;       /* has an IPv4 address */
    uint32_t ip;       /* network byte order, 0 if none */
    int rssi;
    bool clock_valid; /* system time looks like a real date */
} campus_net_status_t;

typedef struct {
    /* Called from the campus task and the Wi-Fi event task. Keep it short. */
    void (*on_status)(const campus_net_status_t *status);
    /*
     * Extra serial commands. Return true if handled (print "@sysu ..." lines
     * yourself); the campus code prints the final "@sysu done".
     */
    bool (*on_command)(char *line);
} campus_net_hooks_t;

/* Starts the USB serial driver and the campus task. Call once from app_main. */
void campus_net_start(const campus_net_hooks_t *hooks);

/* True once NVS partition "campus_nvs" is initialised (other modules store data there). */
bool campus_net_storage_ready(void);

bool campus_net_online(void);

/* Blocks until online or timeout. Returns true if online. */
bool campus_net_wait_online(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
