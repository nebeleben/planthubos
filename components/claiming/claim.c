#include "claim.h"
#include "authtok.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "claim";
#define NS "planthub"
#define KEY "claim_h"
#define RESET_GPIO GPIO_NUM_0
#define RESET_HOLD_MS 10000

static bool s_claimed;
static uint8_t s_hash[32];

static esp_err_t store_hash(const uint8_t hash[32])
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (hash) err = nvs_set_blob(h, KEY, hash, 32);
    else {
        err = nvs_erase_key(h, KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t claim_init(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;  /* fresh NVS = unclaimed */
    size_t len = sizeof(s_hash);
    s_claimed = nvs_get_blob(h, KEY, s_hash, &len) == ESP_OK && len == 32;
    nvs_close(h);
    ESP_LOGI(TAG, "hub is %s", s_claimed ? "claimed" : "unclaimed");
    return ESP_OK;
}

bool claim_is_claimed(void) { return s_claimed; }

esp_err_t claim_generate(char secret_hex[65])
{
    if (s_claimed) return ESP_ERR_INVALID_STATE;
    uint8_t secret[32], hash[32];
    esp_fill_random(secret, sizeof(secret));
    if (mbedtls_sha256(secret, sizeof(secret), hash, 0) != 0) return ESP_FAIL;
    esp_err_t err = store_hash(hash);
    if (err != ESP_OK) return err;
    memcpy(s_hash, hash, 32);
    s_claimed = true;
    authtok_hex_encode(secret, secret_hex);
    ESP_LOGI(TAG, "hub claimed");
    return ESP_OK;
}

bool claim_verify(const char *secret_hex)
{
    if (!s_claimed || !secret_hex) return false;
    uint8_t secret[32], hash[32];
    if (!authtok_hex_decode(secret_hex, secret)) return false;
    if (mbedtls_sha256(secret, sizeof(secret), hash, 0) != 0) return false;
    return authtok_ct_equal(hash, s_hash);
}

esp_err_t claim_reset(void)
{
    esp_err_t err = store_hash(NULL);
    if (err == ESP_OK) {
        s_claimed = false;
        memset(s_hash, 0, sizeof(s_hash));
        ESP_LOGI(TAG, "claim reset");
    }
    return err;
}

static void reset_button_task(void *arg)
{
    int held_ms = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(RESET_GPIO) == 0) {
            held_ms += 100;
            if (held_ms == RESET_HOLD_MS) {
                ESP_LOGW(TAG, "factory reset: clearing claim + wifi, restarting");
                claim_reset();
                app_config_clear_wifi();
                esp_restart();
            }
        } else {
            held_ms = 0;
        }
    }
}

void factory_reset_button_start(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << RESET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);
    xTaskCreate(reset_button_task, "factory_rst", 2048, NULL, 2, NULL);
}
