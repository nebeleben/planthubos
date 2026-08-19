#include "sse.h"
#include "data_core.h"
#include "devices_json.h"
#include "event_log.h"
#include "events_json_escape.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

/* M5b Task 5 added EVENT_LEVEL_ALERT (2) and EVENT_LEVEL_CRITICAL (3), but
 * this render stayed a `level == 1 ? "notify" : "log"` binary choice, so
 * every actuator alert and every critical rendered as "log" -- an alert
 * feed that labels a critical as "log" defeats its own purpose (M5b Task
 * 11 carry-forward from Task 5). No `default`: a future EVENT_LEVEL_* this
 * switch doesn't handle trips -Werror=switch here, same defensive shape as
 * actor.c's verdict_alert(). */
static const char *event_level_str(uint8_t level)
{
    switch (level) {
    case EVENT_LEVEL_LOG:      return "log";
    case EVENT_LEVEL_NOTIFY:   return "notify";
    case EVENT_LEVEL_ALERT:    return "alert";
    case EVENT_LEVEL_CRITICAL: return "critical";
    }
    return "log";
}

/* Fixed JSON scaffolding around one escaped msg, worst case: leading comma
 * (1) + `{"seq":` (7) + a uint32_t's longest decimal form, 10 digits
 * ("4294967295") + `,"ts":` (6) + 10 digits + `,"rule_id":` (11) + 10
 * digits + `,"level":"` (10) + "critical" (8, the longest event_level_str()
 * result) + `","msg":"` (9) + `"}` (2) + NUL (1) = 85 bytes. 96 leaves
 * headroom without tracking that arithmetic exactly. */
#define EVT_LINE_OVERHEAD 96
#define EVT_LINE_MAX (EVENTS_JSON_ESC_MAX + EVT_LINE_OVERHEAD)   /* 793 */

/* GET /api/v1/events?after=<seq> body, on the httpd task. Was a cJSON tree
 * serialised in one shot by cJSON_PrintUnformatted() into a single
 * contiguous heap allocation; under this hub's normal heap fragmentation
 * that allocation returns NULL for a full ~24-event backlog (measured on
 * hardware: 500 "oom" at after=0, 10188 B free / 7680 B largest block),
 * taking out the safety core's only visibility surface (spec §4.6) exactly
 * when it has the most to report. Streamed instead: no cJSON tree, no
 * single large allocation -- each event is formatted into one small stack
 * buffer (EVT_LINE_MAX, see above) and sent as its own chunk via
 * httpd_resp_send_chunk(). JSON shape, field names/order/types, and the
 * `after`/EVENTS_POLL_MAX semantics are all unchanged from the cJSON
 * version above; only how the bytes reach the socket changed. */
static esp_err_t events_json_get(httpd_req_t *req, uint32_t after)
{
    size_t n = event_log_read(after, s_events_poll, EVENTS_POLL_MAX);
    httpd_resp_set_type(req, "application/json");

    esp_err_t err = httpd_resp_send_chunk(req, "{\"events\":[", 11);
    if (err != ESP_OK) return err;

    for (size_t i = 0; i < n; i++) {
        const event_t *e = &s_events_poll[i];
        char line[EVT_LINE_MAX];
        int w = snprintf(line, sizeof line,
                          "%s{\"seq\":%u,\"ts\":%u,\"rule_id\":%u,\"level\":\"%s\",\"msg\":\"",
                          i == 0 ? "" : ",",
                          (unsigned)e->seq, (unsigned)e->ts, (unsigned)e->rule_id,
                          event_level_str(e->level));
        if (w < 0 || (size_t)w >= sizeof line) return ESP_FAIL;   /* cannot happen, see EVT_LINE_OVERHEAD */
        size_t pos = (size_t)w;

        /* Remaining room is EVT_LINE_MAX - w >= EVENTS_JSON_ESC_MAX (the
         * scaffold above never gets close to using up EVT_LINE_OVERHEAD),
         * so msg's worst-case escape always fits without truncating. */
        events_json_escape(e->msg, line + pos, sizeof(line) - pos);
        pos += strlen(line + pos);

        int w2 = snprintf(line + pos, sizeof(line) - pos, "\"}");
        if (w2 < 0 || (size_t)w2 >= sizeof(line) - pos) return ESP_FAIL;   /* cannot happen, same margin */
        pos += (size_t)w2;

        err = httpd_resp_send_chunk(req, line, pos);
        if (err != ESP_OK) return err;
    }

    char tail[48];
    int tn = snprintf(tail, sizeof tail, "],\"last_seq\":%u}", (unsigned)event_log_last_seq());
    if (tn < 0 || (size_t)tn >= sizeof tail) return ESP_FAIL;   /* uint32_t max is 10 digits, tail is 48 */
    err = httpd_resp_send_chunk(req, tail, (size_t)tn);
    if (err != ESP_OK) return err;

    /* ESP-IDF requires a final zero-length chunk to terminate a chunked
     * response. */
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void on_sensor_update(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    /* data_core.c now posts a full device_id_t (kind + 8-byte addr), not a
     * bare mac -- M6b fix round 1. Read it as one directly rather than
     * reconstructing a DEV_KIND_BLE id from raw bytes; the old
     * reconstruction was sound only while every producer was BLE-kind, and
     * would otherwise misrepresent a non-BLE device as BLE (worst case,
     * colliding with a real BLE device that happens to share the first six
     * address bytes). M6b Task 12: this used to `return` here for anything
     * not DEV_KIND_BLE -- "downstream of the registry stays unaware Zigbee
     * exists" (the milestone spec's own words) meant downstream needs no
     * Zigbee-SPECIFIC code, not that a Zigbee reading must be dropped. This
     * handler already treats a device generically below (device_json() ->
     * devices_json.c switches on every device_kind_t), so no kind check is
     * needed at all -- every device, whatever its kind, is just a device. */
    const device_id_t *devid = data;
    /* Single-device lookup via data_core_get_device() (M1/M2 fixwave), not a
     * full registry_t snapshot + registry_find(): this handler only ever
     * runs on the single default event-loop task, which has just a 2304 B
     * stack, and only needs the one device this update is for. A
     * device_entry_t out-param (~124 B, data_core_get_device()'s own doc
     * comment) is small enough to be a plain stack local -- no static
     * needed, and no ~2 KB registry_t (whole-table snapshot for a
     * one-device lookup) held in .bss for this file's whole lifetime
     * either. plant_ids is passed NULL (empty array) -- this handler is not
     * worth a plants_table_t snapshot on this task's tiny stack, and the
     * SSE push is a "something changed" nudge, not the plant-binding source
     * of truth (GET /api/v1/plants is; see task-6-report.md's contract
     * notes). The message buffer below is heap, not static -- see its own
     * comment. */
    device_entry_t dev;
    if (!data_core_get_device(devid, &dev)) return;
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON *o = device_json(&dev, NULL, now_uptime_s);
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
