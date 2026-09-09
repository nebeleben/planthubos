/* Task 5 (zigbee-button-support): rules_resolver.c's event-cap branch
 * (resolve_event_cap(), factored out of resolve_device() for exactly this
 * reason) resolves button.action to its pending code while a press is
 * pending, and to not-ready once it's consumed -- exercised here through
 * resolve_button_test(), a HOST_TEST-only shim in rules_resolver.c that
 * calls that SAME branch directly (see its own doc comment there for why a
 * plain host harness can't drive this through rules_resolve()/
 * resolve_device() end to end: there's no host-side "fake a zigbee join").
 *
 * This test binary links rules_resolver.c itself (compiled -DHOST_TEST), so
 * the linker needs every symbol rules_resolver.c's OTHER functions
 * reference too, even though this test never calls them: psvm_get_ref()/
 * psvm_get_str() (psvm.c, which also pulls in action.c), registry_find()/
 * data_core_snapshot()/data_core_events_*() (registry.c/data_core.c),
 * capability_get()/capability_decode()/device_id_parse() (capability.c/
 * device_id.c) -- all real, all already host-linked elsewhere in this
 * suite. The two remaining callees -- plants_snapshot()/plants_cap_value()/
 * plants_table_action_slot() (plants.c) and app_config_get_sensor_name()
 * (app_config.c) -- pull in NVS/esp_mac/storage.c/action.c-shaped machinery
 * this test has no use for (resolve_event_cap() never calls the
 * plant-ref or app_config-backed device-name path at all), so they're
 * stubbed below instead of linking the real files. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "data_core.h"
#include "capability.h"
#include "registry.h"
#include "plants_table.h"

/* HOST_TEST-only shim, defined in rules_resolver.c. */
int resolve_button_test(const device_id_t *id);

/* ---- link stubs: see the file-header comment above. ---- */

void plants_snapshot(plants_table_t *out)
{
    memset(out, 0, sizeof(*out));
}

bool plants_cap_value(uint8_t plant_id, uint8_t cap_id, const registry_t *reg,
                      float *value_out, uint32_t *age_s_out)
{
    (void)plant_id; (void)cap_id; (void)reg; (void)value_out; (void)age_s_out;
    return false;
}

int plants_table_action_slot(const plants_table_t *t, uint8_t plant_id, uint8_t action_id)
{
    (void)t; (void)plant_id; (void)action_id;
    return -1;
}

bool app_config_get_sensor_name(const uint8_t mac[6], char out[33])
{
    (void)mac; (void)out;
    return false;
}

int main(void)
{
    data_core_init();
    device_id_t b = { .kind = 2 /* DEV_KIND_ZIGBEE */ };
    b.addr[0] = 0xAB;

    /* no press pending -> not ready */
    assert(resolve_button_test(&b) == 0);

    /* a pending press resolves ready, with code 1 */
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 1));
    assert(resolve_button_test(&b) == 1);

    /* still resolves ready on a SECOND read -- resolving never consumes on
     * its own (that's the engine's job, once per value-update pass). */
    assert(resolve_button_test(&b) == 1);

    /* consume as the engine would at the end of a value-update pass ->
     * re-armed (not ready again) */
    data_core_events_consume_through(data_core_events_peek_seq());
    assert(resolve_button_test(&b) == 0);

    /* a press arriving after that pass's high-water mark survives a
     * consume_through() bounded at its earlier seq (mid-pass press, next pass sees it) */
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 1));
    uint32_t seq_first = data_core_events_peek_seq();
    assert(data_core_submit_event(&b, CAP_BUTTON_ACTION, 2));
    int16_t code_after_consume;
    data_core_events_consume_through(seq_first);   /* consume through the earlier press's seq */
    /* the earlier press (code 1, seq_first) is consumed; the later one (code 2, seq > seq_first)
     * survives; data_core_events_for() returns the most-recent, so code is 2 */
    assert(data_core_events_for(&b, CAP_BUTTON_ACTION, &code_after_consume));
    assert(code_after_consume == 2);

    printf("test_rules_events: OK\n");
    return 0;
}
