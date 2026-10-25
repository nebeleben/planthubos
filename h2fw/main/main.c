/* h2fw -- the ESP32-H2 Zigbee coordinator half of the two-chip PlantHub
 * appliance. It reuses the planthubos `zigbee` component wholesale (the
 * coordinator, interview driver, ZCL handlers, metering sweep, Tuya parse
 * and command engine) and forwards every egress -- device announces, coord
 * status, Tuya DPs and measurements -- to the C6 brain over a framed UART
 * (see components/uart_link). This file only brings up the minimum the
 * coordinator's dependency closure needs: NVS, the default event loop,
 * LittleFS (for zb_store's /storage/zb_devices.bin), app_config, data_core,
 * then zigbee_start(). No WiFi, BLE, ESP-NOW, swarm, webserver or brain. */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "app_config.h"
#include "data_core.h"
#include "zigbee.h"
#include "uart_link.h"

#define TAG "h2fw"

/* A 3 s heartbeat: the H2's USB-JTAG console is only readable if something
 * logs periodically (a once-only line is missed if the monitor attaches
 * late -- a spike lesson). Reports formation + heap + device count. */
static void status_task(void *a)
{
    (void)a;
    for (;;) {
        uint8_t ch = 0; uint16_t pan = 0; bool formed = false;
        zigbee_net_info(&ch, &pan, &formed);
        printf("H2FW formed=%d ch=%d pan=0x%04x heap free=%u largest=%u devices=%d\n",
               formed, ch, pan, (unsigned)esp_get_free_heap_size(),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
               zigbee_device_count());
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());  /* data_core posts PLANTHUB_DATA_EVENT */

    esp_vfs_littlefs_conf_t fs_conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t fs = esp_vfs_littlefs_register(&fs_conf);
    if (fs != ESP_OK) ESP_LOGE(TAG, "littlefs mount failed (%s); zb_store will not persist",
                               esp_err_to_name(fs));

    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(data_core_init());
    ESP_ERROR_CHECK(zigbee_start());                   /* coordinator + zb_cmd */
    ESP_ERROR_CHECK(uart_link_start());         /* C6<->H2 transport */

    xTaskCreate(status_task, "status", 3072, NULL, 4, NULL);
}
