/*
 * waveform.h - sample the level of a K-line pin over a short window and
 * keep every edge, so the web Debug tab can draw the pulse train.
 *
 * Reads a GPIO input and never drives a pin. Takes the bus claim, so it
 * cannot run alongside a sniff or a console command. Resolution is set by
 * the polling loop, roughly a microsecond or two, which is plenty for a
 * 10400 baud bit (96 us) but not for anything faster.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define WAVE_EDGES_MAX   2048      /* edges kept per sample; more sets `truncated` */
#define WAVE_MIN_US      1000      /* shortest window */
#define WAVE_MAX_US      200000    /* longest window: 200 ms */

typedef enum {
    WAVE_PIN_RX,                   /* the bus side, after the transceiver */
    WAVE_PIN_TX,                   /* what the ESP32 drives towards the transceiver */
} wave_pin_t;

typedef struct {
    uint8_t  level0;               /* pin level at the start of the window */
    uint32_t window_us;            /* measured length of the window */
    uint32_t edges;                /* level changes recorded */
    bool     truncated;            /* edge buffer filled before the window ended */
    uint32_t high_us;              /* time spent high */
    uint32_t low_us;               /* time spent low */
    uint32_t min_high_us;          /* shortest complete high pulse, 0 if none */
    uint32_t min_low_us;           /* shortest complete low pulse, 0 if none */
    uint32_t est_baud;             /* 1e6 / min pulse; 0 if no complete pulse */
} wave_result_t;

typedef struct {
    uint32_t t_us;                 /* since the start of the window */
    uint8_t  level;                /* level from t_us onward */
} wave_edge_t;

/* Blocking sample for `window_us` (clamped to the limits above). Holds the
 * bus claim for the whole window. ESP_ERR_INVALID_STATE if the bus is busy.
 * Only one sample is kept at a time: a new one replaces the last. */
esp_err_t wave_sample(wave_pin_t pin, uint32_t window_us, wave_result_t *out);

/* The same sample without taking the bus claim. The caller must hold it. */
esp_err_t wave_capture(wave_pin_t pin, uint32_t window_us, wave_result_t *out);

/* Copies the edges of the last sample, oldest first. Returns the count. */
size_t    wave_edges(wave_edge_t *out, size_t max);
