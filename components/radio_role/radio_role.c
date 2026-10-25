#include "radio_role.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG = "radio_role";
#define NS  "planthub"
#define KEY "radio_role"

static radio_role_t s_role;
static bool         s_set;

radio_role_t radio_role_default(void)
{
#if CONFIG_PLANTHUB_RADIO_ROLE_BLE
    return RADIO_ROLE_BLE;
#else
    return RADIO_ROLE_WIFI_ONLY;
#endif
}

esp_err_t radio_role_init(void)
{
    s_role = radio_role_default();
    s_set = false;

    nvs_handle_t h;
    esp_err_t open_err = nvs_open(NS, NVS_READONLY, &h);
    if (open_err != ESP_OK) {
        ESP_LOGI(TAG, "no NVS namespace yet (%s); default %s",
                 esp_err_to_name(open_err), radio_role_str(s_role));
        return ESP_OK;
    }
    char buf[16] = {0};
    size_t len = sizeof(buf);
    esp_err_t err = nvs_get_str(h, KEY, buf, &len);
    nvs_close(h);
    if (err == ESP_OK) {
        radio_role_t parsed;
        if (radio_role_parse(buf, &parsed)) {
            s_role = parsed;
            s_set = true;
        } else {
            /* Unknown string: treat as unset so onboarding re-asks rather
             * than silently running the default forever. */
            ESP_LOGW(TAG, "stored radio_role \"%s\" unknown; treating as unset (default %s)",
                     buf, radio_role_str(s_role));
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* A real read error (corrupt entry, truncated value, ...) rather
         * than the ordinary "never written yet" case -- worth a WARN even
         * though the fallback behavior (default/unset) is the same. */
        ESP_LOGW(TAG, "nvs_get_str(\"%s\") failed (%s); treating as unset (default %s)",
                 KEY, esp_err_to_name(err), radio_role_str(s_role));
    }
    /* No board runs a zigbee radio role any more: Zigbee bridge nodes were
     * retired 2026-10 (Zigbee now comes from the two-chip hub's wired H2).
     * A `zigbee` left in NVS by a former bridge node runs as BLE instead --
     * done here, at the source, so every radio_role_get() caller sees BLE
     * and nothing ever starts a coordinator. NVS is left as it was; the next
     * radio_role_set() (onboarding, or the hub's NODE_CONFIG) overwrites it. */
    if (s_role == RADIO_ROLE_ZIGBEE) {
        ESP_LOGW(TAG, "radio_role zigbee is retired (no Zigbee bridge nodes); running as ble");
        s_role = RADIO_ROLE_BLE;
    }
    ESP_LOGI(TAG, "radio_role=%s (%s)", radio_role_str(s_role), s_set ? "nvs" : "default");
    return ESP_OK;
}

radio_role_t radio_role_get(void)  { return s_role; }
bool         radio_role_is_set(void) { return s_set; }

esp_err_t radio_role_set(radio_role_t role)
{
    if ((int)role < 0 || (int)role > (int)RADIO_ROLE_ZIGBEE) return ESP_ERR_INVALID_ARG;
    /* See radio_role_init(): zigbee is retired as a radio role. */
    if (role == RADIO_ROLE_ZIGBEE) return ESP_ERR_NOT_SUPPORTED;
    s_role = role;
    s_set = true;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s); RAM cache holds %s until reboot",
                 esp_err_to_name(err), radio_role_str(role));
        return err;
    }
    err = nvs_set_str(h, KEY, radio_role_str(role));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persist %s failed (%s)", radio_role_str(role), esp_err_to_name(err));
    }
    return err;
}
