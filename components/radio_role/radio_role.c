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
#elif CONFIG_PLANTHUB_RADIO_ROLE_ZIGBEE
    return RADIO_ROLE_ZIGBEE;
#else
    return RADIO_ROLE_WIFI_ONLY;
#endif
}

esp_err_t radio_role_init(void)
{
    s_role = radio_role_default();
    s_set = false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no NVS namespace yet; default %s", radio_role_str(s_role));
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
    }
    ESP_LOGI(TAG, "radio_role=%s (%s)", radio_role_str(s_role), s_set ? "nvs" : "default");
    return ESP_OK;
}

radio_role_t radio_role_get(void)  { return s_role; }
bool         radio_role_is_set(void) { return s_set; }

esp_err_t radio_role_set(radio_role_t role)
{
    if ((int)role < 0 || (int)role > (int)RADIO_ROLE_ZIGBEE) return ESP_ERR_INVALID_ARG;
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
