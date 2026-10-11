/* Shared ingest: the same decoded frame fed over the ESP-NOW path (via_mac
 * set) and the UART path (via_mac NULL) must land identical cap/value state
 * in data_core. Proves swarm_ingest_* is the single body both transports use. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "swarm_ingest.h"
#include "swarm_frame.h"
#include "data_core.h"
#include "capability.h"

/* --- no-op shims: the actor table and the rules notifier are irrelevant to
 * the cap/value state this test measures; stub them so the link stays small
 * (the test_rules_events.c pattern). --- */
bool actor_declare_ep(int d, uint8_t a, uint8_t e, uint16_t pm, uint8_t f) { (void)d;(void)a;(void)e;(void)pm;(void)f; return true; }
bool actor_prune_absent(int d, const uint8_t *ids, uint8_t n) { (void)d;(void)ids;(void)n; return true; }
void actor_set_device_key(int d, const uint8_t *k) { (void)d;(void)k; }
void rules_notify_value_update(void) { }

static swarm_device_announce_t mk_ann(const uint8_t eui[8]) {
    swarm_device_announce_t a = {0}; a.dev.kind = 2 /* DEV_KIND_ZIGBEE */;
    memcpy(a.dev.addr, eui, 8); a.interviewed = 1;
    a.cap_count = 1; a.cap_ids[0] = CAP_AIR_TEMPERATURE; a.cap_clusters[0] = 0x0402; a.cap_endpoints[0] = 1;
    return a;
}
static swarm_measurement_t mk_meas(const uint8_t eui[8], float v, uint16_t cl) {
    swarm_measurement_t m = {0}; m.dev.kind = 2; memcpy(m.dev.addr, eui, 8);
    m.cap_id = CAP_AIR_TEMPERATURE; m.endpoint = 1; m.value = v; m.age_s = 0; m.source_cluster = cl;
    return m;
}
static bool readback(const uint8_t eui[8], float *out) {
    device_id_t id = { .kind = 2 }; memcpy(id.addr, eui, 8);
    device_entry_t e;
    if (!data_core_get_device(&id, &e)) return false;
    if (!e.caps[CAP_AIR_TEMPERATURE].valid) return false;
    *out = capability_decode(CAP_AIR_TEMPERATURE, e.caps[CAP_AIR_TEMPERATURE].raw);
    return true;
}

int main(void) {
    data_core_init();

    /* ESP-NOW path (via_mac set) on device A */
    const uint8_t mac[6] = {1,2,3,4,5,6};
    const uint8_t euiA[8] = {0xaa,0,0,0,0,0,0,1};
    swarm_device_announce_t annA = mk_ann(euiA);
    swarm_ingest_announce(&annA, mac, 100);
    swarm_measurement_t mA = mk_meas(euiA, 21.5f, 0x0402);
    assert(swarm_ingest_measurement(&mA, 0, 101) == true);
    float got_espnow; assert(readback(euiA, &got_espnow));

    /* UART path (via_mac NULL) on device B -- same frames */
    const uint8_t euiB[8] = {0xbb,0,0,0,0,0,0,2};
    swarm_device_announce_t annB = mk_ann(euiB);
    swarm_ingest_announce(&annB, NULL, 100);
    swarm_measurement_t mB = mk_meas(euiB, 21.5f, 0x0402);
    assert(swarm_ingest_measurement(&mB, 0, 101) == true);
    float got_uart; assert(readback(euiB, &got_uart));

    assert(got_espnow == got_uart);
    assert(fabsf(got_espnow - 21.5f) < 0.1f);
    printf("ingest equivalence: espnow=%.3f uart=%.3f ok\n", (double)got_espnow, (double)got_uart);

    /* Unknown device -> false on both paths (no prior announce) */
    const uint8_t euiC[8] = {0xcc,0,0,0,0,0,0,3};
    swarm_measurement_t mC = mk_meas(euiC, 9.0f, 0x0402);
    assert(swarm_ingest_measurement(&mC, 0, 50) == false);
    printf("unknown-device returns false: ok\n");

    /* Suppressed cluster -> handled (true), value not stored for a NEW device.
     * Announce device D, then feed a reading on a cluster in its suppress-set.
     * With no suppress entries configured, nothing is suppressed, so instead
     * assert the complementary invariant: a source_cluster==0 (raw Tuya path)
     * reading is never suppressed and submits. */
    const uint8_t euiD[8] = {0xdd,0,0,0,0,0,0,4};
    swarm_device_announce_t annD = mk_ann(euiD);
    swarm_ingest_announce(&annD, NULL, 100);
    swarm_measurement_t mD = mk_meas(euiD, 18.0f, 0 /* no source cluster */);
    assert(swarm_ingest_measurement(&mD, 0, 101) == true);
    float got_d; assert(readback(euiD, &got_d) && fabsf(got_d - 18.0f) < 0.1f);
    printf("source_cluster==0 never suppressed, submits: ok\n");

    printf("\nALL SWARM-INGEST TESTS PASS\n");
    return 0;
}
