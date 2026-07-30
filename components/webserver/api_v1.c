#include "api_v1.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "cJSON.h"
#include "esp_timer.h"

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

void api_v1_register(httpd_handle_t server)
{
    httpd_uri_t status = { .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
}
