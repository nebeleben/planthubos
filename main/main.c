#include <stdbool.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_littlefs.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "planthub";

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

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
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

    /* timekeeper_init runs regardless of storage_ok: its boot counter lives
     * in NVS (a separate partition, unaffected by a littlefs failure) and
     * boottab_load already treats a missing/unreachable table file as an
     * empty table, so on an unmounted /storage it just stays permanently
     * unsynced (timekeeper_synced() == false) instead of failing outright. */
    esp_err_t tk_err = timekeeper_init("/storage");
    if (tk_err != ESP_OK) ESP_LOGE(TAG, "timekeeper_init failed (%s); time sync unavailable", esp_err_to_name(tk_err));

    ESP_ERROR_CHECK(data_core_init());

    /* swarm_store_init() loads role + paired-peer state from NVS into RAM;
     * it must run before the role branch below decides how to boot. A
     * device that has never chosen a role reads back SWARM_ROLE_UNSET and
     * falls into the exact M1-M4 path (portal -> wifi -> hub) below,
     * unchanged -- this is the byte-for-byte-with-today requirement for
     * ROLE_UNSET and ROLE_MAIN. */
    ESP_ERROR_CHECK(swarm_store_init());
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
        /* Before wifi starts, so the guard sees the very first AP_START/GOT_IP. */
        ota_rollback_guard_start();
        ESP_ERROR_CHECK(wifi_manager_start());
        if (role != SWARM_ROLE_NODE) {
            /* ROLE_UNSET or ROLE_MAIN: this is today's hub, with
             * swarm_start_main() as the only addition. */
            esp_err_t serr = swarm_start_main();
            if (serr != ESP_OK) ESP_LOGE(TAG, "swarm (main) start failed (%s)", esp_err_to_name(serr));
            esp_err_t ierr = integrations_start();
            if (ierr != ESP_OK) ESP_LOGE(TAG, "integrations start failed (%s); continuing without them", esp_err_to_name(ierr));
        }
        /* role == NODE only reaches here when swarm_store_pair_failed() is
         * true: the portal is shown so the user can see what happened and
         * retry via POST /api/v1/pair/retry. Do not sweep in this state --
         * the portal owns the radio/channel. */
    }

    esp_err_t ble_err = ble_collector_start();
    if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE collector failed to start (%s); running without BLE", esp_err_to_name(ble_err));

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
            ESP_LOGE(TAG, "failed to create batt_cycle task; node will run always-on "
                          "despite its configured battery mode");
        }
    }

    /* Sampler appends to /storage every CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN
     * minutes; starting it against a failed mount would just fail forever,
     * so skip it outright rather than let it retry into the void. Also
     * hub-only: a node keeps no local history to sample. */
    if (storage_ok && role != SWARM_ROLE_NODE) {
        esp_err_t sampler_err = sampler_start("/storage");
        if (sampler_err != ESP_OK) ESP_LOGE(TAG, "sampler_start failed (%s); running without history sampling", esp_err_to_name(sampler_err));
    } else if (!storage_ok) {
        ESP_LOGW(TAG, "skipping sampler_start: storage unavailable");
    }
}
