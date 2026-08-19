/* zb_interview.h -- the interview, as a pure state machine (M6b spec
 * section 5).
 *
 * Same shape as M5b's pending_close_step(): the caller hands in the state
 * and the current time, and gets back what to do next. Every branch --
 * including the timeout and the unmappable device -- is therefore reachable
 * from tests/host/test_zb_interview.c with no radio in the room.
 *
 * A failed or unmappable interview still produces a stored device. A
 * device joined to the network but absent from the UI is the orphan case
 * spec section 4 exists to prevent, and an unmappable device is precisely
 * the evidence M6c is built from.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "zb_store.h"

#define ZB_IV_TIMEOUT_S   30
#define ZB_IV_MAX_ENDPOINTS 4
#define ZB_IV_MAX_CLUSTERS  16

typedef enum {
    ZB_IV_IDLE = 0,
    ZB_IV_WAIT_ACTIVE_EP,
    ZB_IV_WAIT_SIMPLE_DESC,
    ZB_IV_WAIT_CONFIG_REPORT,
    ZB_IV_DONE,
    ZB_IV_FAILED,
} zb_iv_state_t;

typedef enum {
    ZB_IV_ACT_NONE = 0,
    ZB_IV_ACT_SEND_ACTIVE_EP,
    ZB_IV_ACT_SEND_SIMPLE_DESC,
    ZB_IV_ACT_SEND_CONFIG_REPORT,
    ZB_IV_ACT_STORE,
} zb_iv_action_t;

typedef struct {
    zb_iv_state_t state;
    uint32_t      deadline_s;
    bool          request_sent;         /* stops a re-send while waiting */
    bool          progressed;           /* an on_* callback fired since the
                                          * last step(); only step() knows
                                          * now_s, so it -- not the callback
                                          * -- recomputes deadline_s from it */
    uint8_t       endpoints[ZB_IV_MAX_ENDPOINTS];
    uint8_t       endpoint_count;
    uint8_t       endpoint_cursor;
    uint8_t       pending_endpoint;     /* the endpoint SEND_SIMPLE_DESC asks about */
    uint16_t      report_clusters[ZB_STORE_MAX_CAPS];
    /* Whole-branch review, FIX 4: the endpoint EACH report_clusters[]
     * entry was found on -- report_clusters accumulates across every
     * endpoint the interview walks, but Configure Reporting is a
     * per-endpoint request, so dev.endpoint (the FIRST endpoint that
     * yielded ANY mapping, on_clusters() below) is only sometimes the
     * right destination. A device with On/Off on endpoint 1 and
     * temperature on endpoint 2 must have its temperature report
     * addressed to endpoint 2, not to dev.endpoint's 1. */
    uint8_t       report_endpoints[ZB_STORE_MAX_CAPS];
    uint8_t       report_count;
    uint8_t       report_cursor;
    zb_device_t   dev;                  /* built up as answers arrive */
} zb_iv_t;

/* Starts an interview for a freshly joined device. */
void zb_interview_begin(zb_iv_t *iv, const uint8_t eui64[8],
                        uint16_t short_addr, uint32_t now_s);

/* What to do next. Call on a tick and after every on_* callback. */
zb_iv_action_t zb_interview_step(zb_iv_t *iv, uint32_t now_s);

/* Active-endpoint response. */
void zb_interview_on_endpoints(zb_iv_t *iv, const uint8_t *eps, uint8_t n);

/* Simple-descriptor response for one endpoint: runs the auto-map. */
void zb_interview_on_clusters(zb_iv_t *iv, uint8_t endpoint,
                              const uint16_t *clusters, uint8_t n);
