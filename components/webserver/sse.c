#include "sse.h"
#include "data_core.h"
#include "sensors_json.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "sse";
#define SSE_MAX_CLIENTS 2

static httpd_req_t *s_clients[SSE_MAX_CLIENTS];
static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_hb_timer;

/* Send to every connected client; drop clients whose socket errored. */
static void send_all(const char *buf, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS; i++) {
        if (!s_clients[i]) continue;
        if (httpd_resp_send_chunk(s_clients[i], buf, len) != ESP_OK) {
            httpd_req_async_handler_complete(s_clients[i]);
            s_clients[i] = NULL;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const uint8_t *mac = data;
    static registry_t snap;   /* only ever touched on the default event loop task */
    data_core_snapshot(&snap);
    int idx = registry_find(&snap, mac);
    if (idx < 0) return;
    cJSON *o = sensor_json(&snap.sensors[idx]);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    char buf[320];
    int n = snprintf(buf, sizeof(buf), "data: %s\n\n", json);
    free(json);
    if (n > 0 && n < (int)sizeof(buf)) send_all(buf, n);
}

static void heartbeat(void *arg)
{
    send_all(": hb\n\n", 6);
}

static esp_err_t events_get(httpd_req_t *req)
{
    int slot = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS && slot < 0; i++)
        if (!s_clients[i]) slot = i;
    xSemaphoreGive(s_mutex);
    if (slot < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sse client limit");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    /* first chunk flushes the headers and tells the client our retry interval */
    esp_err_t err = httpd_resp_send_chunk(req, "retry: 3000\n\n", 13);
    if (err != ESP_OK) return err;

    httpd_req_t *async = NULL;
    err = httpd_req_async_handler_begin(req, &async);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_clients[slot] = async;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "sse client connected (slot %d)", slot);
    return ESP_OK;
}

void sse_init(httpd_handle_t server)
{
    s_mutex = xSemaphoreCreateMutex();
    httpd_uri_t events = { .uri = "/api/v1/events", .method = HTTP_GET, .handler = events_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events));
    ESP_ERROR_CHECK(esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE,
                                               on_sensor_update, NULL));
    const esp_timer_create_args_t t = { .callback = heartbeat, .name = "sse_hb" };
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hb_timer, 30 * 1000 * 1000ULL));
}
