#include "webserver.h"
#include "api_v1.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "wifi_manager.h"
#include "dns_hijack.h"

static const char *TAG = "webserver";
static httpd_handle_t s_server;

/* The captive-portal fallback is always registered (last, so exact routes
 * win). Whether it actually redirects depends on the *current* AP-mode
 * state, not a boot-time snapshot, so it stays correct across a runtime
 * STA->AP fallback (retry exhaustion) without a reboot. */
static esp_err_t captive_redirect(httpd_req_t *req)
{
    if (!wifi_manager_is_ap_mode()) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Start/stop the DNS hijack task as the wifi driver actually enters/leaves
 * AP mode (covers boot AND later runtime fallback/recovery). */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) return;
    if (id == WIFI_EVENT_AP_START) {
        dns_hijack_start();
    } else if (id == WIFI_EVENT_AP_STOP) {
        dns_hijack_stop();
    }
}

/* Symbols created by EMBED_FILES ('.' and '-' become '_') */
extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[]   asm("_binary_index_html_gz_end");
extern const uint8_t app_js_gz_start[]     asm("_binary_app_js_gz_start");
extern const uint8_t app_js_gz_end[]       asm("_binary_app_js_gz_end");
extern const uint8_t app_css_gz_start[]    asm("_binary_app_css_gz_start");
extern const uint8_t app_css_gz_end[]      asm("_binary_app_css_gz_end");

typedef struct {
    const char *uri;
    const char *content_type;
    const uint8_t *start, *end;
} static_asset_t;

static esp_err_t asset_get(httpd_req_t *req)
{
    const static_asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    return httpd_resp_send(req, (const char *)a->start, a->end - a->start);
}

static const static_asset_t ASSETS[] = {
    { "/",        "text/html",       index_html_gz_start, index_html_gz_end },
    { "/app.js",  "text/javascript", app_js_gz_start,     app_js_gz_end },
    { "/app.css", "text/css",        app_css_gz_start,    app_css_gz_end },
};

esp_err_t webserver_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192; /* wifi_scan_get's records buffer + cJSON work no longer fit in 4K */
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < sizeof(ASSETS) / sizeof(ASSETS[0]); i++) {
        httpd_uri_t u = {
            .uri = ASSETS[i].uri, .method = HTTP_GET,
            .handler = asset_get, .user_ctx = (void *)&ASSETS[i],
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &u));
    }
    api_v1_register(s_server);

    static const httpd_uri_t fallback = {
        .uri = "/*", .method = HTTP_GET, .handler = captive_redirect,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &fallback));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    if (wifi_manager_is_ap_mode()) {
        /* Catch-up for the boot case: wifi_manager_start() runs before this
         * handler is registered, so WIFI_EVENT_AP_START may already have
         * fired. dns_hijack_start() is idempotent, so this is safe even if
         * the event also lands. */
        dns_hijack_start();
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

httpd_handle_t webserver_handle(void) { return s_server; }
