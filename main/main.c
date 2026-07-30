#include "esp_log.h"
#include "app_config.h"

static const char *TAG = "planthub";

void app_main(void)
{
    ESP_ERROR_CHECK(app_config_init());
    char name[16];
    app_config_hub_name(name);
    ESP_LOGI(TAG, "PlantHub booting as %s", name);
}
