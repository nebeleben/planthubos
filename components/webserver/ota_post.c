#include "ota_post.h"
#include "api_v1.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <string.h>

static const char *TAG = "ota";

static void restart_cb(void *arg) { esp_restart(); }

/* Rollback guard ---------------------------------------------------------
 * With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE an OTA'd image boots in
 * PENDING_VERIFY and is reverted on the next reboot unless it confirms
 * itself. "Healthy" here means the hub is reachable again -- either it
 * rejoined the LAN or it fell back to the onboarding AP -- because a
 * reachable hub can always be updated again, while an unreachable one is
 * exactly the brick this guard exists to undo. */
static void mark_valid_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGW(TAG, "network up, image confirmed (rollback cancelled): %s", esp_err_to_name(err));
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_AP_START, mark_valid_cb);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, mark_valid_cb);
}

void ota_rollback_guard_start(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running from %s, no rollback pending", running->label);
        return;
    }
    ESP_LOGW(TAG, "running from %s pending verify; confirming once the network is up", running->label);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START, mark_valid_cb, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, mark_valid_cb, NULL));
}

/* Node-side rollback guard (M5c) -----------------------------------------
 * See ota_post.h for the full contract and why this is a separate pair of
 * functions rather than an extra branch inside ota_rollback_guard_start()
 * above: that function's own confirmation path (mark_valid_cb) is wired to
 * WIFI_EVENT_AP_START/IP_EVENT_STA_GOT_IP, neither of which a paired node
 * (radio-only ESP-NOW, no AP, never associates) ever fires. The hub's own
 * criteria are untouched above; this is purely additive. */
static bool s_node_pending;    /* true once start_node() finds a pending-verify image */
static bool s_node_confirmed;  /* true once confirmed -- guards a wasted repeat flash write */

void ota_rollback_guard_start_node(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (!running || esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "running from %s, no rollback pending (node)", running->label);
        return;
    }
    ESP_LOGW(TAG, "running from %s pending verify (node); confirming once a reading reaches "
                  "the hub or a PONG is received", running->label);
    s_node_pending = true;
    s_node_confirmed = false;
}

void ota_rollback_guard_node_confirm(const char *reason)
{
    if (!s_node_pending || s_node_confirmed) return;
    /* Fix (code review, M5c-era latent bug): this used to latch
     * s_node_confirmed = true BEFORE calling
     * esp_ota_mark_app_valid_cancel_rollback(), so a transient flash
     * failure on that call was recorded as a success -- every LATER health
     * signal this boot (another delivered reading, another PONG, another
     * CHECKIN_ACK) then hit the s_node_confirmed guard above and silently
     * did nothing, leaving the image genuinely unconfirmed with no way to
     * retry short of a reboot. Only latch on ESP_OK, so a failure here
     * still leaves s_node_confirmed false and the next health signal gets
     * a real second attempt. */
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) s_node_confirmed = true;
    ESP_LOGW(TAG, "node healthy (%s), image confirmed (rollback cancelled): %s",
             reason ? reason : "?", esp_err_to_name(err));
}

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
            /* esp_ota_write checks the image header magic on the first chunk, so
             * uploading a non-firmware file lands here rather than at esp_ota_end
             * -- report that as a bad image, not as a flash failure. */
            if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                send_err_json(req, "400 Bad Request", "invalid image");
            else
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
