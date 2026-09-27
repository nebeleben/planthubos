#include <assert.h>
#include <stdio.h>
#include "data_core.h"
#include "capability.h"

int main(void) {
    data_core_init();
    device_id_t b = { .kind = 2 /* DEV_KIND_ZIGBEE */ };
    b.addr[0] = 0xAB;

    /* no event pending initially */
    int16_t code = -1;
    assert(!data_core_events_for(&b, CAP_BUTTON_ACTION, &code));
    assert(data_core_events_peek_seq() == 0);

    /* two identical single-presses: both pending, seq advances */
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 1));
    uint32_t s1 = data_core_events_peek_seq(); assert(s1 > 0);
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 1));
    uint32_t s2 = data_core_events_peek_seq(); assert(s2 > s1);
    assert(data_core_events_for(&b, CAP_BUTTON_ACTION, &code) && code == 1);

    /* a normal value submit on an event cap is rejected */
    assert(!data_core_submit_cap_id(&b, CAP_BUTTON_ACTION, 1.0f));

    /* consume through the pass's high seq -> nothing pending, re-arm */
    data_core_events_consume_through(s2);
    assert(!data_core_events_for(&b, CAP_BUTTON_ACTION, &code));
    assert(data_core_events_peek_seq() == 0);

    /* an event arriving after the peek (seq > consumed) survives */
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 2));
    uint32_t s3 = data_core_events_peek_seq();
    data_core_events_consume_through(s3 - 1);          /* consume an earlier bound */
    assert(data_core_events_for(&b, CAP_BUTTON_ACTION, &code) && code == 2);

    /* Task 6 (multi-endpoint-zigbee): data_core_submit_cap_id_ep() routes a
     * value to a specific (cap, endpoint) instance instead of always the
     * device's default slot -- a dual On/Off device's second gang (endpoint
     * 2) must not clobber, or be clobbered by, the first gang (endpoint 1). */
    device_id_t sw = { .kind = 2 /* DEV_KIND_ZIGBEE */ };
    sw.addr[0] = 0xCD;
    assert(data_core_submit_cap_id_ep(&sw, CAP_SWITCH_STATE, 2, 1.0f, 0));
    /* endpoint 2 was the FIRST write for this (device, cap) -> it becomes
     * the tracked default. A second write at a LOWER endpoint (1) is still
     * accepted, but promotes endpoint 1 to the new default and demotes
     * endpoint 2's value into the extra_ep_caps side list rather than
     * overwriting it in place (registry_set_cap_ep()'s promotion rule,
     * Task 4) -- so the endpoint-2 reading survives, just relocated. */
    assert(data_core_submit_cap_id_ep(&sw, CAP_SWITCH_STATE, 1, 0.0f, 0));
    device_entry_t sw_entry;
    assert(data_core_get_device(&sw, &sw_entry));
    assert(sw_entry.caps[CAP_SWITCH_STATE].valid);
    assert(capability_decode(CAP_SWITCH_STATE, sw_entry.caps[CAP_SWITCH_STATE].raw) == 0.0f);
    bool found_ep2 = false;
    for (uint8_t i = 0; i < sw_entry.extra_ep_cap_count; i++) {
        if (sw_entry.extra_ep_caps[i].cap_id == CAP_SWITCH_STATE && sw_entry.extra_ep_caps[i].endpoint == 2) {
            found_ep2 = true;
            assert(capability_decode(CAP_SWITCH_STATE, sw_entry.extra_ep_caps[i].slot.raw) == 1.0f);
        }
    }
    assert(found_ep2);

    printf("test_data_core_events: OK\n");
    return 0;
}
