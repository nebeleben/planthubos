/* tests/host/test_event_integrity.c */
#include "event_log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A record written at an alert level must SURVIVE sanitize. This is the
 * regression that motivated the whole task. */
static void test_alert_levels_survive_sanitize(void) {
    event_t slots[EVENT_SLOTS];
    memset(slots, 0, sizeof slots);
    event_ring_t r; event_ring_init(&r, slots);
    event_ring_append(&r, /*ts*/ 0, /*rule_id*/ 0, EVENT_LEVEL_ALERT, "valve close unconfirmed");
    event_ring_append(&r, 0, 0, EVENT_LEVEL_CRITICAL, "valve stuck open");
    event_ring_sanitize(slots);
    assert(slots[0].seq != 0 && slots[0].level == EVENT_LEVEL_ALERT);
    assert(slots[1].seq != 0 && slots[1].level == EVENT_LEVEL_CRITICAL);
}

/* A torn record is still caught -- the check moved to the checksum, it did
 * not disappear. Corrupt one byte of a valid record and it must be zeroed. */
static void test_torn_record_still_caught(void) {
    event_t slots[EVENT_SLOTS];
    memset(slots, 0, sizeof slots);
    event_ring_t r; event_ring_init(&r, slots);
    event_ring_append(&r, 0, 0, EVENT_LEVEL_ALERT, "hello");
    slots[0].msg[0] ^= 0xFF;               /* torn payload, plausible header */
    event_ring_sanitize(slots);
    assert(slots[0].seq == 0);
}

static void test_level_above_max_still_rejected(void) {
    event_t slots[EVENT_SLOTS];
    memset(slots, 0, sizeof slots);
    event_ring_t r; event_ring_init(&r, slots);
    event_ring_append(&r, 0, 0, EVENT_LEVEL_ALERT, "x");
    slots[0].level = 9;
    event_ring_sanitize(slots);
    assert(slots[0].seq == 0);
}

int main(void) {
    test_alert_levels_survive_sanitize();
    test_torn_record_still_caught();
    test_level_above_max_still_rejected();
    printf("test_event_integrity: OK\n");
    return 0;
}
