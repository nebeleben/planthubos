/* alert_ring.c -- the pure ring decision logic behind alert_post()/
 * alert_drain() (alert.h's top comment explains why alert.c is split this
 * way; see alert_rec_t's own comment in alert.h for the collapsing rule
 * this file implements). No FreeRTOS anywhere in this file -- directly
 * host-tested by tests/host/test_alert_ring.c, plain `cc`, no ESP-IDF
 * toolchain required. */
#include "alert.h"
#include <string.h>

void alert_ring_init(alert_ring_t *r)
{
    memset(r, 0, sizeof(*r));
}

void alert_ring_push(alert_ring_t *r, const alert_rec_t *in)
{
    if (r->count > 0) {
        uint8_t last = (uint8_t)((r->head + ALERT_RING_LEN - 1) % ALERT_RING_LEN);
        alert_rec_t *prev = &r->recs[last];
        if (prev->code == in->code && prev->dev_idx == in->dev_idx &&
            prev->action_id == in->action_id) {
            prev->level = in->level;
            prev->param = in->param;
            if (prev->repeat < UINT16_MAX) prev->repeat++;
            return;
        }
    }

    alert_rec_t rec = *in;
    rec.repeat = 1;

    /* Ring is full: writing at head overwrites the oldest record (head
     * already equals the current oldest slot once count == ALERT_RING_LEN)
     * -- the newest alert is the one that matters, so we drop the oldest
     * and count it rather than refuse the newest. */
    if (r->count == ALERT_RING_LEN) {
        r->dropped++;
    } else {
        r->count++;
    }
    r->recs[r->head] = rec;
    r->head = (uint8_t)((r->head + 1) % ALERT_RING_LEN);
}
