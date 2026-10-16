/*
 * net.h - WiFi for the web HMI: own access point always, home WiFi when
 * configured and in range.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Brings up WiFi in AP+STA mode and mDNS. Needs nvs_flash_init() first.
 * Never blocks waiting for the home network. */
esp_err_t net_start(void);

typedef struct {
    char     ap_ssid[33];
    int      ap_clients;
    int      ap_channel;
    char     sta_ssid[33];     /* configured home network, "" if none */
    bool     sta_connected;
    char     sta_ip[16];
    int      sta_rssi;
    uint32_t sta_disconnects;  /* since boot */
} net_status_t;

void      net_get_status(net_status_t *out);

/* Persist settings. The caller reboots to apply them. */
esp_err_t net_save_sta(const char *ssid, const char *pass);
esp_err_t net_save_ap_pass(const char *pass);   /* 8..63 chars */
