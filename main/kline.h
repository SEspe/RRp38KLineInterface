/*
 * kline.h - ISO 9141 K-line transport for the Range Rover P38A
 *
 * The K-line is a single-wire, half-duplex, open-drain bus idling high at
 * battery voltage. A transceiver (L9637D or equivalent) converts it to 3.3V
 * UART levels. Because TX and RX share one wire, every byte written is
 * echoed back on RX and must be consumed - see kline_write().
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KLINE_MAX_FRAME 260

/* ISO 9141-2 timing parameters, milliseconds.
 * Spec ranges are noted; the values used are inside those ranges with a
 * little slack, because P38 modules are not always strictly compliant. */
#define KLINE_W1_MIN_MS   60    /* end of address -> 0x55 sync : 60..300  */
#define KLINE_W1_MAX_MS   400
#define KLINE_W2_MAX_MS   100   /* 0x55 -> KW1                 : 5..20    */
#define KLINE_W3_MAX_MS   100   /* KW1 -> KW2                  : 0..20    */
#define KLINE_W4_MS       30    /* KW2 -> inverted KW2         : 25..50   */
#define KLINE_W5_MS       320   /* bus idle before a new init  : >=300    */
#define KLINE_P1_MS       25    /* ECU inter-byte              : 0..20    */
#define KLINE_P2_MS       60    /* request -> response         : 25..50   */
#define KLINE_P3_MS       60    /* response -> next request    : 55..5000 */
#define KLINE_P4_MS       6     /* tester inter-byte           : 5..20    */

/* Result of a 5-baud (slow) initialisation. */
typedef struct {
    bool     responded;    /* anything at all came back            */
    bool     valid;        /* full handshake completed             */
    uint8_t  sync;         /* expected 0x55                        */
    uint8_t  kw1;          /* keyword 1                            */
    uint8_t  kw2;          /* keyword 2                            */
    uint8_t  addr_echo;    /* ECU's inverted address byte          */
    uint32_t w1_actual_ms; /* measured address-to-sync gap         */
} kline_init_result_t;

/* Bring up the UART and transceiver pins. Safe to call once at boot. */
esp_err_t kline_start(void);
void      kline_stop(void);
bool      kline_started(void);

/* Disarm and release the bus (bypass MOSFET off, transceiver asleep)
 * without touching the UART. Used before a reboot from the web side. */
void      kline_release_bus(void);

/* Bus ownership. Anything that reads or writes the UART - a console
 * command, a web-started sniff - claims it first, so two tasks never
 * interleave on the one wire. Non-blocking: false means someone else holds
 * it, and kline_owner() says who. `who` must be a string literal. */
bool        kline_claim(const char *who);
void        kline_unclaim(void);
const char *kline_owner(void);   /* "" when free */

/* Runtime transmit gate. Independent of CONFIG_RRP38_ALLOW_TX, which is a
 * compile-time gate; both must permit before any byte leaves the ESP32. */
bool      kline_tx_armed(void);
void      kline_set_tx_armed(bool armed);
bool      kline_tx_possible(void);   /* compile gate AND runtime gate */

/* Discard anything sitting in the RX buffer. */
void      kline_flush_rx(void);

/* Write bytes with ISO 9141 P4 inter-byte spacing, consuming the half-duplex
 * echo of each byte. Returns ESP_ERR_INVALID_RESPONSE on echo mismatch when
 * CONFIG_RRP38_ECHO_STRICT is set. */
esp_err_t kline_write(const uint8_t *buf, size_t len);

/* Read up to max bytes. Waits first_timeout_ms for the first byte, then
 * gap_timeout_ms between bytes; returns when the gap expires or buf is full.
 * Returns the byte count, or a negative value on error. */
int       kline_read(uint8_t *buf, size_t max,
                     uint32_t first_timeout_ms, uint32_t gap_timeout_ms);

/* Classic 5-baud address initialisation (ISO 9141-2). addr is the ECU
 * address byte - 0x33 is the OBD-II standard address. */
esp_err_t kline_slow_init(uint8_t addr, kline_init_result_t *out);

/* KWP2000 fast init: 25ms low, 25ms high, then the caller sends a
 * StartCommunication request. */
esp_err_t kline_fast_init(void);

/* Simple 8-bit modulo-256 sum, the checksum used by ISO 9141 and by most
 * Land Rover proprietary framing. */
uint8_t   kline_checksum(const uint8_t *buf, size_t len);

/* Hex helper: parses "81 12 F1 81" or "8112F181" into buf. Returns count
 * or -1 on a malformed string. */
int       kline_parse_hex(const char *s, uint8_t *buf, size_t max);

#ifdef __cplusplus
}
#endif
