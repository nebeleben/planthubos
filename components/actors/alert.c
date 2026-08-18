#include "alert.h"
#include "action.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

/* Fixed RAM ring of ALERT_RING_LEN (8) records (see alert.h's header
 * comment for why this exists as a separate, shallow front door in front
 * of event_log_append()). s_mux is a spinlock, not a mutex, for the same
 * reason ble_collector.c's s_adv_mux is one: a producer here can be an
 * esp_timer callback, which must never block, and a critical section
 * around a single struct-copy push is a few instructions. alert_ring_t and
 * alert_ring_push() (alert.h/alert_ring.c) are the pure collapsing/
 * eviction decision -- this file only ever takes the spinlock, calls one
 * of them, and releases it. */
static alert_ring_t s_ring;
static portMUX_TYPE  s_mux = portMUX_INITIALIZER_UNLOCKED;
static alert_wake_fn_t s_wake_hook;

void alert_set_wake_hook(alert_wake_fn_t fn)
{
    s_wake_hook = fn;
}

void alert_post(uint8_t level, uint8_t code, int dev_idx, uint8_t action_id, uint16_t param)
{
    alert_rec_t rec;
    rec.level = level;
    rec.code = code;
    rec.dev_idx = (int8_t)dev_idx;
    rec.action_id = action_id;
    rec.param = param;
    rec.repeat = 0; /* alert_ring_push() ignores this on input and sets it itself */

    portENTER_CRITICAL(&s_mux);
    alert_ring_push(&s_ring, &rec);
    portEXIT_CRITICAL(&s_mux);

    /* Wake the rules engine task immediately rather than waiting for its
     * next natural wake -- see alert_wake_fn_t's comment in alert.h. Called
     * AFTER releasing the spinlock above: xEventGroupSetBits() can unblock
     * a task and yield, which must never happen while s_mux is held. A NULL
     * hook (no rules_init() yet, or rules_init() itself failed) is a safe
     * no-op -- the record is already queued above, and the drop counter
     * still reports an eventual overflow if the ring is never drained; a
     * hub whose rules_init() failed is already a logged broken-hub state
     * that this hook is not trying to rescue. */
    if (s_wake_hook) s_wake_hook();
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
        char msg[EVENT_MSG_MAX + 1];
        /* ACTION_NONE means this alert is not about a specific action (see
         * alert_post()'s comment in alert.h) -- omit the clause entirely
         * rather than print a name for an action that was never involved.
         * repeat > 1 (alert_ring_push()'s collapsing, Task 11) appends
         * " (x<N>)" so the count reaches GET /api/v1/events through the
         * SAME "msg" text every other alert already carries -- no new event
         * field, no event_t format bump, and the UI can render it (as
         * plain text, or a highlighted "xN" badge) directly off this
         * string. */
        if (r->action_id == ACTION_NONE) {
            snprintf(msg, sizeof(msg), "alert code=%u dev=%d param=%u",
                     (unsigned)r->code, (int)r->dev_idx, (unsigned)r->param);
        } else {
            const action_t *a = action_get(r->action_id);
            snprintf(msg, sizeof(msg), "alert code=%u dev=%d action=%s param=%u",
                     (unsigned)r->code, (int)r->dev_idx, a ? a->name : "?",
                     (unsigned)r->param);
        }
        if (r->repeat > 1) {
            size_t len = strlen(msg);
            snprintf(msg + len, sizeof(msg) - len, " (x%u)", (unsigned)r->repeat);
        }
        event_log_append(r->level, 0, msg);
    }
}
