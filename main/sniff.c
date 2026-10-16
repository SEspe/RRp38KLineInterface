/*
 * sniff.c - passive K-line capture, shared by the console and the web page.
 *
 * Timestamps are taken when this task reads each byte out of the UART
 * driver's buffer, not when the byte arrived on the wire. With WiFi running
 * on the C6's single core that adds a few ms of jitter, which matters only
 * for gaps close to the split threshold.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "kline.h"
#include "sniff.h"

static const char *TAG = "sniff";

static sniff_frame_t     s_ring[SNIFF_RING];
static SemaphoreHandle_t s_lock;          /* guards everything below */
static StaticSemaphore_t s_lock_buf;
static uint32_t          s_seq;
static uint32_t          s_session;
static uint32_t          s_bytes;
static uint32_t          s_frames;
static uint32_t          s_gap_ms;
static int64_t           s_end_us;
static const char       *s_owner = "";
static volatile bool     s_running;
static volatile bool     s_stop;

void sniff_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void commit(const sniff_frame_t *f)
{
    lock();
    s_seq++;
    sniff_frame_t *slot = &s_ring[s_seq % SNIFF_RING];
    *slot = *f;
    slot->seq = s_seq;
    s_frames++;
    unlock();
}

static void session_begin(const char *owner, uint32_t secs, uint32_t gap_ms)
{
    lock();
    s_session++;
    s_bytes  = 0;
    s_frames = 0;
    s_gap_ms = gap_ms;
    s_owner  = owner;
    s_end_us = esp_timer_get_time() + (int64_t)secs * 1000000;
    s_stop   = false;
    s_running = true;
    unlock();
}

static void session_end(void)
{
    lock();
    s_running = false;
    unlock();
}

/* The capture loop. The caller holds the bus claim. */
static void capture(uint32_t gap_ms, bool print)
{
    kline_flush_rx();

    const int64_t start_us = esp_timer_get_time();
    /* Poll at most every 20 ms so a frame closes soon after its gap. */
    const uint32_t poll_ms = gap_ms < 20 ? (gap_ms ? gap_ms : 1) : 20;

    sniff_frame_t cur;
    bool    open    = false;
    int64_t last_us = 0;
    int     col     = 0;

    if (print) {
        printf("listening, splitting frames on gaps > %" PRIu32 "ms\n", gap_ms);
        printf("(passive - nothing is transmitted)\n\n");
    }

    while (!s_stop && esp_timer_get_time() < s_end_us) {
        uint8_t b;
        int r = kline_read(&b, 1, poll_ms, 0);
        int64_t now_us = esp_timer_get_time();

        if (r != 1) {
            if (open && (now_us - last_us) / 1000 > (int64_t)gap_ms) {
                commit(&cur);
                open = false;
            }
            continue;
        }

        int64_t gap = last_us ? (now_us - last_us) / 1000 : -1;
        if (open && gap > (int64_t)gap_ms) {
            commit(&cur);
            open = false;
        }
        if (!open) {
            memset(&cur, 0, sizeof(cur));
            cur.t_ms   = (uint32_t)((now_us - start_us) / 1000);
            cur.gap_ms = gap < 0 ? 0 : (uint32_t)gap;
            open = true;
            if (print) {
                if (col != 0) {
                    printf("\n");
                }
                printf("[%8.3fs +%5lldms] ", (double)now_us / 1e6,
                       (long long)(gap < 0 ? 0 : gap));
                col = 0;
            }
        }
        if (cur.len < SNIFF_FRAME_MAX) {
            cur.d[cur.len] = b;
        }
        if (cur.len < UINT16_MAX) {
            cur.len++;
        }
        s_bytes++;
        last_us = now_us;

        if (print) {
            printf("%02X ", b);
            if (++col >= 24) {
                printf("\n                     ");
                col = 1;
            }
            fflush(stdout);
        }
    }
    if (open) {
        commit(&cur);
    }
    if (print) {
        printf("\n\n%" PRIu32 " bytes captured%s\n", s_bytes, s_stop ? " (stopped)" : "");
    }
}

static uint32_t clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

esp_err_t sniff_run(uint32_t secs, uint32_t gap_ms, bool print)
{
    if (!kline_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!kline_claim("console")) {
        return ESP_ERR_INVALID_STATE;
    }
    session_begin("console", clamp(secs, 1, SNIFF_MAX_SECS), clamp(gap_ms, 1, 1000));
    capture(s_gap_ms, print);
    session_end();
    kline_unclaim();
    return ESP_OK;
}

static void bg_task(void *arg)
{
    capture(s_gap_ms, false);
    ESP_LOGI(TAG, "web sniff ended: %" PRIu32 " bytes, %" PRIu32 " frames", s_bytes, s_frames);
    session_end();
    kline_unclaim();
    vTaskDelete(NULL);
}

esp_err_t sniff_start_bg(uint32_t secs, uint32_t gap_ms)
{
    if (!kline_started()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!kline_claim("web")) {
        return ESP_ERR_INVALID_STATE;
    }
    session_begin("web", clamp(secs, 1, SNIFF_MAX_SECS), clamp(gap_ms, 1, 1000));
    /* Above the HTTP server (5) so a busy page does not delay byte reads. */
    if (xTaskCreate(bg_task, "sniff", 4096, NULL, 6, NULL) != pdPASS) {
        session_end();
        kline_unclaim();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "web sniff started: %" PRIu32 " ms gap", s_gap_ms);
    return ESP_OK;
}

void sniff_stop(void)
{
    s_stop = true;
}

void sniff_get_state(sniff_state_t *out)
{
    memset(out, 0, sizeof(*out));
    lock();
    out->running  = s_running;
    out->session  = s_session;
    out->last_seq = s_seq;
    out->bytes    = s_bytes;
    out->frames   = s_frames;
    out->gap_ms   = s_gap_ms;
    if (s_running) {
        int64_t left = (s_end_us - esp_timer_get_time()) / 1000000;
        out->remain_s = left > 0 ? (uint32_t)left : 0;
        strlcpy(out->owner, s_owner, sizeof(out->owner));
    } else {
        /* Not sniffing, but a console init or send may hold the bus. */
        strlcpy(out->owner, kline_owner(), sizeof(out->owner));
    }
    unlock();
}

size_t sniff_frames_since(uint32_t since, sniff_frame_t *out, size_t max)
{
    size_t n = 0;
    lock();
    uint32_t first = since + 1;
    if (s_seq >= SNIFF_RING && first < s_seq - SNIFF_RING + 1) {
        first = s_seq - SNIFF_RING + 1;   /* older ones are overwritten */
    }
    for (uint32_t q = first; q <= s_seq && n < max; q++) {
        out[n++] = s_ring[q % SNIFF_RING];
    }
    unlock();
    return n;
}
