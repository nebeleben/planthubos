#include <stdbool.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "integr_config.h"
#include "claim.h"
#include "wifi_manager.h"
#include "webserver.h"
#include "data_core.h"
#include "ble_collector.h"
#include "timekeeper.h"
#include "sampler.h"
#include "ota_post.h"
#include "swarm.h"
#include "swarm_store.h"
#include "integrations.h"
#include "plants.h"
#include "event_log.h"
#include "rules.h"
#include "sse.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "planthub";

/* mqtt_pub.c (components/integrations) has no header of its own -- its file
 * header explains why it, alone, owns every esp_mqtt_client_publish() call,
 * and mqtt_pub_event() (Task 6) is its one additional entry point beyond
 * integrations_start(). Declared extern here rather than growing
 * integrations.h for this single call site (on_event_logged() below, its
 * only caller). */
extern void mqtt_pub_event(const char *json);

/* event_log hook (spec §5/§6): event_log_append() invokes this on every
 * append. Builds the {"ts":...,"rule":"...","level":"log"|"notify",
 * "msg":"..."} payload ONCE via cJSON (which escapes rule/msg for us) and
 * hands the same buffer to both the SSE event feed and the MQTT event
 * topic -- that's why event_log_set_hooks() below passes this as the SOLE
 * hook (its mqtt_hook argument is NULL): building the JSON independently
 * in two separate hook callbacks would be wasted, duplicate work for every
 * single event. Rule name comes from rules_list() keyed by the event's
 * rule_id; a rule deleted after logging (or, defensively, rule_id 0) falls
 * back to "rule <id>" rather than dropping the push. */
static void on_event_logged(const event_t *e)
{
    rule_info_t infos[RULES_MAX];
    size_t n = rules_list(infos, RULES_MAX);
    char name[RULES_NAME_MAX + 1];
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (infos[i].id == e->rule_id) {
            strlcpy(name, infos[i].name, sizeof(name));
            found = true;
            break;
        }
    }
    if (!found) snprintf(name, sizeof(name), "rule %u", (unsigned)e->rule_id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "ts", (double)e->ts);
    cJSON_AddStringToObject(root, "rule", name);
    cJSON_AddStringToObject(root, "level", e->level == 1 ? "notify" : "log");
    cJSON_AddStringToObject(root, "msg", e->msg);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    sse_push_event(json);
    mqtt_pub_event(json);
    free(json);
}

/* M7 Task 5: wrapper task for swarm_node_battery_cycle() (spec §4). That
 * function either deep-sleeps (esp_deep_sleep() never returns) or returns
 * ESP_OK meaning "continue as always-on" -- app_main() itself must still
 * return promptly (it always has), so the cycle cannot run inline on
 * app_main's own task/stack. Instead it runs here, on its own dedicated
 * task created just below, in the node-paired branch, only when
 * swarm_store_power_mode() != SWARM_PM_ALWAYS_ON. 4096 bytes, not the
 * default 2048: swarm_node_battery_cycle() calls into espnow_link_send()
 * and the BLE-adjacent scan/forward machinery already running on other
 * tasks is live throughout, so the smaller default stack is not enough
 * headroom here. A return from swarm_node_battery_cycle() means "fall
 * through to today's always-on behaviour" -- for a node, that behaviour is
 * simply everything swarm_start_node()/ble_collector_start() already
 * started continuing to run untouched, so this task has nothing further to
 * do but delete itself. */
static void batt_cycle_task(void *arg)
{
    (void)arg;
    swarm_node_battery_cycle();
    vTaskDelete(NULL);
}

/* Boot-time heap trace (M2 hardware hotfix round 3): permanent, not debug
 * litter -- a memory-constrained device's boot sequence is exactly where a
 * milestone-by-milestone heap trace earns its keep. `largest_free_block` is
 * the number that actually gates whether a later multi-KB-or-contiguous
 * allocation (an xTaskCreate, a wifi rx buffer, ...) can succeed --
 * esp_get_free_heap_size() alone can look healthy while fragmentation
 * blocks it (see sampler.c's log_heap_diag(), added for the same reason
 * one boot-log level down). Called at every major init milestone along the
 * boot path that actually reaches sampler_start() below (hub role,
 * storage_ok) -- see each call site's own placement. */
static void log_heap(const char *milestone)
{
    ESP_LOGI(TAG, "heap @ %s: free=%u B largest_free_block(8BIT|INTERNAL)=%u B",
             milestone, (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
}

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
    /* Before anything slow: the rapid power-cycle counter must increment
     * within the first moments of a boot (see power_reset.c). */
    power_cycle_reset_start();
    ESP_ERROR_CHECK(integr_config_init());
    ESP_ERROR_CHECK(claim_init());
    factory_reset_button_start();
    char name[16];
    app_config_hub_name(name);
    ESP_LOGI(TAG, "PlantHub booting as %s", name);

    /* netif + the default event loop must exist before webserver_start()
     * registers its WIFI_EVENT_AP_START/AP_STOP handler, and that handler
     * must in turn be registered before wifi_manager_start() ever calls
     * esp_wifi_start() -- otherwise a first-boot-into-AP WIFI_EVENT_AP_START
     * could fire with no listener and the captive portal's DNS hijack would
     * never start. Hence: netif/event-loop, then webserver, then wifi.
     *
     * data_core_init() only needs the default event loop (it posts
     * PLANTHUB_DATA_EVENT on it) so it slots in right after the loop is
     * created and before webserver/wifi come up. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* A storage/NVS failure here must not panic-reboot-loop the hub: that
     * would brick WiFi onboarding (M1's core function) over a feature (M3
     * history) that can degrade gracefully instead. So littlefs, timekeeper
     * and sampler all get the same log-and-continue treatment as BLE below,
     * rather than ESP_ERROR_CHECK. */
    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t storage_err = esp_vfs_littlefs_register(&fs_conf);
    bool storage_ok = storage_err == ESP_OK;
    if (!storage_ok) ESP_LOGE(TAG, "littlefs mount failed (%s); running without on-device history", esp_err_to_name(storage_err));
    log_heap("after littlefs mount");

    /* M2 Task 5 (spec Sec.5): one-time V1->V2 data wipe, gated on NVS
     * "data_fmt". Must run before ANYTHING that reads plants.bin or a
     * history ring file (plants_init()/sampler_start() below both qualify),
     * so this is the very next thing once NVS (app_config_init() above) and
     * littlefs are both up -- earlier than even timekeeper_init(), which
     * doesn't touch plants/history data but there is no reason to let it
     * run first either. data_core_init() just below is deliberately NOT
     * gated on data_fmt_safe(): it's an in-RAM pub/sub event registry with
     * nothing persisted in plants.bin/ring format, so a downgraded
     * firmware's refusal to touch data (see data_fmt_safe() below) doesn't
     * apply to it. A NULL base (storage_ok false) makes data_fmt_apply()
     * itself a no-op that retries on a later boot once storage mounts --
     * same log-and-continue treatment as every other littlefs-dependent
     * init in this function. */
    data_fmt_apply(storage_ok ? "/storage" : NULL);
    log_heap("after data_fmt_apply");

    /* timekeeper_init runs regardless of storage_ok: its boot counter lives
     * in NVS (a separate partition, unaffected by a littlefs failure) and
     * boottab_load already treats a missing/unreachable table file as an
     * empty table, so on an unmounted /storage it just stays permanently
     * unsynced (timekeeper_synced() == false) instead of failing outright. */
    esp_err_t tk_err = timekeeper_init("/storage");
    if (tk_err != ESP_OK) ESP_LOGE(TAG, "timekeeper_init failed (%s); time sync unavailable", esp_err_to_name(tk_err));

    ESP_ERROR_CHECK(data_core_init());
    log_heap("after data_core_init");

    /* swarm_store_init() loads role + paired-peer state from NVS into RAM;
     * it must run before the role branch below decides how to boot. A
     * device that has never chosen a role reads back SWARM_ROLE_UNSET and
     * falls into the exact M1-M4 path (portal -> wifi -> hub) below,
     * unchanged -- this is the byte-for-byte-with-today requirement for
     * ROLE_UNSET and ROLE_MAIN. */
    ESP_ERROR_CHECK(swarm_store_init());
    /* Second half of the rapid power-cycle reset (power_reset.c): a node
     * has no WiFi credentials, so the wifi clear alone is invisible there
     * -- its "back to onboarding" is clearing role + pairing, which needs
     * swarm_store and therefore happens here, not in app_config. Must run
     * before the role branch below reads the role. Hubs keep the
     * documented wifi-only semantics (pairings survive). */
    if (power_cycle_reset_triggered() && swarm_store_role() == SWARM_ROLE_NODE) {
        ESP_LOGW(TAG, "power-cycle reset on a node: clearing role and pairing");
        swarm_store_reset_all();
    }
    swarm_role_t role = swarm_store_role();
    bool node_paired = (role == SWARM_ROLE_NODE) && swarm_store_hub(NULL, NULL, NULL);
    /* Unpaired node, no pairing failure on record yet: actively search for
     * a hub instead of sitting in a portal nobody asked to see -- this is
     * the reconciliation between task-5 ("unpaired node runs the portal")
     * and task-6 ("choosing node sweeps for a hub"): search first, and
     * only fall back to the portal once a search has actually failed. */
    bool node_should_search = (role == SWARM_ROLE_NODE) && !node_paired && !swarm_store_pair_failed();
    /* M7 Task 5: separate from node_paired -- that stays true even when
     * swarm_start_node() below fails and this boot falls back to the
     * portal instead, but swarm_node_battery_cycle()'s precondition is
     * specifically "swarm_start_node() has succeeded". Gating the
     * battery-cycle task on node_paired alone would create it against a
     * node that never actually started its ESP-NOW/forward machinery. */
    bool node_started = false;

    if (node_paired) {
        /* Node, already paired to a hub: no web server, no storage sampler,
         * no STA/AP management -- radio-only wifi so ESP-NOW can run, plus
         * BLE collection (started below, common to both roles).
         *
         * Rollback guard (M5c, critical -- see ota_post.h): a paired node
         * has neither of ota_rollback_guard_start()'s two events (no AP, no
         * IP) to confirm an OTA'd image on, so without this a node updated
         * over the air would boot in PENDING_VERIFY, never confirm, and
         * silently roll back to its old firmware on its very next reboot --
         * every node update would appear to succeed and then quietly undo
         * itself. Both calls below must run BEFORE swarm_start_node()
         * starts forward_task()/the receive callback, so nothing can prove
         * this node "healthy" before the guard is armed and a callback is
         * wired to receive that signal. */
        ota_rollback_guard_start_node();
        swarm_node_set_health_cb(ota_rollback_guard_node_confirm);
        esp_err_t nerr = swarm_start_node();
        node_started = (nerr == ESP_OK);
        if (nerr != ESP_OK) {
            /* Without this fallback a device that fails here is completely
             * inert: no webserver, no wifi_manager, no ESP-NOW -- silent
             * and unreachable except via the 10s factory-reset button.
             * Same portal fallback as the search branch just below. */
            ESP_LOGE(TAG, "node start failed (%s); falling back to the portal so the user can recover",
                     esp_err_to_name(nerr));
            ESP_ERROR_CHECK(webserver_start());
            ota_rollback_guard_start();
            ESP_ERROR_CHECK(wifi_manager_start());
        }
    } else if (node_should_search) {
        /* Radio-only, same as the paired-node branch above -- no
         * webserver/wifi_manager while actively sweeping, since pairing
         * needs to hop channels freely and a portal would fight it for the
         * radio. A watcher task (started inside) reboots this device once
         * the search resolves, landing either in the paired branch above
         * (success) or the portal branch below (failure, via the
         * persisted pair-failed flag). */
        esp_err_t serr = swarm_start_node_search();
        if (serr != ESP_OK) {
            ESP_LOGE(TAG, "node search failed to start (%s); falling back to the portal", esp_err_to_name(serr));
            ESP_ERROR_CHECK(webserver_start());
            ota_rollback_guard_start();
            ESP_ERROR_CHECK(wifi_manager_start());
        }
    } else {
        ESP_ERROR_CHECK(webserver_start());
        log_heap("after webserver_start");
        /* Before wifi starts, so the guard sees the very first AP_START/GOT_IP. */
        ota_rollback_guard_start();
        ESP_ERROR_CHECK(wifi_manager_start());
        log_heap("after wifi_manager_start");
        if (role != SWARM_ROLE_NODE) {
            /* ROLE_UNSET or ROLE_MAIN: this is today's hub, with
             * swarm_start_main() as the only addition. */
            esp_err_t serr = swarm_start_main();
            if (serr != ESP_OK) ESP_LOGE(TAG, "swarm (main) start failed (%s)", esp_err_to_name(serr));

            /* M8 Task 2: hub-only plant registry, NVS-backed -- see
             * plants.h. Hub role only (nodes keep no plants), so this lives
             * inside the role != NODE branch rather than up at the
             * littlefs/data_core block above. storage_base mirrors
             * sampler_start()'s own storage_ok gating just below: NULL when
             * the littlefs mount failed, so plants_delete()'s ring-file
             * cleanup degrades to a no-op instead of failing. Never fails
             * boot on its own account (see plants.h) -- log-and-continue,
             * same treatment as littlefs/timekeeper above, not
             * ESP_ERROR_CHECK. */
            /* Skipped outright on a FUTURE data_fmt (spec Sec.5 / Task 5):
             * data_fmt_apply() above already refused to wipe or touch
             * anything in that case (a downgraded firmware must not
             * interpret a newer plants.bin format), so plants_init() must
             * not open it either -- there is nothing safe for it to read. */
            if (data_fmt_safe()) {
                esp_err_t perr = plants_init(storage_ok ? "/storage" : NULL);
                if (perr != ESP_OK) ESP_LOGE(TAG, "plants_init failed (%s); plant registry unavailable", esp_err_to_name(perr));
            } else {
                ESP_LOGE(TAG, "data_fmt: on-disk format is newer than this firmware supports; skipping plants_init");
            }
            log_heap("after plants_init");

            esp_err_t ierr = integrations_start();
            if (ierr != ESP_OK) ESP_LOGE(TAG, "integrations start failed (%s); continuing without them", esp_err_to_name(ierr));

            /* M1 rules engine (spec §4): hub-only, same as plants_init()
             * just above -- a rule's plant() refs need the plant table,
             * which nodes never load. event_log_init() must run before
             * rules_init() starts the engine task: rules_engine.c's real
             * firing path calls event_log_append() on every fire, and that
             * function dereferences event_log's static ring, which stays
             * NULL/zeroed (and would crash on first use) until
             * event_log_init() has loaded it -- see event_log.c. Neither
             * call can fail boot (see their own headers); both are
             * log-and-continue like every other storage-adjacent init here.
             * Note: this lands after webserver_start()/wifi_manager_start()
             * above, not before as an earlier draft of this ordering
             * assumed -- those two must stay ahead of swarm_start_main() for
             * the AP_START/portal-DNS reasons documented at their own call
             * sites, and no rules HTTP route exists yet for this task's
             * ordering to protect (Task 6 wires the API). What matters here
             * is only that both run after plants_init()/data_core_init(),
             * which they do. */
            event_log_init();
            /* Before rules_init() starts the engine task, so no event fired
             * in the window right after boot can slip through with the SSE/
             * MQTT hooks still unset (see on_event_logged()'s own comment
             * above -- a NULL hook is harmless either way, event_log.c
             * checks before calling, but there's no reason to leave the gap
             * open at all). */
            event_log_set_hooks(on_event_logged, NULL);
            rules_init();
        }
        /* role == NODE only reaches here when swarm_store_pair_failed() is
         * true: the portal is shown so the user can see what happened and
         * retry via POST /api/v1/pair/retry. Do not sweep in this state --
         * the portal owns the radio/channel. */
    }

    esp_err_t ble_err = ble_collector_start();
    if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE collector failed to start (%s); running without BLE", esp_err_to_name(ble_err));
    log_heap("after swarm/BLE bring-up");

    /* M7 Task 5 (spec §4): a battery-mode paired node runs its wake cycle
     * (scan -> checkin -> sleep) instead of just sitting always-on -- see
     * batt_cycle_task()'s own comment above for why this needs its own
     * task. Added here, after ble_collector_start() (not earlier in the
     * node_paired branch above), so BLE scanning is already live before the
     * cycle's scan window starts consuming it. Gated on node_started, not
     * node_paired -- see node_started's own comment above: only a node
     * whose swarm_start_node() actually succeeded satisfies
     * swarm_node_battery_cycle()'s precondition. */
    if (node_started && swarm_store_power_mode() != SWARM_PM_ALWAYS_ON) {
        if (xTaskCreate(batt_cycle_task, "batt_cycle", 4096, NULL, 2, NULL) != pdPASS) {
            /* Code review fix (issue 5): without this, the node keeps
             * running (nothing else here fails), but sends no CHECKIN at
             * all -- swarm.c's always_on_checkin_task() gates its own
             * periodic checkin on power_mode == ALWAYS_ON, which the stored
             * mode here still isn't, so it stays silent too. That would
             * leave this node unmanageable (no way for the hub to ever
             * reach it with a mode change) until someone physically
             * recovers it. Persisting ALWAYS_ON here makes the stored mode
             * match what the node is actually doing (running always-on,
             * task-creation failure notwithstanding), so the always-on
             * task's gate now matches reality and starts checking in --
             * giving the hub a path to re-deliver a battery mode later. */
            ESP_LOGE(TAG, "failed to create batt_cycle task; node will run always-on "
                          "despite its configured battery mode -- persisting ALWAYS_ON "
                          "so it still sends periodic checkins");
            esp_err_t perr = swarm_store_set_power_mode(SWARM_PM_ALWAYS_ON);
            if (perr != ESP_OK) {
                ESP_LOGE(TAG, "also failed to persist the ALWAYS_ON fallback (%s); this node may be "
                              "unreachable until physically recovered", esp_err_to_name(perr));
            }
        }
    }

    /* Sampler appends to /storage every CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN
     * minutes; starting it against a failed mount would just fail forever,
     * so skip it outright rather than let it retry into the void. Also
     * hub-only: a node keeps no local history to sample. */
    if (storage_ok && role != SWARM_ROLE_NODE && data_fmt_safe()) {
        log_heap("before sampler_start");
        esp_err_t sampler_err = sampler_start("/storage");
        if (sampler_err != ESP_OK) ESP_LOGE(TAG, "sampler_start failed (%s); running without history sampling", esp_err_to_name(sampler_err));
    } else if (!storage_ok) {
        ESP_LOGW(TAG, "skipping sampler_start: storage unavailable");
    } else if (storage_ok && role != SWARM_ROLE_NODE) {
        ESP_LOGE(TAG, "data_fmt: on-disk format is newer than this firmware supports; skipping sampler_start");
    }
}
