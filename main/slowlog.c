/*
 * slowlog.c - see slowlog.h.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "slowlog.h"

static const char *TAG = "slowlog";

#define SLOWLOG_GPIO    (CONFIG_RRP38_KLINE_RX_GPIO)
#define SLOWLOG_PERIOD_US 1000             /* 1 kHz: a 5-baud bit is 200 ms */
#define SLOWLOG_MAX     12288              /* edges kept, about 96 KB */

static slowlog_edge_t   s_edges[SLOWLOG_MAX];
static volatile uint32_t s_count;
static volatile bool    s_running;
static volatile bool    s_full;
static int              s_last = -1;
static int64_t          s_t0_us;
static esp_timer_handle_t s_timer;

/* Runs every 1 ms in the esp_timer task. Reads the pin, records any change. */
static void sample(void *arg)
{
    (void)arg;
    if (!s_running) {
        return;
    }
    int level = gpio_get_level((gpio_num_t)SLOWLOG_GPIO);
    if (level == s_last) {
        return;
    }
    s_last = level;
    if (s_count >= SLOWLOG_MAX) {
        s_full = true;
        s_running = false;
        ESP_LOGW(TAG, "buffer full at %" PRIu32 " edges; stopped", s_count);
        return;
    }
    s_edges[s_count].t_ms  = (uint32_t)((esp_timer_get_time() - s_t0_us) / 1000);
    s_edges[s_count].level = (uint8_t)level;
    s_count++;
}

bool slowlog_start(void)
{
    if (s_running) {
        return false;
    }
    if (!s_timer) {
        const esp_timer_create_args_t a = { .callback = sample, .name = "slowlog" };
        if (esp_timer_create(&a, &s_timer) != ESP_OK) {
            return false;
        }
    }
    gpio_input_enable((gpio_num_t)SLOWLOG_GPIO);
    s_count = 0;
    s_full = false;
    s_last = -1;
    s_t0_us = esp_timer_get_time();
    s_running = true;
    if (esp_timer_start_periodic(s_timer, SLOWLOG_PERIOD_US) != ESP_OK) {
        s_running = false;
        return false;
    }
    ESP_LOGI(TAG, "started: 1 kHz RX log on GPIO %d", SLOWLOG_GPIO);
    return true;
}

void slowlog_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false;
    esp_timer_stop(s_timer);
    ESP_LOGI(TAG, "stopped: %" PRIu32 " edges", s_count);
}

void slowlog_state(slowlog_state_t *out)
{
    out->running = s_running;
    out->full = s_full;
    out->count = s_count;
}

size_t slowlog_get(uint32_t from, slowlog_edge_t *out, size_t max)
{
    uint32_t n = s_count;
    if (from >= n) {
        return 0;
    }
    size_t k = 0;
    for (uint32_t i = from; i < n && k < max; i++) {
        out[k++] = s_edges[i];
    }
    return k;
}
