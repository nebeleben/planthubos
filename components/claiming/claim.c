#include "claim.h"
#include "authtok.h"
#include "app_config.h"
#include "swarm_store.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "claim";
#define NS "planthub"
#define KEY "claim_h"
/* Must match the BOOT button of the board actually in use, or the only
 * documented recovery path for an unreachable device does nothing at all.
 * Was hardcoded to GPIO_NUM_0 (the classic-ESP32 boot pin) until hardware
 * showed that no amount of holding BOOT reset an ESP32-C3 node: the C3
 * devkit's button is on GPIO9 and nothing is wired to GPIO0, so the hold
 * silently never fired on this project's primary target. Defaults are now
 * per-target in Kconfig -- see CONFIG_PLANTHUB_FACTORY_RESET_GPIO. */
#define RESET_GPIO ((gpio_num_t)CONFIG_PLANTHUB_FACTORY_RESET_GPIO)
#define RESET_HOLD_MS 10000

static bool s_claimed;
static uint8_t s_hash[32];
static SemaphoreHandle_t s_mutex;

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
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;  /* fresh NVS = unclaimed */
    size_t len = sizeof(s_hash);
    s_claimed = nvs_get_blob(h, KEY, s_hash, &len) == ESP_OK && len == 32;
    nvs_close(h);
    ESP_LOGI(TAG, "hub is %s", s_claimed ? "claimed" : "unclaimed");
    return ESP_OK;
}

/* Lock-free: s_claimed is a single bool word, so a torn read is not possible.
 * The only race is reading a stale "claimed" just before/after an in-flight
 * claim_reset() flips it. A stale-true read just means a caller proceeds to
 * claim_verify(), which re-checks s_claimed under the mutex and fails closed
 * if the reset has since landed; a stale-false read on a claim just-in-
 * progress can't happen since claim_generate() only ever transitions
 * false->true once and nothing else can be racing it (claim is one-shot). */
bool claim_is_claimed(void) { return s_claimed; }

esp_err_t claim_generate(char secret_hex[65])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_claimed) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t secret[32], hash[32];
    esp_fill_random(secret, sizeof(secret));
    if (mbedtls_sha256(secret, sizeof(secret), hash, 0) != 0) {
        xSemaphoreGive(s_mutex);
        memset(secret, 0, sizeof(secret));
        return ESP_FAIL;
    }
    esp_err_t err = store_hash(hash);
    if (err != ESP_OK) {
        xSemaphoreGive(s_mutex);
        memset(secret, 0, sizeof(secret));
        return err;
    }
    memcpy(s_hash, hash, 32);
    s_claimed = true;
    xSemaphoreGive(s_mutex);
    authtok_hex_encode(secret, secret_hex);
    memset(secret, 0, sizeof(secret));
    ESP_LOGI(TAG, "hub claimed");
    return ESP_OK;
}

bool claim_verify(const char *secret_hex)
{
    if (!secret_hex) return false;
    uint8_t secret[32], hash[32];
    if (!authtok_hex_decode(secret_hex, secret)) return false;
    if (mbedtls_sha256(secret, sizeof(secret), hash, 0) != 0) {
        memset(secret, 0, sizeof(secret));
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_claimed && authtok_ct_equal(hash, s_hash);
    xSemaphoreGive(s_mutex);
    memset(secret, 0, sizeof(secret));
    return ok;
}

esp_err_t claim_reset(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = store_hash(NULL);
    if (err == ESP_OK) {
        s_claimed = false;
        memset(s_hash, 0, sizeof(s_hash));
        ESP_LOGI(TAG, "claim reset");
    }
    xSemaphoreGive(s_mutex);
    return err;
}

static void reset_button_task(void *arg)
{
    int held_ms = 0;
    bool fired = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (gpio_get_level(RESET_GPIO) == 0) {
            held_ms += 100;
            if (held_ms >= RESET_HOLD_MS && !fired) {
                fired = true;
                ESP_LOGW(TAG, "factory reset: clearing claim + wifi + swarm role/pairing, restarting");
                claim_reset();
                app_config_clear_wifi();
                /* A paired node runs no web server at all -- without this,
                 * the physical button would be its only recovery path, and
                 * even that wouldn't work, since role/hub/node state lived
                 * on untouched and it would just pair right back on
                 * reboot. Clearing swarm state here returns any role
                 * (main, node, or a node stuck in the pair-failed portal)
                 * to a fresh ROLE_UNSET device. */
                swarm_store_reset_all();
                esp_restart();
            }
        } else {
            held_ms = 0;
            fired = false;
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
