/*
 * slowlog.h - continuous, passive 1 kHz log of the K-line RX pin.
 *
 * Samples the pin once per millisecond and keeps a list of level changes
 * (time since start, new level). It never drives a pin and does not claim
 * the bus, so it can run alongside the passive sniff. Built for recording a
 * five-baud init, which a 10400 sniff cannot see.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t t_ms;   /* since start */
    uint8_t  level;  /* new level at t_ms */
} slowlog_edge_t;

/* Starts a new log, clearing the previous one. Returns false if the log
 * is already running. */
bool slowlog_start(void);

/* Stops the log. The data is kept until the next start. */
void slowlog_stop(void);

typedef struct {
    bool     running;
    bool     full;       /* buffer filled before a stop */
    uint32_t count;      /* edges recorded */
} slowlog_state_t;

void slowlog_state(slowlog_state_t *out);

/* Copies edges with index >= from, up to max. Returns the number copied. */
size_t slowlog_get(uint32_t from, slowlog_edge_t *out, size_t max);
