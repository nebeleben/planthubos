#include <stdbool.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "app_config.h"
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

static const char *TAG = "planthub";

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
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
    bool node_ready = (role == SWARM_ROLE_NODE) && swarm_store_hub(NULL, NULL, NULL);

    if (node_ready) {
        /* Node, already paired to a hub: no web server, no storage sampler,
         * no STA/AP management -- radio-only wifi so ESP-NOW can run, plus
         * BLE collection (started below, common to both roles). */
        esp_err_t nerr = swarm_start_node();
        if (nerr != ESP_OK) ESP_LOGE(TAG, "node start failed (%s)", esp_err_to_name(nerr));
    } else {
        ESP_ERROR_CHECK(webserver_start());
        /* Before wifi starts, so the guard sees the very first AP_START/GOT_IP. */
        ota_rollback_guard_start();
        ESP_ERROR_CHECK(wifi_manager_start());
        if (role != SWARM_ROLE_NODE) {
            /* ROLE_UNSET or ROLE_MAIN: this is today's hub, with
             * swarm_start_main() as the only addition. An unpaired
             * ROLE_NODE falls into this same portal branch above (so the
             * user can see status and retry pairing) but must not itself
             * act as a hub, hence the guard here. */
            esp_err_t serr = swarm_start_main();
            if (serr != ESP_OK) ESP_LOGE(TAG, "swarm (main) start failed (%s)", esp_err_to_name(serr));
        }
    }

    esp_err_t ble_err = ble_collector_start();
    if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE collector failed to start (%s); running without BLE", esp_err_to_name(ble_err));

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
