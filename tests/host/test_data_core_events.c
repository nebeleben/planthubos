#include <assert.h>
#include <stdio.h>
#include "data_core.h"
#include "capability.h"

int main(void) {
    data_core_init();
    device_id_t b = { .kind = 1 /* DEV_KIND_ZIGBEE */ };
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

    printf("test_data_core_events: OK\n");
    return 0;
}
