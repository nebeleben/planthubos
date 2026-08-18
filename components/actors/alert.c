#include "alert.h"
#include "action.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>

/* Fixed RAM ring of ALERT_RING_LEN (8) records (see alert.h's header
 * comment for why this exists as a separate, shallow front door in front
 * of event_log_append()). s_mux is a spinlock, not a mutex, for the same
 * reason ble_collector.c's s_adv_mux is one: a producer here can be an
 * esp_timer callback, which must never block, and a critical section
 * around a single struct-copy push is a few instructions. */
typedef struct {
    alert_rec_t recs[ALERT_RING_LEN];
    uint8_t     head;      /* next slot to write */
    uint8_t     count;     /* valid records queued, 0..ALERT_RING_LEN */
    uint32_t    dropped;   /* oldest-dropped-on-overflow, since last drain */
} alert_ring_t;

static alert_ring_t s_ring;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;

void alert_post(uint8_t level, uint8_t code, int dev_idx, uint8_t action_id, uint16_t param)
{
    alert_rec_t rec;
    rec.level = level;
    rec.code = code;
    rec.dev_idx = (int8_t)dev_idx;
    rec.action_id = action_id;
    rec.param = param;

    portENTER_CRITICAL(&s_mux);
    /* Ring is full: writing at head overwrites the oldest record (head
     * already equals the current oldest slot once count == ALERT_RING_LEN)
     * -- the newest alert is the one that matters, so we drop the oldest
     * and count it rather than refuse the newest. */
    if (s_ring.count == ALERT_RING_LEN) {
        s_ring.dropped++;
    } else {
        s_ring.count++;
    }
    s_ring.recs[s_ring.head] = rec;
    s_ring.head = (uint8_t)((s_ring.head + 1) % ALERT_RING_LEN);
    portEXIT_CRITICAL(&s_mux);
}

void alert_drain(void)
{
    alert_rec_t local[ALERT_RING_LEN];
    uint8_t  n;
    uint8_t  tail;
    uint32_t dropped;

    portENTER_CRITICAL(&s_mux);
    n = s_ring.count;
    tail = (uint8_t)((s_ring.head + ALERT_RING_LEN - n) % ALERT_RING_LEN);
    for (uint8_t i = 0; i < n; i++) {
        local[i] = s_ring.recs[(tail + i) % ALERT_RING_LEN];
    }
    dropped = s_ring.dropped;
    s_ring.count = 0;
    s_ring.dropped = 0;
    portEXIT_CRITICAL(&s_mux);

    /* event_log_append() below is the LittleFS fopen/fwrite + SSE/MQTT
     * chain -- safe here because alert_drain() only ever runs on the rules
     * engine task (see alert.h), never under the critical section above. */
    if (dropped > 0) {
        char msg[EVENT_MSG_MAX + 1];
        snprintf(msg, sizeof(msg), "alert ring overflow: %u alert(s) dropped",
                 (unsigned)dropped);
        event_log_append(EVENT_LEVEL_CRITICAL, 0, msg);
    }

    for (uint8_t i = 0; i < n; i++) {
        const alert_rec_t *r = &local[i];
        const action_t *a = action_get(r->action_id);
        char msg[EVENT_MSG_MAX + 1];
        snprintf(msg, sizeof(msg), "alert code=%u dev=%d action=%s param=%u",
                 (unsigned)r->code, (int)r->dev_idx, a ? a->name : "?",
                 (unsigned)r->param);
        event_log_append(r->level, 0, msg);
    }
}
