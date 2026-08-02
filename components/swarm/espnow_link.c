/* ESP-NOW transport: peers, blocking send, channel control.
 *
 * Header note (verified against ~/esp/esp-idf/components/esp_wifi/include/esp_now.h,
 * IDF 5.3): esp_now.h is not a standalone IDF component in this SDK version —
 * it is a header inside esp_wifi, and its symbols are provided by the
 * esp_wifi library. There is nothing to REQUIRES beyond esp_wifi itself.
 */
#include "espnow_link.h"
#include "swarm_frame.h"
#include "swarm_store.h"

#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>

static const char *TAG = "espnow_link";

/* ESP-NOW primary master key. This is NOT a secret: the PMK only encrypts
 * the per-peer LMK inside ESP-NOW's internal peer table on this device; it
 * is never transmitted and provides no protection against an attacker who
 * doesn't already have code-execution on this chip. The actual link
 * security comes from the per-node LMK (random, generated at pairing time)
 * plus the fact that pairing only accepts a new node during a short,
 * operator-initiated time window (see pairing.c). Every PlanthHub device
 * can safely share this same constant. */
static const uint8_t ESPNOW_PMK[16] = "planthub-pmk-v1";

static espnow_rx_cb_t s_rx_cb;
static SemaphoreHandle_t s_send_lock;   /* serializes espnow_link_send/broadcast callers */
static SemaphoreHandle_t s_send_done;   /* signalled by the ESP-NOW send callback */
static volatile esp_now_send_status_t s_last_status;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* Runs on the WiFi driver task: decode RSSI and hand off immediately.
     * Do not block, do not touch NVS/flash, do not call espnow_link_send()
     * from here or from anything this callback calls synchronously. */
    if (!s_rx_cb || !info || !info->src_addr || !data || len <= 0) return;
    int rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    s_rx_cb(info->src_addr, data, len, rssi);
}

static void on_send(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    s_last_status = status;
    xSemaphoreGive(s_send_done);
}

esp_err_t espnow_link_add_peer(const uint8_t mac[6], const uint8_t *lmk, uint8_t channel)
{
    if (!mac) return ESP_ERR_INVALID_ARG;

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = channel;      /* 0 == "follow the radio's current channel" */
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = (lmk != NULL);
    if (lmk) memcpy(peer.lmk, lmk, sizeof(peer.lmk));

    if (esp_now_is_peer_exist(mac)) {
        return esp_now_mod_peer(&peer);
    }
    return esp_now_add_peer(&peer);
}

esp_err_t espnow_link_init(espnow_rx_cb_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;

    if (!s_send_done) {
        s_send_done = xSemaphoreCreateBinary();
        if (!s_send_done) return ESP_ERR_NO_MEM;
    }
    if (!s_send_lock) {
        s_send_lock = xSemaphoreCreateMutex();
        if (!s_send_lock) return ESP_ERR_NO_MEM;
    }

    s_rx_cb = cb;

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) return err;

    err = esp_now_register_recv_cb(on_recv);
    if (err != ESP_OK) return err;

    err = esp_now_register_send_cb(on_send);
    if (err != ESP_OK) return err;

    err = esp_now_set_pmk(ESPNOW_PMK);
    if (err != ESP_OK) return err;

    /* Broadcast peer: unencrypted (ESP-NOW cannot encrypt broadcast frames)
     * and pinned to no fixed channel. */
    const uint8_t bcast[6] = ESPNOW_BROADCAST_MAC;
    err = espnow_link_add_peer(bcast, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) return err;

    /* Re-add whatever peer(s) this device already knows about, so a reboot
     * doesn't require re-pairing. Best-effort: a failure here just means
     * the peer gets re-added lazily on next successful pairing/ping. */
    swarm_role_t role = swarm_store_role();
    if (role == SWARM_ROLE_NODE) {
        uint8_t hub_mac[6], hub_lmk[SWARM_LMK_LEN], hub_ch;
        if (swarm_store_hub(hub_mac, hub_lmk, &hub_ch)) {
            esp_err_t peer_err = espnow_link_add_peer(hub_mac, hub_lmk, 0);
            if (peer_err != ESP_OK) {
                ESP_LOGW(TAG, "failed to re-add stored hub peer: %s", esp_err_to_name(peer_err));
            }
        }
    } else if (role == SWARM_ROLE_MAIN) {
        int n = swarm_store_node_count();
        for (int i = 0; i < n; i++) {
            uint8_t mac[6], lmk[SWARM_LMK_LEN];
            if (swarm_store_node_at(i, mac, lmk)) {
                esp_err_t peer_err = espnow_link_add_peer(mac, lmk, 0);
                if (peer_err != ESP_OK) {
                    ESP_LOGW(TAG, "failed to re-add stored node peer: %s", esp_err_to_name(peer_err));
                }
            }
        }
    }

    return ESP_OK;
}

/* Common blocking-send path for espnow_link_send()/espnow_link_broadcast().
 * Never call this from the espnow_rx_cb_t callback: on_send() runs on the
 * same WiFi driver task as on_recv(), so a send issued from inside on_recv()
 * would block that task waiting for a signal only that same task can
 * deliver, which either deadlocks or (best case) always times out. */
static esp_err_t send_blocking(const uint8_t *mac, const uint8_t *data, size_t len)
{
    if (!mac || !data || len == 0) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_send_lock, portMAX_DELAY);

    /* Clear any stale completion (e.g. from a prior call that timed out
     * just before its callback finally fired) so we don't read a leftover
     * status for this send. */
    xSemaphoreTake(s_send_done, 0);

    esp_err_t err = esp_now_send(mac, data, len);
    if (err == ESP_OK) {
        if (xSemaphoreTake(s_send_done, pdMS_TO_TICKS(200)) == pdTRUE) {
            err = (s_last_status == ESP_NOW_SEND_SUCCESS) ? ESP_OK : ESP_FAIL;
        } else {
            err = ESP_ERR_TIMEOUT;
        }
    }

    xSemaphoreGive(s_send_lock);
    return err;
}

esp_err_t espnow_link_send(const uint8_t mac[6], const uint8_t *data, size_t len)
{
    return send_blocking(mac, data, len);
}

esp_err_t espnow_link_broadcast(const uint8_t *data, size_t len)
{
    const uint8_t bcast[6] = ESPNOW_BROADCAST_MAC;
    return send_blocking(bcast, data, len);
}

esp_err_t espnow_link_set_channel(uint8_t ch)
{
    if (ch < 1 || ch > 13) return ESP_ERR_INVALID_ARG;

    /* Refuse when this device is associated to an AP as a station: the
     * hub's operating channel follows that association, and yanking it out
     * from under esp_wifi would break both the AP link and (per the ESP-NOW
     * docs) ESP-NOW itself, since ESP-NOW shares the radio's channel with
     * WiFi. esp_wifi_get_mode() alone only reports the configured mode
     * (STA/AP/APSTA), not whether the station is actually associated, so we
     * additionally probe esp_wifi_sta_get_ap_info(): it returns ESP_OK only
     * while associated and ESP_ERR_WIFI_NOT_CONNECT otherwise. A node that
     * hasn't joined any AP (the normal M5a case) is free to hop channels. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA)) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            ESP_LOGW(TAG, "refusing channel %u: station is associated to an AP", ch);
            return ESP_ERR_INVALID_STATE;
        }
    }

    return esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
}

uint8_t espnow_link_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&primary, &second);
    return primary;
}
