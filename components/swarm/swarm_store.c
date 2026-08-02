#include "swarm_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "swarm_store";
#define NS "planthub"
#define KEY_ROLE  "role"
#define KEY_HUB   "sw_hub"
#define KEY_NODES "sw_nodes"

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[SWARM_LMK_LEN];
    uint8_t channel;
} hub_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[SWARM_LMK_LEN];
} node_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t count;
    node_entry_t n[SWARM_MAX_NODES];
} nodes_blob_t;

static SemaphoreHandle_t s_mutex;
static swarm_role_t s_role;
static bool s_hub_set;
static hub_blob_t s_hub;
static nodes_blob_t s_nodes;

static esp_err_t write_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_key(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t swarm_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_role = SWARM_ROLE_UNSET;
    s_hub_set = false;
    memset(&s_hub, 0, sizeof(s_hub));
    memset(&s_nodes, 0, sizeof(s_nodes));

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;  /* fresh NVS = defaults */

    uint8_t role_byte;
    if (nvs_get_u8(h, KEY_ROLE, &role_byte) == ESP_OK) {
        if (role_byte == SWARM_ROLE_UNSET || role_byte == SWARM_ROLE_MAIN || role_byte == SWARM_ROLE_NODE) {
            s_role = (swarm_role_t)role_byte;
        } else {
            ESP_LOGW(TAG, "invalid stored role byte %u; defaulting to UNSET", role_byte);
            s_role = SWARM_ROLE_UNSET;
        }
    }

    size_t hub_len = sizeof(s_hub);
    s_hub_set = nvs_get_blob(h, KEY_HUB, &s_hub, &hub_len) == ESP_OK && hub_len == sizeof(s_hub);
    if (!s_hub_set) memset(&s_hub, 0, sizeof(s_hub));

    size_t nodes_len = sizeof(s_nodes);
    if (nvs_get_blob(h, KEY_NODES, &s_nodes, &nodes_len) != ESP_OK || nodes_len != sizeof(s_nodes)) {
        memset(&s_nodes, 0, sizeof(s_nodes));
    }
    if (s_nodes.count > SWARM_MAX_NODES) s_nodes.count = SWARM_MAX_NODES;  /* defensive */

    nvs_close(h);
    ESP_LOGI(TAG, "role=%d hub_paired=%d nodes=%d", s_role, s_hub_set, s_nodes.count);
    return ESP_OK;
}

swarm_role_t swarm_store_role(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    swarm_role_t r = s_role;
    xSemaphoreGive(s_mutex);
    return r;
}

esp_err_t swarm_store_set_role(swarm_role_t r)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_ROLE, (uint8_t)r);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) s_role = r;
    xSemaphoreGive(s_mutex);
    return err;
}

bool swarm_store_hub(uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN], uint8_t *channel_out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_hub_set;
    if (ok) {
        if (mac_out) memcpy(mac_out, s_hub.mac, 6);
        if (lmk_out) memcpy(lmk_out, s_hub.lmk, SWARM_LMK_LEN);
        if (channel_out) *channel_out = s_hub.channel;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t swarm_store_set_hub(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN], uint8_t channel)
{
    hub_blob_t blob;
    memcpy(blob.mac, mac, 6);
    memcpy(blob.lmk, lmk, SWARM_LMK_LEN);
    blob.channel = channel;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = write_blob(KEY_HUB, &blob, sizeof(blob));
    if (err == ESP_OK) {
        s_hub = blob;
        s_hub_set = true;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_set_channel(uint8_t channel)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_hub_set) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    hub_blob_t blob = s_hub;
    blob.channel = channel;
    esp_err_t err = write_blob(KEY_HUB, &blob, sizeof(blob));
    if (err == ESP_OK) s_hub = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_clear_hub(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = erase_key(KEY_HUB);
    if (err == ESP_OK) {
        s_hub_set = false;
        memset(&s_hub, 0, sizeof(s_hub));
    }
    xSemaphoreGive(s_mutex);
    return err;
}

int swarm_store_node_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_nodes.count;
    xSemaphoreGive(s_mutex);
    return n;
}

bool swarm_store_node_at(int idx, uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = idx >= 0 && idx < s_nodes.count;
    if (ok) {
        if (mac_out) memcpy(mac_out, s_nodes.n[idx].mac, 6);
        if (lmk_out) memcpy(lmk_out, s_nodes.n[idx].lmk, SWARM_LMK_LEN);
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t swarm_store_add_node(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nodes_blob_t blob = s_nodes;
    int idx = -1;
    for (int i = 0; i < blob.count; i++) {
        if (memcmp(blob.n[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (blob.count >= SWARM_MAX_NODES) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        idx = blob.count++;
    }
    memcpy(blob.n[idx].mac, mac, 6);
    memcpy(blob.n[idx].lmk, lmk, SWARM_LMK_LEN);

    esp_err_t err = write_blob(KEY_NODES, &blob, sizeof(blob));
    if (err == ESP_OK) s_nodes = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_clear_nodes(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = erase_key(KEY_NODES);
    if (err == ESP_OK) memset(&s_nodes, 0, sizeof(s_nodes));
    xSemaphoreGive(s_mutex);
    return err;
}
