/* zb_interview.c -- the interview state machine (M6b spec section 5).
 *
 * See zb_interview.h for the shape and the reasoning. Two things worth
 * calling out here:
 *
 * - Progress earns more time, silence does not -- and only step() knows
 *   how much time has actually passed. Each on_* callback just records
 *   that an answer arrived (iv->progressed); step() is the one that turns
 *   that into deadline_s = now_s + ZB_IV_TIMEOUT_S, before it checks the
 *   deadline. Doing the recompute inside the callback instead -- adding
 *   ZB_IV_TIMEOUT_S onto whatever deadline_s already held -- was tried and
 *   rejected: several callbacks delivered back-to-back at one instant (a
 *   device with four endpoints, say) would then stretch a 30-second budget
 *   into minutes, which is progress earning time by CALL COUNT rather than
 *   by elapsed time. A bare zb_interview_step() call with no callback in
 *   between never sets progressed, so a device that never replies is still
 *   bounded by the original deadline.
 * - A failed or unmappable interview still ends in ZB_IV_ACT_STORE. The
 *   only difference is dev.interviewed: 0 means "joined, never heard from
 *   again", 1 means "answered, even if nothing it said mapped to
 *   anything we understand".
 */
#include "zb_interview.h"
#include "zb_map.h"
#include <string.h>

void zb_interview_begin(zb_iv_t *iv, const uint8_t eui64[8],
                         uint16_t short_addr, uint32_t now_s) {
    memset(iv, 0, sizeof(*iv));
    memcpy(iv->dev.eui64, eui64, 8);
    iv->dev.short_addr = short_addr;
    iv->state = ZB_IV_WAIT_ACTIVE_EP;
    iv->deadline_s = now_s + ZB_IV_TIMEOUT_S;
    iv->request_sent = false;
}

zb_iv_action_t zb_interview_step(zb_iv_t *iv, uint32_t now_s) {
    if (iv->state == ZB_IV_DONE || iv->state == ZB_IV_FAILED)
        return ZB_IV_ACT_NONE;

    /* An answer arrived since the last step(): recompute the deadline from
     * now_s, the only time this function actually has, before checking it
     * -- so an answer that lands right up against the deadline still
     * counts as progress. */
    if (iv->progressed) {
        iv->deadline_s = now_s + ZB_IV_TIMEOUT_S;
        iv->progressed = false;
    }

    /* Silence, not an answer: the device gets stored as joined-but-not-
     * interviewed, never dropped -- see the file header comment. */
    if (now_s >= iv->deadline_s) {
        iv->state = ZB_IV_FAILED;
        iv->dev.interviewed = 0;
        return ZB_IV_ACT_STORE;
    }

    switch (iv->state) {
    case ZB_IV_WAIT_ACTIVE_EP:
        if (iv->request_sent)
            return ZB_IV_ACT_NONE;
        iv->request_sent = true;
        return ZB_IV_ACT_SEND_ACTIVE_EP;

    case ZB_IV_WAIT_SIMPLE_DESC:
        if (iv->endpoint_cursor < iv->endpoint_count) {
            if (iv->request_sent)
                return ZB_IV_ACT_NONE;
            iv->pending_endpoint = iv->endpoints[iv->endpoint_cursor];
            iv->request_sent = true;
            return ZB_IV_ACT_SEND_SIMPLE_DESC;
        }
        /* Every endpoint has answered (or there were none to ask). */
        if (iv->report_count > 0) {
            iv->state = ZB_IV_WAIT_CONFIG_REPORT;
            iv->report_cursor = 1;
            return ZB_IV_ACT_SEND_CONFIG_REPORT;
        }
        iv->dev.interviewed = 1;
        iv->state = ZB_IV_DONE;
        return ZB_IV_ACT_STORE;

    case ZB_IV_WAIT_CONFIG_REPORT:
        if (iv->report_cursor < iv->report_count) {
            iv->report_cursor++;
            return ZB_IV_ACT_SEND_CONFIG_REPORT;
        }
        iv->dev.interviewed = 1;
        iv->state = ZB_IV_DONE;
        return ZB_IV_ACT_STORE;

    default:
        return ZB_IV_ACT_NONE;
    }
}

void zb_interview_on_endpoints(zb_iv_t *iv, const uint8_t *eps, uint8_t n) {
    uint8_t count = n;
    if (count > ZB_IV_MAX_ENDPOINTS)
        count = ZB_IV_MAX_ENDPOINTS;
    for (uint8_t i = 0; i < count; i++)
        iv->endpoints[i] = eps[i];
    iv->endpoint_count = count;
    iv->endpoint_cursor = 0;

    iv->state = ZB_IV_WAIT_SIMPLE_DESC;
    iv->request_sent = false;
    iv->progressed = true;
}

void zb_interview_on_clusters(zb_iv_t *iv, uint8_t endpoint,
                               const uint16_t *clusters, uint8_t n) {
    /* Whether ANY endpoint before this one already produced a mapping --
     * used below to set dev.endpoint to the FIRST one that did. */
    bool had_mapping_before =
        iv->dev.cap_count > 0 || iv->dev.action_count > 0 || iv->report_count > 0;
    bool yielded = false;

    for (uint8_t i = 0; i < n; i++) {
        uint16_t cluster = clusters[i];

        uint8_t cap = zb_map_cluster_to_cap(cluster);
        if (cap != ZB_MAP_NONE && iv->dev.cap_count < ZB_STORE_MAX_CAPS) {
            iv->dev.caps[iv->dev.cap_count] = cap;
            iv->dev.cap_clusters[iv->dev.cap_count] = cluster;
            iv->dev.cap_count++;
            yielded = true;
        }

        if (iv->dev.action_count < ZB_STORE_MAX_ACTIONS) {
            int room = ZB_STORE_MAX_ACTIONS - iv->dev.action_count;
            int added = zb_map_cluster_to_actions(
                cluster, &iv->dev.actions[iv->dev.action_count], room);
            if (added > 0) {
                iv->dev.action_count += (uint8_t)added;
                yielded = true;
            }
        }

        if (zb_map_report_attr(cluster) != ZB_MAP_NO_ATTR &&
            iv->report_count < ZB_STORE_MAX_CAPS) {
            iv->report_clusters[iv->report_count] = cluster;
            iv->report_count++;
            yielded = true;
        }
    }

    if (yielded && !had_mapping_before)
        iv->dev.endpoint = endpoint;

    iv->endpoint_cursor++;
    iv->request_sent = false;
    iv->progressed = true;
}
