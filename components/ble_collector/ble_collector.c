#include "ble_collector.h"
#include "data_core.h"
#include "mibeacon.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"

static const char *TAG = "ble_collector";

#define XIAOMI_SVC_UUID 0xFE95
/* GAP scan units are 0.625 ms */
#define SCAN_UNITS(ms) ((uint16_t)((ms) * 1000 / 625))

static int gap_event(struct ble_gap_event *event, void *arg);

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .passive = 1,
        .itvl = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS),
        .window = SCAN_UNITS(CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS),
        .filter_duplicates = 0,   /* we want repeated frames; registry dedups */
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) return 0;
        if (!fields.svc_data_uuid16 || fields.svc_data_uuid16_len < 2) return 0;
        uint16_t uuid = (uint16_t)(fields.svc_data_uuid16[0] | (fields.svc_data_uuid16[1] << 8));
        if (uuid != XIAOMI_SVC_UUID) return 0;
        mibeacon_t m;
        if (mibeacon_parse(fields.svc_data_uuid16 + 2, fields.svc_data_uuid16_len - 2, &m) == MIBEACON_OK) {
            if (m.product_id != MIBEACON_PRODUCT_MIFLORA) return 0;
            /* Direct reception: no relaying node, just heard (age_s = 0).
             * event->disc.rssi is the advertisement's RSSI in dBm (127 if
             * unavailable per NimBLE's ble_gap.h), passed straight through
             * so direct readings carry signal strength too, same as
             * node-relayed ones. */
            data_core_submit_from(&m, NULL, event->disc.rssi, 0);
        }
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();   /* should not happen with BLE_HS_FOREVER, but be safe */
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ESP_LOGI(TAG, "NimBLE synced, starting passive scan (itvl=%dms window=%dms)",
             CONFIG_PLANTHUB_BLE_SCAN_ITVL_MS, CONFIG_PLANTHUB_BLE_SCAN_WINDOW_MS);
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* returns on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_collector_start(void)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
