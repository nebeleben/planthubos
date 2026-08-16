/* adv_queue.c -- the raw-advertisement ring (M3 §1 design spec). Pure C99 +
 * libc only, deliberately no ESP-IDF/FreeRTOS includes, so the host test
 * (tests/host/test_adv_queue.c) compiles and exercises this file directly,
 * the same pattern battery_sched.c already uses in this component.
 *
 * ble_collector.c embeds one adv_ring_t as static storage (16 * 48 B
 * budgeted, M3 spec §7) and wraps push/pop in a short FreeRTOS critical
 * section -- see its own comment for why a spinlock rather than a mutex.
 * This file itself has no notion of tasks, locking, or "the" ring instance;
 * every caller owns and serializes access to its own adv_ring_t. */
#include "adv_queue.h"
#include <string.h>

_Static_assert(sizeof(adv_item_t) <= 48,
               "adv_item_t must fit the 48 B per-slot budget (M3 spec section 7)");

void adv_ring_init(adv_ring_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool adv_ring_push(adv_ring_t *r, const adv_item_t *it)
{
    if (r->full) {
        /* Existing slots are untouched -- head/tail don't move, only the
         * counter does, so a full ring never corrupts what it already
         * holds. */
        r->dropped++;
        return false;
    }
    r->slots[r->head] = *it;
    r->head = (uint8_t)((r->head + 1) % ADV_QUEUE_LEN);
    r->full = (r->head == r->tail);
    return true;
}

bool adv_ring_pop(adv_ring_t *r, adv_item_t *out)
{
    if (r->head == r->tail && !r->full) return false;   /* empty */
    *out = r->slots[r->tail];
    r->tail = (uint8_t)((r->tail + 1) % ADV_QUEUE_LEN);
    r->full = false;   /* a pop always leaves at least one free slot */
    return true;
}

size_t adv_ring_count(const adv_ring_t *r)
{
    if (r->full) return ADV_QUEUE_LEN;
    if (r->head >= r->tail) return (size_t)(r->head - r->tail);
    return (size_t)(ADV_QUEUE_LEN - (r->tail - r->head));
}

uint32_t adv_ring_dropped(const adv_ring_t *r)
{
    return r->dropped;
}
