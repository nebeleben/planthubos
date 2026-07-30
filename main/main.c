#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "webserver.h"

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
     * never start. Hence: netif/event-loop, then webserver, then wifi. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(webserver_start());
    ESP_ERROR_CHECK(wifi_manager_start());
}
