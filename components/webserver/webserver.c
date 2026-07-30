#include "webserver.h"
#include "api_v1.h"
#include "esp_log.h"
#include "wifi_manager.h"
#include "dns_hijack.h"

static const char *TAG = "webserver";
static httpd_handle_t s_server;

static esp_err_t captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
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

    if (wifi_manager_is_ap_mode()) {
        static const httpd_uri_t fallback = {
            .uri = "/*", .method = HTTP_GET, .handler = captive_redirect,
        };
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &fallback));
        dns_hijack_start();
    }

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

httpd_handle_t webserver_handle(void) { return s_server; }
