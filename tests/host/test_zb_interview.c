/* Host test for zb_interview.c (M6b spec section 5). The interview as a
 * pure step function: no radio, no timers, no ESP-IDF. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "zb_interview.h"
#include "capability.h"
#include "action.h"

int main(void) {
    static const uint8_t EUI[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    zb_iv_t iv;

    /* --- the happy path: a plug with On/Off --- */
    zb_interview_begin(&iv, EUI, 0x1234, 100);
    assert(zb_interview_step(&iv, 100) == ZB_IV_ACT_SEND_ACTIVE_EP);
    /* Asking twice before a reply arrives must NOT re-send. */
    assert(zb_interview_step(&iv, 101) == ZB_IV_ACT_NONE);

    uint8_t eps[2] = { 1, 0 };
    zb_interview_on_endpoints(&iv, eps, 1);
    assert(zb_interview_step(&iv, 102) == ZB_IV_ACT_SEND_SIMPLE_DESC);
    assert(iv.pending_endpoint == 1);

    uint16_t clusters[2] = { 0x0006, 0x0000 };   /* On/Off + Basic */
    zb_interview_on_clusters(&iv, 1, clusters, 2);
    assert(zb_interview_step(&iv, 103) == ZB_IV_ACT_SEND_CONFIG_REPORT);
    assert(zb_interview_step(&iv, 104) == ZB_IV_ACT_STORE);
    assert(iv.state == ZB_IV_DONE);

    /* The built device carries the mapping, not the raw clusters. */
    assert(iv.dev.interviewed == 1);
    assert(iv.dev.cap_count == 1 && iv.dev.caps[0] == CAP_SWITCH_STATE);
    assert(iv.dev.action_count == 2);
    assert(iv.dev.actions[0] == ACT_SWITCH_ON);
    assert(iv.dev.actions[1] == ACT_SWITCH_OFF);
    assert(memcmp(iv.dev.eui64, EUI, 8) == 0);

    /* --- a sensor: capability, no actions --- */
    zb_interview_begin(&iv, EUI, 0x1234, 200);
    zb_interview_step(&iv, 200);
    zb_interview_on_endpoints(&iv, eps, 1);
    zb_interview_step(&iv, 201);
    uint16_t sensor[1] = { 0x0402 };
    zb_interview_on_clusters(&iv, 1, sensor, 1);
    zb_interview_step(&iv, 202);
    assert(zb_interview_step(&iv, 203) == ZB_IV_ACT_STORE);
    assert(iv.dev.caps[0] == CAP_AIR_TEMPERATURE);
    assert(iv.dev.action_count == 0);

    /* --- a device with nothing mappable is STORED, not dropped ---
     * It is exactly M6c's input, so losing it loses the evidence. */
    zb_interview_begin(&iv, EUI, 0x1234, 300);
    zb_interview_step(&iv, 300);
    zb_interview_on_endpoints(&iv, eps, 1);
    zb_interview_step(&iv, 301);
    uint16_t tuya[1] = { 0xEF00 };
    zb_interview_on_clusters(&iv, 1, tuya, 1);
    assert(zb_interview_step(&iv, 302) == ZB_IV_ACT_STORE);
    assert(iv.state == ZB_IV_DONE);
    assert(iv.dev.cap_count == 0 && iv.dev.action_count == 0);
    assert(iv.dev.interviewed == 1);        /* interviewed, just unmapped */

    /* --- a silent device times out and is still stored --- */
    zb_interview_begin(&iv, EUI, 0x1234, 400);
    assert(zb_interview_step(&iv, 400) == ZB_IV_ACT_SEND_ACTIVE_EP);
    assert(zb_interview_step(&iv, 400 + ZB_IV_TIMEOUT_S - 1) == ZB_IV_ACT_NONE);
    assert(zb_interview_step(&iv, 400 + ZB_IV_TIMEOUT_S) == ZB_IV_ACT_STORE);
    assert(iv.state == ZB_IV_FAILED);
    assert(iv.dev.interviewed == 0);        /* joined, NOT interviewed */

    printf("test_zb_interview: OK\n");
    return 0;
}
