#include "sse.h"
#include "data_core.h"
#include "sensors_json.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "sse";
#define SSE_MAX_CLIENTS 2

static httpd_req_t *s_clients[SSE_MAX_CLIENTS];
static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_hb_timer;
static httpd_handle_t s_server;

/* Send to every connected client; drop clients whose socket errored. This
 * only ever runs on the httpd task's own context (queued via
 * httpd_queue_work below), so a stalled client blocking inside
 * httpd_resp_send_chunk for up to send_wait_timeout never stalls the
 * esp_timer task (heartbeat) or the default event-loop task (sensor
 * updates) -- it can only ever delay the httpd task's own request queue,
 * which is where such a wait belongs. */
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

/* httpd work-queue callback: takes ownership of the heap buffer produced by
 * on_sensor_update()/heartbeat() and frees it once sent. */
static void send_all_work(void *arg)
{
    char *buf = arg;
    send_all(buf, strlen(buf));
    free(buf);
}

/* Hand a heap-allocated, NUL-terminated message to the httpd task for
 * sending. Frees buf itself on failure (e.g. queue full) so callers never
 * have to. */
static void queue_send(char *buf)
{
    if (!buf) return;
    if (httpd_queue_work(s_server, send_all_work, buf) != ESP_OK) {
        ESP_LOGW(TAG, "sse work queue full, dropping message");
        free(buf);
    }
}

static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const uint8_t *mac = data;
    static registry_t snap;   /* only ever touched on the default event loop task */
    /* static: this handler only ever runs on the single default event-loop
     * task, which has just a 2304 B stack -- keep the 320 B message buffer
     * off it. */
    static char buf[320];
    data_core_snapshot(&snap);
    int idx = registry_find(&snap, mac);
    if (idx < 0) return;
    cJSON *o = sensor_json(&snap.sensors[idx]);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    int n = snprintf(buf, sizeof(buf), "data: %s\n\n", json);
    free(json);
    if (n > 0 && n < (int)sizeof(buf)) queue_send(strdup(buf));
}

static void heartbeat(void *arg)
{
    queue_send(strdup(": hb\n\n"));
}

static esp_err_t events_get(httpd_req_t *req)
{
    int slot = -1;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < SSE_MAX_CLIENTS && slot < 0; i++)
        if (!s_clients[i]) slot = i;
    if (slot < 0) {
        /* No free slot -- but a client may already be dead (peer reset,
         * cable pull) without us having noticed yet; keepalive alone can
         * take up to ~45s to surface that via a failed heartbeat send.
         * Peek each occupied socket for an already-visible close/error
         * before giving up, so a reconnecting client doesn't have to wait
         * out that window. events_get runs on the httpd task, same as the
         * queued sender (send_all_work), so this can't race a concurrent
         * send onto the same slot. */
        for (int i = 0; i < SSE_MAX_CLIENTS && slot < 0; i++) {
            int fd = httpd_req_to_sockfd(s_clients[i]);
            if (fd < 0) continue;
            char c;
            int r = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0 || (r < 0 && errno != EWOULDBLOCK && errno != EAGAIN)) {
                httpd_req_async_handler_complete(s_clients[i]);
                s_clients[i] = NULL;
                slot = i;
            }
        }
    }
    xSemaphoreGive(s_mutex);
    if (slot < 0) {
        /* esp_http_server's httpd_err_code_t has no 503 entry in this IDF
         * version; set the status line/body manually, same pattern as
         * api_v1.c's 413 response. */
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"sse client limit\"}");
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

    /* Without keepalive, a client that vanishes without a clean FIN (power
     * loss, wifi drop) pins this slot until LWIP's default retransmit
     * timeout, which is minutes -- far longer than our 2-slot budget can
     * absorb. Probing keeps idle-dead peers detectable within
     * ~30 + 5*3 = 45s, so the next heartbeat send (which calls
     * httpd_resp_send_chunk on a now-broken socket) reaps the slot. */
    int fd = httpd_req_to_sockfd(async);
    if (fd >= 0) {
        int one = 1, idle = 30, intvl = 5, cnt = 3;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_clients[slot] = async;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "sse client connected (slot %d)", slot);
    return ESP_OK;
}

void sse_init(httpd_handle_t server)
{
    s_server = server;
    s_mutex = xSemaphoreCreateMutex();
    httpd_uri_t events = { .uri = "/api/v1/events", .method = HTTP_GET, .handler = events_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &events));
    ESP_ERROR_CHECK(esp_event_handler_register(PLANTHUB_DATA_EVENT, DATA_EVENT_SENSOR_UPDATE,
                                               on_sensor_update, NULL));
    const esp_timer_create_args_t t = { .callback = heartbeat, .name = "sse_hb" };
    ESP_ERROR_CHECK(esp_timer_create(&t, &s_hb_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_hb_timer, 30 * 1000 * 1000ULL));
}
