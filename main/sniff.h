/*
 * sniff.h - passive K-line capture, shared by the console and the web page.
 *
 * Bytes are split into frames on inter-byte gaps, because the framing is
 * unknown and gap timing is the main clue to where frames start and end.
 * Every frame goes into a ring buffer the web page polls; the console
 * variant also prints it. Never transmits.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SNIFF_RING        64    /* frames kept for the web page */
#define SNIFF_FRAME_MAX   48    /* bytes kept per frame; longer ones are marked truncated */
#define SNIFF_MAX_SECS    3600

typedef struct {
    uint32_t seq;        /* 1, 2, 3 ... across sessions; 0 = empty slot */
    uint32_t t_ms;       /* first byte, ms since the session started */
    uint32_t gap_ms;     /* silence before it; 0 for a session's first frame */
    uint16_t len;        /* bytes received, may exceed SNIFF_FRAME_MAX */
    uint8_t  d[SNIFF_FRAME_MAX];
} sniff_frame_t;

typedef struct {
    bool     running;
    char     owner[16];  /* "console" / "web", or the bus owner if busy */
    uint32_t session;    /* bumps on every start; the page clears on change */
    uint32_t last_seq;
    uint32_t bytes;      /* this session */
    uint32_t frames;     /* this session */
    uint32_t gap_ms;
    uint32_t remain_s;
} sniff_state_t;

/* Once at boot, before the console or web server can call in. */
void      sniff_init(void);

/* Blocking capture in the caller's task (the console's `sniff`).
 * ESP_ERR_INVALID_STATE if the bus is held by someone else. */
esp_err_t sniff_run(uint32_t secs, uint32_t gap_ms, bool print);

/* Background capture for the web page. Returns at once. */
esp_err_t sniff_start_bg(uint32_t secs, uint32_t gap_ms);

/* Ends whichever capture is running, console or web. */
void      sniff_stop(void);

void      sniff_get_state(sniff_state_t *out);

/* Copies frames with seq > since, oldest first. Returns the count. */
size_t    sniff_frames_since(uint32_t since, sniff_frame_t *out, size_t max);
