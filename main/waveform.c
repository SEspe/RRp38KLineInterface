/*
 * waveform.c - polling edge capture for the Debug tab. See waveform.h.
 *
 * Each edge is one word: cycles since the start of the window, shifted left
 * one bit, with the new level in bit 0. Cycles come from the CPU counter,
 * which is far finer than esp_timer; the counter is converted to time with
 * the rate measured over the same window, so no clock setting is assumed.
 */
#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "kline.h"
#include "waveform.h"

static const char *TAG = "wave";

#define WAVE_TX_GPIO  (CONFIG_RRP38_KLINE_TX_GPIO)
#define WAVE_RX_GPIO  (CONFIG_RRP38_KLINE_RX_GPIO)

static uint32_t s_edge[WAVE_EDGES_MAX];
static uint32_t s_n;
static uint32_t s_cyc_per_us = 1;

static uint32_t to_us(uint32_t cyc)
{
    return cyc / s_cyc_per_us;
}

/* Adds one stretch of constant level to the totals. Only stretches that
 * begin and end at an edge count towards the shortest pulse; the ones at
 * either end of the window are cut short by the window itself. */
static void add_segment(wave_result_t *r, uint8_t level, uint32_t cyc, bool complete)
{
    uint32_t d = to_us(cyc);
    if (level) {
        r->high_us += d;
    } else {
        r->low_us += d;
    }
    if (!complete || d == 0) {
        return;
    }
    uint32_t *min = level ? &r->min_high_us : &r->min_low_us;
    if (*min == 0 || d < *min) {
        *min = d;
    }
}

static void capture(int gpio, uint32_t window_us, wave_result_t *r)
{
    const int64_t t0 = esp_timer_get_time();
    const uint32_t c0 = esp_cpu_get_cycle_count();
    const int64_t t_end = t0 + window_us;

    uint8_t last = gpio_get_level(gpio);
    r->level0 = last;

    uint32_t n = 0;
    bool full = false;
    uint32_t iter = 0;
    for (;;) {
        uint32_t c = esp_cpu_get_cycle_count();
        uint8_t lv = gpio_get_level(gpio);
        if (lv != last) {
            if (n >= WAVE_EDGES_MAX) {
                full = true;
                break;
            }
            s_edge[n++] = ((c - c0) << 1) | lv;
            last = lv;
        }
        /* The timer is read every 256 polls: cheap, but still steers the end. */
        if ((++iter & 0xFF) == 0 && esp_timer_get_time() >= t_end) {
            break;
        }
    }

    const int64_t t1 = esp_timer_get_time();
    const uint32_t c1 = esp_cpu_get_cycle_count();
    const uint32_t total = c1 - c0;

    uint32_t span_us = (uint32_t)(t1 - t0);
    if (span_us == 0) {
        span_us = 1;
    }
    s_cyc_per_us = total / span_us;
    if (s_cyc_per_us == 0) {
        s_cyc_per_us = 1;
    }
    s_n = n;

    r->window_us  = span_us;
    r->edges      = n;
    r->truncated  = full;

    uint32_t prev = 0;
    uint8_t lv = r->level0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t at = s_edge[i] >> 1;
        add_segment(r, lv, at - prev, i > 0);
        prev = at;
        lv = s_edge[i] & 1;
    }
    add_segment(r, lv, total - prev, false);

    uint32_t shortest = r->min_high_us;
    if (r->min_low_us && (!shortest || r->min_low_us < shortest)) {
        shortest = r->min_low_us;
    }
    r->est_baud = shortest ? 1000000 / shortest : 0;
}

esp_err_t wave_sample(wave_pin_t pin, uint32_t window_us, wave_result_t *out)
{
    if (!kline_claim("web")) {
        return ESP_ERR_INVALID_STATE;
    }
    if (window_us < WAVE_MIN_US) {
        window_us = WAVE_MIN_US;
    }
    if (window_us > WAVE_MAX_US) {
        window_us = WAVE_MAX_US;
    }

    int gpio = pin == WAVE_PIN_TX ? WAVE_TX_GPIO : WAVE_RX_GPIO;
    /* Input buffer only: the UART keeps driving TX, and nothing is written. */
    gpio_input_enable((gpio_num_t)gpio);

    memset(out, 0, sizeof(*out));
    capture(gpio, window_us, out);
    kline_unclaim();

    ESP_LOGD(TAG, "pin %s: %" PRIu32 " edges in %" PRIu32 " us, %" PRIu32 " cycles/us",
             pin == WAVE_PIN_TX ? "tx" : "rx", out->edges, out->window_us, s_cyc_per_us);
    return ESP_OK;
}

size_t wave_edges(wave_edge_t *out, size_t max)
{
    size_t n = s_n < max ? s_n : max;
    for (size_t i = 0; i < n; i++) {
        out[i].t_us  = to_us(s_edge[i] >> 1);
        out[i].level = s_edge[i] & 1;
    }
    return n;
}
