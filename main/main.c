#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "webserver.h"
#include "data_core.h"
#include "ble_collector.h"
#include "timekeeper.h"
#include "sampler.h"

static const char *TAG = "planthub";

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
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

    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&fs_conf));
    ESP_ERROR_CHECK(timekeeper_init("/storage"));

    ESP_ERROR_CHECK(data_core_init());
    ESP_ERROR_CHECK(webserver_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    esp_err_t ble_err = ble_collector_start();
    if (ble_err != ESP_OK) ESP_LOGE(TAG, "BLE collector failed to start (%s); running without BLE", esp_err_to_name(ble_err));
    ESP_ERROR_CHECK(sampler_start("/storage"));
}
