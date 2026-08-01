#include "ota_post.h"
#include "api_v1.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "ota";

static void restart_cb(void *arg) { esp_restart(); }

static void send_err_json(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    httpd_resp_sendstr(req, body);
}

esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { send_err_json(req, "500 Internal Server Error", "no ota partition"); return ESP_OK; }
    if (req->content_len < 4096 || req->content_len > part->size) {
        send_err_json(req, "400 Bad Request", "bad image size");
        return ESP_OK;
    }

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, req->content_len, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        send_err_json(req, "500 Internal Server Error", "ota begin failed");
        return ESP_OK;
    }

    static uint8_t buf[4096];   /* single httpd worker task; no reentrancy */
    size_t remaining = req->content_len;
    while (remaining > 0) {
        int n = httpd_req_recv(req, (char *)buf,
                               remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n <= 0) {
            esp_ota_abort(ota);
            send_err_json(req, "400 Bad Request", "upload interrupted");
            return ESP_FAIL;   /* close the desynced socket */
        }
        if ((err = esp_ota_write(ota, buf, n)) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota);
            send_err_json(req, "500 Internal Server Error", "flash write failed");
            return ESP_OK;
        }
        remaining -= n;
    }

    if ((err = esp_ota_end(ota)) != ESP_OK) {           /* validates the image */
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        send_err_json(req, "400 Bad Request", "invalid image");
        return ESP_OK;
    }
    if ((err = esp_ota_set_boot_partition(part)) != ESP_OK) {
        send_err_json(req, "500 Internal Server Error", "set boot failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    ESP_LOGW(TAG, "OTA ok (%u bytes to %s), rebooting", (unsigned)req->content_len, part->label);

    const esp_timer_create_args_t t = { .callback = restart_cb, .name = "ota_restart" };
    esp_timer_handle_t timer;
    if (esp_timer_create(&t, &timer) == ESP_OK)
        esp_timer_start_once(timer, 1500000);
    return ESP_OK;
}
