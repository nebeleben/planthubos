#include "sse.h"
#include "data_core.h"
#include "devices_json.h"
#include "event_log.h"
#include "cJSON.h"
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

/* Task 6 (spec §6): named `event` SSE message, pushed by main.c's
 * event_log hook on every event_log_append(). json is caller-owned (the
 * hook builds it once and also hands it to mqtt_pub_event()) -- this
 * copies it into one right-sized heap buffer via snprintf rather than
 * strdup+reformat, then hands that buffer to queue_send() exactly like
 * on_sensor_update()'s buf below. */
void sse_push_event(const char *json)
{
    if (!json) return;
    size_t cap = strlen(json) + 32;   /* "event: event\ndata: " + json + "\n\n" + NUL, generous */
    char *buf = malloc(cap);
    if (!buf) return;
    int n = snprintf(buf, cap, "event: event\ndata: %s\n\n", json);
    if (n <= 0 || (size_t)n >= cap) { free(buf); return; }
    queue_send(buf);
}

/* GET /api/v1/events?after=<seq> -- Task 6's JSON event-history poll (spec
 * §6), sharing this exact route/handler with the SSE stream below rather
 * than a second registration: ESP-IDF's httpd_register_uri_handler()
 * rejects an exact-URI duplicate for the same method
 * (ESP_ERR_HTTPD_HANDLER_EXISTS, httpd_uri.c) -- api_v1.c cannot register
 * its own "/api/v1/events" GET alongside this one, so the poll branch lives
 * here instead, keyed off the "after" query key's mere presence (its value
 * may be empty -> after=0, matching the spec's "default after=0"). No
 * "after" key at all falls through to the existing SSE-stream behaviour
 * below, unchanged. Capped at 50 events/call per the spec; s_events_poll
 * is static (>6KB of event_t, far too big for the 8KB httpd task stack)
 * and safe to share -- like api_v1.c's s_api_reg_snap, only one httpd
 * request ever runs at a time. */
#define EVENTS_POLL_MAX 50
static event_t s_events_poll[EVENTS_POLL_MAX];

static esp_err_t events_json_get(httpd_req_t *req, uint32_t after)
{
    size_t n = event_log_read(after, s_events_poll, EVENTS_POLL_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "events");
    for (size_t i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "seq", s_events_poll[i].seq);
        cJSON_AddNumberToObject(o, "ts", s_events_poll[i].ts);
        cJSON_AddNumberToObject(o, "rule_id", s_events_poll[i].rule_id);
        cJSON_AddStringToObject(o, "level", s_events_poll[i].level == 1 ? "notify" : "log");
        cJSON_AddStringToObject(o, "msg", s_events_poll[i].msg);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "last_seq", event_log_last_seq());
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err;
    if (body) {
        err = httpd_resp_sendstr(req, body);
        free(body);
    } else {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    return err;
}

static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* mac is copied into the event queue by esp_event (see data_core.c's
     * esp_event_post() calls); every current producer is BLE-kind, same as
     * V1 -- data_core.h doesn't post a device_id_t here, so DEV_KIND_BLE is
     * the only sound interpretation of a bare 6-byte mac payload. */
    const uint8_t *mac = data;
    /* static: this handler only ever runs on the single default event-loop
     * task, which has just a 2304 B stack -- keep the registry snapshot
     * off it (same reasoning api_v1.c's s_api_reg_snap doc comment gives;
     * only one instance of this handler ever runs at a time, so sharing is
     * safe). registry_t is bigger than V1's legacy_registry_t shim it
     * replaces (~2 KB vs ~0.9 KB) -- see task-6-report.md for the byte
     * accounting. The message buffer below is heap, not static -- see its
     * own comment. */
    static registry_t snap;
    /* plant_ids is passed NULL (empty array) -- this handler is not worth
     * a plants_table_t snapshot on this task's tiny stack, and the SSE
     * push is a "something changed" nudge, not the plant-binding source of
     * truth (GET /api/v1/plants is; see task-6-report.md's contract
     * notes). */
    data_core_snapshot(&snap);
    device_id_t devid = device_id_from_mac(DEV_KIND_BLE, mac);
    int idx = registry_find(&snap, &devid);
    if (idx < 0) return;
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON *o = device_json(&snap.devices[idx], NULL, now_uptime_s);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json) return;
    /* Right-sized heap buffer, same pattern as sse_push_event() above --
     * device_json()'s payload was already being heap-allocated twice
     * (cJSON's print buffer, then a strdup() of a static scratch copy);
     * one malloc() sized off the json string itself removes both the
     * double-allocation and the 1024 B static buffer that only ever held
     * an intermediate copy. "data: " (6) + json + "\n\n" (2) + NUL (1) = 9;
     * +16 keeps a little slack without the wasted-until-huge sizing a
     * fixed static needed. */
    size_t cap = strlen(json) + 16;
    char *buf = malloc(cap);
    if (!buf) {
        free(json);
        return;
    }
    int n = snprintf(buf, cap, "data: %s\n\n", json);
    free(json);
    if (n <= 0 || (size_t)n >= cap) {
        free(buf);
        return;
    }
    queue_send(buf);
}

static void heartbeat(void *arg)
{
    queue_send(strdup(": hb\n\n"));
}

static esp_err_t events_get(httpd_req_t *req)
{
    /* JSON poll branch (see events_json_get()'s comment above) -- checked
     * first, before any SSE client-slot bookkeeping below, since a poll
     * request never wants one.
     *
     * Reviewer fix: must distinguish "genuinely no after key"
     * (ESP_ERR_NOT_FOUND at either step below -- falls through to the SSE
     * stream, unchanged) from any OTHER non-OK result
     * (ESP_ERR_HTTPD_RESULT_TRUNC for a query string over 63 bytes or an
     * "after" value over 15 bytes, ESP_ERR_INVALID_ARG, ...), which must
     * 400 rather than silently fall through -- an unauthenticated
     * GET .../events?after=<16+ digits> would otherwise be misrouted into
     * claiming one of the only SSE_MAX_CLIENTS==2 slots, and two such
     * requests can starve the live feed for every real client
     * indefinitely. A present-but-non-numeric "after" value is likewise
     * rejected (strict endptr check) rather than silently treated as 0. */
    char query[64], val[16];
    esp_err_t qerr = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (qerr == ESP_OK) {
        esp_err_t kerr = httpd_query_key_value(query, "after", val, sizeof(val));
        if (kerr == ESP_OK) {
            char *endptr = NULL;
            unsigned long after = strtoul(val, &endptr, 10);
            if (endptr == val || *endptr != '\0') {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad events query");
                return ESP_OK;
            }
            return events_json_get(req, (uint32_t)after);
        }
        if (kerr != ESP_ERR_NOT_FOUND) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad events query");
            return ESP_OK;
        }
        /* kerr == ESP_ERR_NOT_FOUND: query string present, just no "after" key -- fall through to SSE. */
    } else if (qerr != ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad events query");
        return ESP_OK;
    }
    /* qerr == ESP_ERR_NOT_FOUND (no query string at all): fall through to SSE. */

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
