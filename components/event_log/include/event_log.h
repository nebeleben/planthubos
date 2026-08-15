#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define EVENT_SLOTS   256
#define EVENT_MSG_MAX 118

typedef struct {
    uint32_t seq;        /* monotonic, 0 = empty slot */
    uint32_t ts;         /* epoch seconds (0 if clock never synced) */
    uint32_t rule_id;
    uint8_t  level;      /* 0 log, 1 notify */
    char     msg[EVENT_MSG_MAX + 1];
} event_t;

/* Pure ring over a caller-provided slot array (host-testable). */
typedef struct { event_t *slots; uint32_t next_seq; } event_ring_t;
void     event_ring_init(event_ring_t *r, event_t *slots);         /* scans for max seq */
uint32_t event_ring_append(event_ring_t *r, uint32_t ts, uint32_t rule_id,
                           uint8_t level, const char *msg);         /* returns seq */
/* Copies up to max events with seq > after, oldest first; returns count. */
size_t   event_ring_read(const event_ring_t *r, uint32_t after,
                         event_t *out, size_t max);

/* Device wrapper (event_log.c): loads/saves /storage/events.bin, serializes
 * appends behind a mutex, calls the SSE push hook + MQTT hook if registered. */
void     event_log_init(void);
uint32_t event_log_append(uint8_t level, uint32_t rule_id, const char *msg);
size_t   event_log_read(uint32_t after, event_t *out, size_t max);
uint32_t event_log_last_seq(void);
typedef void (*event_hook_t)(const event_t *e);
void     event_log_set_hooks(event_hook_t sse_hook, event_hook_t mqtt_hook);
