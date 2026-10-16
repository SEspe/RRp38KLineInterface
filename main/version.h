#pragma once

/* FIRMWARE_VERSION is also read by the top-level CMakeLists.txt into
 * PROJECT_VER, so it lands in the app descriptor and the version of the image
 * in the inactive OTA slot can be read back at runtime. */
#define FIRMWARE_VERSION    "0.2.0"
#define FIRMWARE_NAME       "RRp38KLine"
#define MDNS_HOSTNAME       "rrp38"          /* http://rrp38.local/ and the DHCP hostname */
#define WIFI_AP_SSID        "RRp38-KLine"
#define WIFI_AP_PASSWORD    "rrp38kline"     /* default until changed on the WiFi tab; WPA2 needs >= 8 chars */
#define NVS_NAMESPACE       "rrp38"
