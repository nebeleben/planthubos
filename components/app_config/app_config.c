#include "app_config.h"
#include "creds_validate.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define NS "planthub"

typedef struct {
    bool used;          /* entry populated (name may still be "absent") */
    uint8_t mac[6];
    char name[33];      /* "" = known absent */
} name_cache_t;

static name_cache_t s_names[16];
static SemaphoreHandle_t s_name_mutex;

static name_cache_t *name_cache_find(const uint8_t mac[6], bool take_free)
{
    name_cache_t *free_e = NULL;
    for (int i = 0; i < 16; i++) {
        if (s_names[i].used && memcmp(s_names[i].mac, mac, 6) == 0) return &s_names[i];
        if (!s_names[i].used && !free_e) free_e = &s_names[i];
    }
    if (!take_free || !free_e) return NULL;
    free_e->used = true;
    memcpy(free_e->mac, mac, 6);
    free_e->name[0] = '\0';
    return free_e;
}

esp_err_t app_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) return err;
    s_name_mutex = xSemaphoreCreateMutex();
    if (!s_name_mutex) return ESP_ERR_NO_MEM;
    return ESP_OK;
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
    if (err == ESP_OK) {
        xSemaphoreTake(s_name_mutex, portMAX_DELAY);
        name_cache_t *ce = name_cache_find(mac, true);
        if (ce) strlcpy(ce->name, name, sizeof(ce->name));
        xSemaphoreGive(s_name_mutex);
    }
    return err;
}

bool app_config_get_sensor_name(const uint8_t mac[6], char out[33])
{
    xSemaphoreTake(s_name_mutex, portMAX_DELAY);
    name_cache_t *e = name_cache_find(mac, false);
    if (e) {
        bool present = e->name[0] != '\0';
        if (present) memcpy(out, e->name, 33);
        xSemaphoreGive(s_name_mutex);
        return present;
    }
    xSemaphoreGive(s_name_mutex);

    /* NVS miss path (first lookup for this mac) — do NVS outside the lock */
    char key[16], name[33] = "";
    name_key(key, mac);
    nvs_handle_t h;
    bool present = false;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(name);
        present = nvs_get_str(h, key, name, &len) == ESP_OK && name[0] != '\0';
        nvs_close(h);
    }
    xSemaphoreTake(s_name_mutex, portMAX_DELAY);
    name_cache_t *ce = name_cache_find(mac, true);
    if (ce) memcpy(ce->name, name, 33);
    xSemaphoreGive(s_name_mutex);
    if (present) memcpy(out, name, 33);
    return present;
}
