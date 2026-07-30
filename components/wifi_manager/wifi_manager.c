#include "wifi_manager.h"
#include "wifi_fsm.h"
#include "app_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <string.h>

ESP_EVENT_DEFINE_BASE(PLANTHUB_EVENT);

static const char *TAG = "wifi_mgr";
#define MAX_RETRIES 5

static wifi_fsm_t s_fsm;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_ip[16] = "0.0.0.0";

static void do_action(wifi_action_t act);

static void start_sta(void)
{
    wifi_creds_t creds;
    if (!app_config_get_wifi(&creds)) return;
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, creds.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, creds.password, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
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
