#include "app_config.h"
#include "creds_validate.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>

#define NS "planthub"

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

bool app_config_get_wifi(wifi_creds_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t slen = sizeof(out->ssid), plen = sizeof(out->password);
    bool ok = nvs_get_str(h, "wifi_ssid", out->ssid, &slen) == ESP_OK &&
              nvs_get_str(h, "wifi_pass", out->password, &plen) == ESP_OK;
    nvs_close(h);
    return ok && out->ssid[0] != '\0';
}

esp_err_t app_config_set_wifi(const wifi_creds_t *c)
{
    if (!creds_validate(c->ssid, c->password)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if ((err = nvs_set_str(h, "wifi_ssid", c->ssid)) == ESP_OK &&
        (err = nvs_set_str(h, "wifi_pass", c->password)) == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t app_config_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, "wifi_ssid");
    nvs_erase_key(h, "wifi_pass");
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void app_config_hub_name(char out[16])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, 16, "PlantHub-%02X%02X", mac[4], mac[5]);
}

static void name_key(char out[16], const uint8_t mac[6])
{
    snprintf(out, 16, "nm_%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

esp_err_t app_config_set_sensor_name(const uint8_t mac[6], const char *name)
{
    if (!name || strlen(name) > 32) return ESP_ERR_INVALID_ARG;
    char key[16];
    name_key(key, mac);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = name[0] ? nvs_set_str(h, key, name) : nvs_erase_key(h, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool app_config_get_sensor_name(const uint8_t mac[6], char out[33])
{
    char key[16];
    name_key(key, mac);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 33;
    bool ok = nvs_get_str(h, key, out, &len) == ESP_OK && out[0] != '\0';
    nvs_close(h);
    return ok;
}
