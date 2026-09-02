#include "wifi_manager.h"
#include "wifi_fsm.h"
#include "app_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_phy_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ESP_EVENT_DEFINE_BASE(PLANTHUB_EVENT);

static const char *TAG = "wifi_mgr";
#define MAX_RETRIES 5

static wifi_fsm_t s_fsm;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_ip[16] = "0.0.0.0";
static bool s_boot_prescan_done = false;

static void do_action(wifi_action_t act);

static void start_sta(void)
{
    wifi_creds_t creds;
    if (!app_config_get_wifi(&creds)) return;
    wifi_config_t cfg = { 0 };
    /* Not strlcpy: wifi_sta_config_t has no ssid_len field, so a full 32-byte
     * SSID or 64-byte raw-hex PSK must land in the array without truncation
     * or a forced NUL terminator. cfg is zero-initialized above, so an
     * unterminated max-length array here is valid for STA config. strlen is
     * guaranteed <= 32/64 by creds_validate() at store time. */
    memcpy(cfg.sta.ssid, creds.ssid, strlen(creds.ssid));
    memcpy(cfg.sta.password, creds.password, strlen(creds.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Boot-time association fix (radio-role plan, Task 2b). Measured on
     * the C6: esp_wifi_connect() issued straight after esp_wifi_start()
     * fails its own scan with WIFI_REASON_NO_AP_FOUND on every attempt
     * (~2.4 s apart) even with the router at -57 dBm on channel 1, while
     * an explicit active scan first sees the router and the following
     * connect succeeds on the first try. Before this fix the BLE-on hub
     * only associated on its 6th attempt (~13 s) and a BLE-off hub hit
     * MAX_RETRIES and fell to the setup AP. So: scan for our SSID, pin
     * the channel when found, then connect. Blocking, bounded by the
     * per-channel scan time below (<= ~2 s over 13 channels). Not found
     * is not fatal -- connect anyway and let the FSM retry as before.
     *
     * start_sta() has two callers: wifi_manager_start() at boot, and
     * on_planthub_event()'s WIFI_EV_NEW_CREDS path when the user submits
     * new credentials at runtime (from the default event-loop task,
     * where a ~2 s blocking scan would stall every other event handler
     * on the system). Only the boot call needs this: it is the one
     * racing a receiver that hasn't seen any APs yet. s_boot_prescan_done
     * gates it to that first call; every later start_sta() (including a
     * later NEW_CREDS retry after a first successful boot) just connects
     * straight away. */
    if (!s_boot_prescan_done) {
        s_boot_prescan_done = true;
        wifi_scan_config_t sc = {
            .ssid = (uint8_t *)creds.ssid,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active = { .min = 60, .max = 150 },
        };
        esp_err_t se = esp_wifi_scan_start(&sc, true);
        uint16_t n = 0;
        if (se == ESP_OK) esp_wifi_scan_get_ap_num(&n);
        if (n > 0) {
            wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
            if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
                /* Records come sorted by RSSI; pin the strongest BSS's channel. */
                cfg.sta.channel = recs[0].primary;
                ESP_LOGI(TAG, "pre-scan: %u BSS for our SSID, best ch=%u rssi=%d", (unsigned)n,
                         recs[0].primary, recs[0].rssi);
                ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
            } else {
                /* calloc failed: the driver still holds the scan list it
                 * built for esp_wifi_scan_get_ap_records() to consume --
                 * release it explicitly since nothing will call that now. */
                esp_wifi_clear_ap_list();
            }
            free(recs);
        } else {
            ESP_LOGW(TAG, "pre-scan: our SSID not seen (%s, n=%u); connecting anyway",
                     esp_err_to_name(se), (unsigned)n);
        }
    }

    esp_wifi_connect();
}

static void start_ap(void)
{
    char name[16];
    app_config_hub_name(name);
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.ap.ssid, name, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(name);
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.max_connection = 4;
    /* APSTA so /api/v1/wifi/scan works while the portal is open */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    strlcpy(s_ip, "192.168.4.1", sizeof(s_ip));
    ESP_LOGI(TAG, "AP mode: SSID=%s ip=%s", name, s_ip);
}

static void do_action(wifi_action_t act)
{
    switch (act) {
    case WIFI_ACT_START_STA:
        esp_wifi_stop();
        start_sta();
        break;
    case WIFI_ACT_START_AP:
        esp_wifi_stop();
        start_ap();
        break;
    case WIFI_ACT_RECONNECT:
        esp_wifi_connect();
        break;
    case WIFI_ACT_NONE:
        break;
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected: reason=%d rssi=%d", d ? d->reason : -1, d ? d->rssi : 0);
        do_action(wifi_fsm_step(&s_fsm, WIFI_EV_DISCONNECTED));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "STA connected, ip=%s", s_ip);
        do_action(wifi_fsm_step(&s_fsm, WIFI_EV_GOT_IP));
    }
}

/* Runs on the default event loop task, same as on_wifi_event above, so this
 * and on_wifi_event can never touch s_fsm concurrently. */
static void on_planthub_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == PLANTHUB_EVENT && id == PLANTHUB_EVENT_APPLY_CREDS) {
        do_action(wifi_fsm_step(&s_fsm, WIFI_EV_NEW_CREDS));
    }
}

esp_err_t wifi_manager_start(void)
{
    /* esp_netif_init() and esp_event_loop_create_default() are called by
     * main() before webserver_start(), so that the webserver's
     * WIFI_EVENT_AP_START/AP_STOP handler is registered before esp_wifi
     * ever starts (see main.c). */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    /* PHY warm-up (radio-role plan, Task 2c). Measured on the C6: on a
     * fresh NVS the WiFi receiver comes up deaf -- an unfiltered active
     * scan sees none of ~20 bench APs and every connect fails with
     * WIFI_REASON_NO_AP_FOUND -- for as long as we watched (36 s+), with
     * full or stored PHY calibration alike. Nothing on the WiFi side
     * (bandwidth/protocol, stop/start, power-save) wakes it. Enabling the
     * BT modem's PHY once BEFORE WiFi starts does: the first scan then
     * hears everything and the STA associates first try. That is what
     * the NimBLE controller init had been doing implicitly on every V1
     * hub, and why the bug only surfaced once the wifi_only role (no BT
     * controller) became the fresh-hub default. Placement matters: the
     * same pulse after esp_wifi_start() has no effect. Harmless in the
     * ble/zigbee roles (their stacks enable their own modems later). */
    esp_phy_enable(PHY_MODEM_BT);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_phy_disable(PHY_MODEM_BT);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PLANTHUB_EVENT, PLANTHUB_EVENT_APPLY_CREDS, on_planthub_event, NULL));

    wifi_fsm_init(&s_fsm, MAX_RETRIES);
    wifi_creds_t creds;
    bool have = app_config_get_wifi(&creds);
    do_action(wifi_fsm_step(&s_fsm, have ? WIFI_EV_CREDS_PRESENT : WIFI_EV_NO_CREDS));
    return ESP_OK;
}

void wifi_manager_apply_new_creds(void)
{
    /* Safe to call from any task (e.g. a FreeRTOS timer callback on the
     * timer daemon task): post onto the default event loop instead of
     * touching s_fsm directly, so it's serialized with on_wifi_event. */
    esp_event_post(PLANTHUB_EVENT, PLANTHUB_EVENT_APPLY_CREDS, NULL, 0, portMAX_DELAY);
}

bool wifi_manager_is_ap_mode(void)
{
    return s_fsm.state == WIFI_ST_AP_MODE;
}

void wifi_manager_get_ip(char out[16])
{
    strlcpy(out, s_ip, 16);
}
