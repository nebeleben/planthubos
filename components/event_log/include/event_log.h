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
/* Shortened from 118 in M5b Task 5 (by exactly the 2 B added below as
 * `crc`) so sizeof(event_t) stays 132 B -- EVENT_SLOTS of these are static,
 * and this milestone's static budget does not have 128 B (EVENT_SLOTS * 4)
 * to spare (see M5a's boot-time failures noted above). */
#define EVENT_MSG_MAX 116

/* Levels, in ascending severity. LOG/NOTIFY (0/1) predate this task;
 * ALERT/CRITICAL (2/3) are M5b Task 5's addition for the safety core (a
 * close that could not be confirmed, a guard refusal, a command dropped for
 * expiry). Widening event_ring_sanitize()'s old "level > 1 => torn write"
 * check to "level > EVENT_LEVEL_MAX" is exactly why this task also adds
 * `crc` below: relying on the level range to catch torn writes is what made
 * levels 2/3 look like corruption in the first place, so once the range
 * legitimately includes them, a real integrity check has to carry the
 * guarantee the range used to. */
#define EVENT_LEVEL_LOG      0
#define EVENT_LEVEL_NOTIFY   1
#define EVENT_LEVEL_ALERT    2
#define EVENT_LEVEL_CRITICAL 3
#define EVENT_LEVEL_MAX      EVENT_LEVEL_CRITICAL

typedef struct {
    uint32_t seq;        /* monotonic, 0 = empty slot */
    uint32_t ts;         /* epoch seconds (0 if clock never synced) */
    uint32_t rule_id;
    uint8_t  level;      /* 0 log .. 3 critical, see EVENT_LEVEL_* above */
    char     msg[EVENT_MSG_MAX + 1];
    /* Checksum over every other byte of the record (offsetof(event_t, crc)
     * bytes starting at 0), set by event_ring_append() and verified by
     * event_ring_sanitize(). This is the SECOND of the sanitizer's two
     * validity signals (the first is msg[EVENT_MSG_MAX] == '\0') -- it
     * replaces the old "level > 1 implies torn write" inference, which a
     * widened level range can no longer supply. Placed last so it, and
     * nothing else, is excluded from the range it covers. */
    uint16_t crc;
} event_t;
_Static_assert(sizeof(event_t) == 132,
               "event_t must not grow: EVENT_SLOTS of these are static");

/* Pure ring over a caller-provided slot array (host-testable). */
typedef struct { event_t *slots; uint32_t next_seq; } event_ring_t;
/* Computes the record checksum stored in event_t.crc (see the field's own
 * comment) -- exposed so tests can construct valid records by hand. */
uint16_t event_record_crc(const event_t *e);
/* Zeroes any slot that fails validity: level > EVENT_LEVEL_MAX (cheap first
 * reject), or msg[EVENT_MSG_MAX] != '\0', or crc mismatch (the integrity
 * check that carries the guarantee -- see event_t.crc). A torn write
 * (unclean power loss mid-record) can leave a slot with a bogus seq, which
 * event_ring_init would otherwise trust and use to desync the whole ring's
 * numbering. Call before event_ring_init on any array loaded from
 * persistent storage. event_ring_append always produces valid slots, so
 * this never touches a slot it wrote itself. */
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
