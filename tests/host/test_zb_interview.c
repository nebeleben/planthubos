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
    /* Task 13: the undrivable cluster is retained, not discarded -- this
     * is M6c's input. */
    assert(iv.dev.unmapped_count == 1);
    assert(iv.dev.unmapped_clusters[0] == 0xEF00);

    /* --- a silent device times out and is still stored --- */
    zb_interview_begin(&iv, EUI, 0x1234, 400);
    assert(zb_interview_step(&iv, 400) == ZB_IV_ACT_SEND_ACTIVE_EP);
    assert(zb_interview_step(&iv, 400 + ZB_IV_TIMEOUT_S - 1) == ZB_IV_ACT_NONE);
    assert(zb_interview_step(&iv, 400 + ZB_IV_TIMEOUT_S) == ZB_IV_ACT_STORE);
    assert(iv.state == ZB_IV_FAILED);
    assert(iv.dev.interviewed == 0);        /* joined, NOT interviewed */

    /* --- fix round 1: a burst of callbacks at ONE instant must extend the
     * deadline once, not once per callback. Four endpoints answering back
     * to back at t=1000 must leave the deadline at exactly 1000 + TIMEOUT,
     * never a multiple of it. --- */
    zb_interview_begin(&iv, EUI, 0x1234, 1000);
    uint8_t eps4[4] = { 1, 2, 3, 4 };
    uint16_t temp[1] = { 0x0402 };
    zb_interview_on_endpoints(&iv, eps4, 4);           /* callback 1 */
    zb_interview_on_clusters(&iv, 1, temp, 1);         /* callback 2 */
    zb_interview_on_clusters(&iv, 2, temp, 1);         /* callback 3 */
    zb_interview_on_clusters(&iv, 3, temp, 1);         /* callback 4 */
    zb_interview_on_clusters(&iv, 4, temp, 1);         /* callback 5 */
    /* All five callbacks fired at t=1000; the FIRST step() call after them
     * is what recomputes the deadline, and it must do so from now_s, not
     * by stacking one extension per callback delivered. */
    assert(zb_interview_step(&iv, 1000) == ZB_IV_ACT_SEND_CONFIG_REPORT);
    assert(iv.deadline_s == 1000 + ZB_IV_TIMEOUT_S);

    /* Then it goes silent: no further callback, so the deadline set above
     * must hold -- a device that answered and then stopped still times out
     * exactly on schedule, not minutes late because it was chatty early on. */
    assert(zb_interview_step(&iv, 1000 + ZB_IV_TIMEOUT_S) == ZB_IV_ACT_STORE);
    assert(iv.state == ZB_IV_FAILED);

    /* --- fix round 2: two endpoints reporting the SAME cluster must not
     * double up. The registry keeps one slot per (device, capability id)
     * -- a second CAP_SWITCH_STATE entry for this EUI-64 is not just a
     * wasted array slot, it is unrepresentable downstream and would have
     * silently starved gang 2's actions of a slot gang 1 already took. --- */
    zb_interview_begin(&iv, EUI, 0x1234, 500);
    zb_interview_step(&iv, 500);
    uint8_t eps2[2] = { 1, 2 };
    zb_interview_on_endpoints(&iv, eps2, 2);
    zb_interview_step(&iv, 501);
    uint16_t gang1[1] = { 0x0006 };
    zb_interview_on_clusters(&iv, 1, gang1, 1);
    zb_interview_step(&iv, 502);
    uint16_t gang2[1] = { 0x0006 };
    zb_interview_on_clusters(&iv, 2, gang2, 1);
    zb_interview_step(&iv, 503);                    /* SEND_CONFIG_REPORT */
    assert(zb_interview_step(&iv, 504) == ZB_IV_ACT_STORE);
    assert(iv.dev.cap_count == 1);
    assert(iv.dev.action_count == 2);
    assert(iv.dev.actions[0] == ACT_SWITCH_ON);
    assert(iv.dev.actions[1] == ACT_SWITCH_OFF);

    /* --- the dedup must not be over-broad: two endpoints reporting
     * DIFFERENT clusters keep both capabilities. --- */
    zb_interview_begin(&iv, EUI, 0x1234, 600);
    zb_interview_step(&iv, 600);
    zb_interview_on_endpoints(&iv, eps2, 2);
    zb_interview_step(&iv, 601);
    uint16_t humidity[1] = { 0x0405 };
    zb_interview_on_clusters(&iv, 1, temp, 1);       /* 0x0402, endpoint 1 */
    zb_interview_step(&iv, 602);
    zb_interview_on_clusters(&iv, 2, humidity, 1);   /* 0x0405, endpoint 2 */
    zb_interview_step(&iv, 603);                    /* SEND_CONFIG_REPORT */
    zb_interview_step(&iv, 604);                    /* SEND_CONFIG_REPORT */
    assert(zb_interview_step(&iv, 605) == ZB_IV_ACT_STORE);
    assert(iv.dev.cap_count == 2);
    assert(iv.dev.caps[0] == CAP_AIR_TEMPERATURE);
    assert(iv.dev.caps[1] == CAP_AIR_HUMIDITY);

    /* --- FIX 4 (whole-branch review): report_clusters[] accumulates
     * across every endpoint, but Configure Reporting is per-endpoint --
     * each entry must remember ITS OWN endpoint, not dev.endpoint (which
     * is only the first endpoint to yield any mapping at all, here
     * endpoint 1's temperature). Endpoint 2's humidity report must stay
     * addressed to endpoint 2. --- */
    assert(iv.dev.endpoint == 1);
    assert(iv.report_count == 2);
    assert(iv.report_clusters[0] == 0x0402);   /* temperature, endpoint 1 */
    assert(iv.report_endpoints[0] == 1);
    assert(iv.report_clusters[1] == 0x0405);   /* humidity, endpoint 2 */
    assert(iv.report_endpoints[1] == 2);

    /* --- Task 13: the same undrivable cluster on two endpoints is
     * recorded once, not twice -- same dedup reasoning as caps/actions
     * above, just for unmapped_clusters. --- */
    zb_interview_begin(&iv, EUI, 0x1234, 700);
    zb_interview_step(&iv, 700);
    zb_interview_on_endpoints(&iv, eps2, 2);
    zb_interview_step(&iv, 701);
    uint16_t tuya_ep1[1] = { 0xEF00 };
    zb_interview_on_clusters(&iv, 1, tuya_ep1, 1);
    zb_interview_step(&iv, 702);
    uint16_t tuya_ep2[1] = { 0xEF00 };
    zb_interview_on_clusters(&iv, 2, tuya_ep2, 1);
    assert(zb_interview_step(&iv, 703) == ZB_IV_ACT_STORE);
    assert(iv.dev.unmapped_count == 1);
    assert(iv.dev.unmapped_clusters[0] == 0xEF00);

    /* --- Task 13: the unmapped array bound holds -- more undrivable
     * clusters than ZB_STORE_MAX_UNMAPPED are dropped silently rather
     * than overflowing the array. --- */
    zb_interview_begin(&iv, EUI, 0x1234, 800);
    zb_interview_step(&iv, 800);
    zb_interview_on_endpoints(&iv, eps, 1);
    zb_interview_step(&iv, 801);
    uint16_t many[8] = {
        0xFC00, 0xFC01, 0xFC02, 0xFC03, 0xFC04, 0xFC05, 0xFC06, 0xFC07,
    };
    zb_interview_on_clusters(&iv, 1, many, 8);
    assert(zb_interview_step(&iv, 802) == ZB_IV_ACT_STORE);
    assert(iv.dev.unmapped_count == ZB_STORE_MAX_UNMAPPED);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        assert(iv.dev.unmapped_clusters[i] == (uint16_t)(0xFC00 + i));
    }

    printf("test_zb_interview: OK\n");
    return 0;
}
