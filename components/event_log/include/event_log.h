#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 64, not the spec's original 256: the ring is mirrored in static RAM
 * (EVENT_SLOTS * sizeof(event_t) ~ 8.5KB at 64). At 256 (~34KB) the C3
 * no longer had enough contiguous heap for the BLE controller -- BLE_INIT
 * "hci inits failed" on real hardware, silently killing sensor collection.
 * 32 events is still ample feed history for the UI (second reduction from
 * 64: the sampler then failed ESP_ERR_NO_MEM on the same board -- every
 * static KB matters on the C3 once WiFi+BLE are both up). */
#define EVENT_SLOTS   32
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
/* Zeroes any slot that fails validity (msg[EVENT_MSG_MAX] != '\0', or
 * level > 1) -- a torn write (unclean power loss mid-record) can leave a
 * slot with a bogus seq, which event_ring_init would otherwise trust and
 * use to desync the whole ring's numbering. Call before event_ring_init
 * on any array loaded from persistent storage. event_ring_append always
 * produces valid slots, so this never touches a slot it wrote itself. */
void     event_ring_sanitize(event_t *slots);
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
