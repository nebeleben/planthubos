#include "api_v1.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_v1";
#define FW_VERSION "0.1.0"

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

static esp_err_t status_get(httpd_req_t *req)
{
    char name[16], ip[16];
    app_config_hub_name(name);
    wifi_manager_get_ip(ip);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "version", FW_VERSION);
    cJSON_AddBoolToObject(root, "ap_mode", wifi_manager_is_ap_mode());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    return send_json(req, root);
}

static int rssi_desc_cmp(const void *a, const void *b)
{
    const wifi_ap_record_t *ra = a, *rb = b;
    return (int)rb->rssi - (int)ra->rssi;
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true /* block */);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
        return ESP_OK;
    }
    uint16_t n = 20;
    /* wifi_ap_record_t is 80+ bytes; keep it off the httpd task stack, which
     * also has to have room for the cJSON tree below. */
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
        return ESP_OK;
    }
    esp_wifi_scan_get_ap_records(&n, recs);

    /* Sort strongest-first so the dedup pass below keeps the strongest
     * instance of each SSID, and the output is sorted by RSSI as promised. */
    qsort(recs, n, sizeof(wifi_ap_record_t), rssi_desc_cmp);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        bool dup = false; /* keep strongest instance of each ssid */
        for (int j = 0; j < i && !dup; j++)
            dup = strcmp((char *)recs[i].ssid, (char *)recs[j].ssid) == 0;
        if (dup || recs[i].ssid[0] == '\0') continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o, "secure", recs[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);
    return send_json(req, root);
}

static void apply_creds_cb(TimerHandle_t t)
{
    wifi_manager_apply_new_creds();
    xTimerDelete(t, 0);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[256];
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    if (req->content_len > sizeof(body) - 1) {
        /* esp_http_server's httpd_err_code_t has no 413 entry in this IDF
         * version; set the status line/body manually. */
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"payload too large\"}");
        return ESP_OK;
    }

    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
            return ESP_OK;
        }
        received += (size_t)r;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    const cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "password");

    wifi_creds_t creds = { 0 };
    if (cJSON_IsString(ssid)) strlcpy(creds.ssid, ssid->valuestring, sizeof(creds.ssid));
    if (cJSON_IsString(pass)) strlcpy(creds.password, pass->valuestring, sizeof(creds.password));
    cJSON_Delete(json);

    if (app_config_set_wifi(&creds) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"invalid credentials\"}");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");

    /* Switch to STA ~1.5s later so this response reaches the client first. */
    TimerHandle_t t = xTimerCreate("creds", pdMS_TO_TICKS(1500), pdFALSE, NULL, apply_creds_cb);
    if (t) {
        xTimerStart(t, 0);
    } else {
        /* Creds are already saved via app_config_set_wifi above, so a
         * reboot still picks them up even if we can't schedule the switch. */
        ESP_LOGE(TAG, "failed to create creds-apply timer; will apply on next reboot");
    }
    return ESP_OK;
}

void api_v1_register(httpd_handle_t server)
{
    httpd_uri_t status = { .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    httpd_uri_t scan = { .uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan));
    httpd_uri_t wifi = { .uri = "/api/v1/wifi", .method = HTTP_POST, .handler = wifi_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi));
}
