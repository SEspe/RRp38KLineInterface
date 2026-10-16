#pragma once
#include "esp_err.h"

/* Status page, WiFi settings, OTA upload. Read-only towards the K-line:
 * nothing here can arm or transmit (PLAN.md Track H, decision 25). */
esp_err_t web_server_start(void);
