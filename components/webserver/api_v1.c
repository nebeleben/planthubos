#include "api_v1.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "data_core.h"
#include "sensors_json.h"
#include "storage.h"
#include "timekeeper.h"
#include "claim.h"
#include "ota_post.h"
#include "swarm.h"
#include "swarm_store.h"
#include "node_ota.h"
#include "pairing.h"
#include "integr_config.h"
#include "espnow_link.h"
#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_v1";
#define FW_VERSION "0.8.0"

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t api_send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"unauthorized\"}");
    return ESP_OK;
}

bool api_auth_ok(httpd_req_t *req)
{
    if (!claim_is_claimed()) return true;
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strncmp(hdr, "Bearer ", 7) != 0) return false;
    return claim_verify(hdr + 7);
}

/* Gate for POST /api/v1/role and POST /api/v1/pair/retry.
 *
 * Deliberately NOT the uniform "unclaimed = open" posture every other
 * mutating endpoint in this file uses (api_auth_ok() alone, which returns
 * true whenever the hub is unclaimed): role selection is inherently an
 * onboarding-time, physically-present action, so requiring AP mode covers
 * every legitimate path -- a fresh device's own portal, or a node sitting
 * in its post-failure retry portal. A claimed, already-onboarded hub can
 * additionally be converted with its key. There is no legitimate remote,
 * unauthenticated reason to change a running hub's role. Unlike other
 * unclaimed-open endpoints, a role flip is stealthy and persistent: the
 * device leaves the network entirely and can then be silently adopted by
 * any ESP-NOW peer in range, with recovery needing physical access to the
 * factory-reset button. That asymmetry is why this one endpoint is
 * deliberately stricter -- do not "harmonise" it back to plain
 * api_auth_ok(), which is true on an unclaimed hub regardless of AP mode
 * and would make this check a no-op against exactly the LAN-remote flip
 * it exists to block. */
static bool role_change_ok(httpd_req_t *req)
{
    return wifi_manager_is_ap_mode() || (claim_is_claimed() && api_auth_ok(req));
}

static const char *role_str(swarm_role_t r)
{
    switch (r) {
    case SWARM_ROLE_MAIN: return "main";
    case SWARM_ROLE_NODE: return "node";
    default:              return "unset";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    char name[16], ip[16];
    app_config_hub_name(name);
    wifi_manager_get_ip(ip);
    swarm_role_t role = swarm_store_role();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", name);
    cJSON_AddStringToObject(root, "version", FW_VERSION);
    cJSON_AddBoolToObject(root, "ap_mode", wifi_manager_is_ap_mode());
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(root, "time_synced", timekeeper_synced());
    cJSON_AddNumberToObject(root, "epoch_s", timekeeper_now());
    cJSON_AddBoolToObject(root, "claimed", claim_is_claimed());
    cJSON_AddStringToObject(root, "role", role_str(role));
    /* Node: paired to a hub. Unset/main: has at least one node paired to it. */
    bool paired = (role == SWARM_ROLE_NODE) ? swarm_store_hub(NULL, NULL, NULL)
                                             : swarm_store_node_count() > 0;
    cJSON_AddBoolToObject(root, "paired", paired);
    cJSON_AddBoolToObject(root, "pair_failed", swarm_store_pair_failed());
    size_t fs_total = 0, fs_used = 0;
    if (esp_littlefs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        cJSON_AddNumberToObject(root, "fs_total", fs_total);
        cJSON_AddNumberToObject(root, "fs_used", fs_used);
    }
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) cJSON_AddStringToObject(root, "partition", running->label);
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
    if (claim_is_claimed() && !wifi_manager_is_ap_mode() && !api_auth_ok(req)) return api_send_401(req);

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

static esp_err_t sensors_get(httpd_req_t *req)
{
    static registry_t snap;   /* 16 entries is too big for the stack; guarded by httpd single-call-per-uri */
    data_core_snapshot(&snap);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; i < REGISTRY_MAX_SENSORS; i++) {
        if (snap.sensors[i].in_use) cJSON_AddItemToArray(arr, sensor_json(&snap.sensors[i]));
    }
    return send_json(req, root);
}

/* Parse 12 uppercase/lowercase hex chars into mac[6]; returns false on malformed input. */
static bool parse_mac12(const char *s, uint8_t mac[6])
{
    for (int i = 0; i < 6; i++) {
        unsigned hi, lo;
        char a = s[i * 2], b = s[i * 2 + 1];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else return false;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else return false;
        mac[i] = (uint8_t)((hi << 4) | lo);
    }
    return s[12] == '\0' || s[12] == '/';
}

static bool resolve_shim(void *rctx, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch)
{
    (void)rctx;
    return timekeeper_resolve(boot_id, rel_s, epoch);
}

typedef struct {
    httpd_req_t *req;
    bool first;
    bool failed;
} hist_ctx_t;

/* Once c->failed is set we just return without sending further chunks; we
 * don't abort the underlying storage_query scan early, but that scan is
 * bounded (<=STORAGE_RAW_CAP == 2880 records) so letting it run to
 * completion costs at most a bounded, harmless amount of wasted work. */
static void hist_row(void *vctx, uint32_t epoch, const storage_rec_t *rec)
{
    hist_ctx_t *c = vctx;
    if (c->failed) return;
    char line[128];
    int n = snprintf(line, sizeof(line), "%s[%lu,", c->first ? "" : ",", (unsigned long)epoch);
    c->first = false;
    #define APPEND_NUM(cond, fmt, val) \
        n += (cond) ? snprintf(line + n, sizeof(line) - n, fmt, val) : snprintf(line + n, sizeof(line) - n, "null")
    APPEND_NUM(rec->temp_dc != STORAGE_TEMP_NONE, "%.1f", rec->temp_dc / 10.0);
    n += snprintf(line + n, sizeof(line) - n, ",");
    APPEND_NUM(rec->moisture_pct != STORAGE_U8_NONE, "%u", rec->moisture_pct);
    n += snprintf(line + n, sizeof(line) - n, ",");
    APPEND_NUM(rec->lux != STORAGE_LUX_NONE, "%lu", (unsigned long)rec->lux);
    n += snprintf(line + n, sizeof(line) - n, ",");
    APPEND_NUM(rec->conductivity_us != STORAGE_U16_NONE, "%u", rec->conductivity_us);
    n += snprintf(line + n, sizeof(line) - n, "]");
    #undef APPEND_NUM
    if (n >= (int)sizeof(line) || httpd_resp_sendstr_chunk(c->req, line) != ESP_OK)
        c->failed = true;
}

static esp_err_t history_get(httpd_req_t *req)
{
    /* URI: /api/v1/sensors/{MAC12}/history */
    const char *macs = req->uri + strlen("/api/v1/sensors/");
    uint8_t mac[6];
    if (!parse_mac12(macs, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");
        return ESP_OK;
    }
    if (!strstr(req->uri, "/history")) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    char query[96] = "", val[16];
    httpd_req_get_url_query_str(req, query, sizeof(query));
    storage_tier_t tier = STORAGE_TIER_RAW;
    if (httpd_query_key_value(query, "tier", val, sizeof(val)) == ESP_OK && strcmp(val, "hourly") == 0)
        tier = STORAGE_TIER_HOURLY;

    uint32_t now = timekeeper_now();
    uint32_t to = now, from = now > 86400 ? now - 86400 : 0;
    if (httpd_query_key_value(query, "to", val, sizeof(val)) == ESP_OK) to = (uint32_t)strtoul(val, NULL, 10);
    if (httpd_query_key_value(query, "from", val, sizeof(val)) == ESP_OK) from = (uint32_t)strtoul(val, NULL, 10);

    httpd_resp_set_type(req, "application/json");
    bool synced = timekeeper_synced();
    char head[64];
    snprintf(head, sizeof(head), "{\"tier\":\"%s\",\"synced\":%s,\"points\":[",
             tier == STORAGE_TIER_RAW ? "raw" : "hourly", synced ? "true" : "false");
    httpd_resp_sendstr_chunk(req, head);

    if (synced) {
        hist_ctx_t ctx = { .req = req, .first = true, .failed = false };
        storage_query("/storage", mac, tier, from, to, resolve_shim, NULL, hist_row, &ctx);
        /* A chunk send already failed (client/socket gone) -- don't send the
         * trailing chunks over a dead connection, and return non-OK so
         * esp_http_server closes the session instead of believing it's
         * still alive. */
        if (ctx.failed) return ESP_FAIL;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunked response */
    return ESP_OK;
}

static esp_err_t sensor_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    /* URI: /api/v1/sensors/{MAC12} */
    uint8_t mac[6];
    if (!parse_mac12(req->uri + strlen("/api/v1/sensors/"), mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");
        return ESP_OK;
    }
    char body[128];
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
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    esp_err_t err = cJSON_IsString(name) ? app_config_set_sensor_name(mac, name->valuestring)
                                         : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t time_post(httpd_req_t *req)
{
    char body[64];
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
    const cJSON *epoch = cJSON_GetObjectItem(json, "epoch_s");
    bool ok = cJSON_IsNumber(epoch) && epoch->valuedouble > 1e9 && epoch->valuedouble < 4e9;
    if (ok) timekeeper_set_epoch((uint32_t)epoch->valuedouble);
    cJSON_Delete(json);
    if (!ok) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid epoch"); return ESP_OK; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t claim_post(httpd_req_t *req)
{
    char secret[65];
    esp_err_t err = claim_generate(secret);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"already claimed\"}");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "claim failed");
        return ESP_OK;
    }
    /* Bypass cJSON/send_json here: both would leave an un-scrubbed copy of
     * the one-time secret sitting in heap memory (cJSON's strdup'd string
     * and send_json's cJSON_PrintUnformatted buffer), and this is the
     * highest-value secret in the system. Format and send it directly so
     * the only copies are stack-local and can be zeroed below. */
    char body[96];
    snprintf(body, sizeof(body), "{\"secret\":\"%s\"}", secret);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);   /* copies to the socket synchronously before returning */
    memset(body, 0, sizeof(body));
    memset(secret, 0, sizeof(secret));
    return ESP_OK;
}

static esp_err_t unclaim_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    if (claim_reset() != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "unclaim failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t nodes_get(httpd_req_t *req)
{
    /* Sized for SWARM_MAX_NODES (6) entries, each now carrying a name (up to
     * SWARM_NODE_NAME_LEN=24 bytes) and a "buffered" field alongside
     * mac/last_seen_s/frames_rx/rssi -- ~140 bytes/entry worst case, well
     * under this with room to spare. 512 (the pre-M5b size) is no longer
     * enough since the name/buffered fields were added. */
    char buf[1536];
    int n = swarm_node_list_json(buf, sizeof(buf));
    if (n < 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nodes list failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t nodes_pair_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    pairing_open_window(120);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"window_s\":120}");
    return ESP_OK;
}

/* POST /api/v1/nodes/{MAC12} body {"name":"..."} -- rename (empty clears). */
static esp_err_t node_rename_post(httpd_req_t *req, const uint8_t mac[6])
{
    char body[64];
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    if (req->content_len > sizeof(body) - 1) {
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
    const cJSON *name = cJSON_GetObjectItem(json, "name");
    esp_err_t err = cJSON_IsString(name) ? swarm_store_set_node_name(mac, name->valuestring)
                                         : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown node");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/nodes/{MAC12}/ota -- starts a hub-side node_ota.c session
 * pushing this hub's own running firmware to the node. 409 when one is
 * already running (for ANY node -- node_ota.c allows only one session
 * hub-wide at a time, see node_ota.h). */
static esp_err_t node_ota_start_post(httpd_req_t *req, const uint8_t mac[6])
{
    esp_err_t err = node_ota_start(mac);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"an update is already in progress\"}");
        return ESP_OK;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown node");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota start failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/nodes/{MAC12}/ota/abort. node_ota_abort() has no per-node
 * target of its own (only one session can ever be active hub-wide -- see
 * node_ota.h), so this checks the active session actually targets `mac`
 * first: a stray abort against the wrong node's URL (a UI race, a stale
 * tab) must not silently kill an unrelated node's in-flight transfer. */
static esp_err_t node_ota_abort_post(httpd_req_t *req, const uint8_t mac[6])
{
    node_ota_progress_t p;
    node_ota_progress(&p);
    if (!p.active || memcmp(p.mac, mac, 6) != 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"no active update for this node\"}");
        return ESP_OK;
    }
    esp_err_t err = node_ota_abort();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "abort failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/nodes/{MAC12}[/ota[/abort]] -- rename, start an OTA push, or
 * abort one, all under the SAME registered route. ESP-IDF's
 * httpd_uri_match_wildcard only ever special-cases a trailing '*' in the
 * template (verified against this repo's installed IDF,
 * components/esp_http_server/src/httpd_uri.c: `asterisk` is only set when
 * the template's LAST character is '*') -- so a second, more specific
 * wildcard registered under the same "/api/v1/nodes/" prefix would be
 * rejected outright at registration time (httpd_register_uri_handler()
 * runs the very same wildcard match against every already-registered
 * template as its own duplicate check, and the existing wildcard node
 * route already "covers" any such string). Dispatching on the URI
 * suffix inside one handler is therefore the only option, not a stylistic
 * choice. Registered AFTER nodes_pair_post's exact "/api/v1/nodes/pair"
 * route (see api_v1_register()) so that reserved path keeps matching
 * first; only a request whose path segment isn't "pair" ever reaches this
 * wildcard handler, since httpd_find_uri_handler() returns the first
 * registered match in order. */
static esp_err_t node_post_dispatch(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/nodes/");
    uint8_t mac[6];
    if (!parse_mac12(tail, mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");
        return ESP_OK;
    }
    const char *suffix = tail + 12;

    if (strcmp(suffix, "/ota") == 0) return node_ota_start_post(req, mac);
    if (strcmp(suffix, "/ota/abort") == 0) return node_ota_abort_post(req, mac);
    return node_rename_post(req, mac);
}

/* GET /api/v1/nodes/{MAC12}/ota -- progress of the hub-wide node_ota.c
 * session, scoped to `mac`: since only one session can ever be active at a
 * time (node_ota.h), a query for a node that ISN'T the current/last
 * session's target reports an honest idle/zeroed state rather than another
 * node's numbers. Unauthenticated, like every other GET in this file
 * (nodes_get included) -- only the mutating start/abort routes above are
 * gated. New route (no existing GET wildcard under this prefix), so no
 * registration-order dependency with anything else. */
static esp_err_t node_ota_get(httpd_req_t *req)
{
    const char *tail = req->uri + strlen("/api/v1/nodes/");
    uint8_t mac[6];
    if (!parse_mac12(tail, mac) || strcmp(tail + 12, "/ota") != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    node_ota_progress_t p;
    node_ota_progress(&p);
    bool for_this_node = p.state != OTA_ST_IDLE && memcmp(p.mac, mac, 6) == 0;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "active", for_this_node && p.active);
    cJSON_AddNumberToObject(root, "state", for_this_node ? p.state : OTA_ST_IDLE);
    cJSON_AddNumberToObject(root, "sent", for_this_node ? p.sent_offset : 0);
    cJSON_AddNumberToObject(root, "acked", for_this_node ? p.acked_offset : 0);
    cJSON_AddNumberToObject(root, "total", for_this_node ? p.total_len : 0);
    cJSON_AddNumberToObject(root, "err", for_this_node ? p.err : 0);
    return send_json(req, root);
}

/* DELETE /api/v1/nodes/{MAC12} -- forget: removes the node from
 * swarm_store's persisted table AND removes the corresponding ESP-NOW peer,
 * so a forgotten node cannot keep sending encrypted frames this hub would
 * otherwise still decrypt. Chose DELETE over a POST .../forget alias: it
 * needs no reserved-path precedence trick (a different method never
 * collides with nodes_pair_post's POST-only route on the same URI space,
 * regardless of registration order), and HTTP_DELETE is available in this
 * IDF's httpd_method_t (esp_http_server.h / http_parser.h). */
static esp_err_t node_forget_delete(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    uint8_t mac[6];
    if (!parse_mac12(req->uri + strlen("/api/v1/nodes/"), mac)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad mac");
        return ESP_OK;
    }

    esp_err_t err = swarm_store_forget_node(mac);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown node");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "forget failed");
        return ESP_OK;
    }

    /* Forgetting must fully forget: otherwise this node's RAM stats slot
     * stays occupied for the rest of the boot (blocking a later replacement
     * node from ever getting one, once all slots have been used) and every
     * sensor it last relayed keeps reporting it as "via" forever, since
     * is_paired_node() (swarm.c) will reject its frames from now on and
     * nothing else can ever re-attribute them. Both are in-RAM-only, no
     * NVS/flash and no send, so doing this here on the httpd task is safe. */
    swarm_forget_node_stats(mac);
    data_core_clear_node_attribution(mac);

    /* Best-effort: the security-relevant half is already done above --
     * hub_rx_cb's is_paired_node() (swarm.c) rejects a forgotten node's
     * READING frames regardless of whether the ESP-NOW peer table itself
     * still has a stale entry -- so a failure here is logged, not fatal to
     * the request. */
    esp_err_t peer_err = espnow_link_remove_peer(mac);
    if (peer_err != ESP_OK) {
        ESP_LOGW(TAG, "forget: espnow_link_remove_peer failed: %s", esp_err_to_name(peer_err));
    }

    /* Best-effort notification (M5c, closing the M5b gap): without this, a
     * still-powered node never learns it was forgotten -- its reads are
     * dropped hub-side by is_paired_node() from now on, but the node itself
     * keeps believing it is paired and never resyncs or re-pairs on its
     * own. Fire-and-forget from a dedicated task (see swarm.c); does not
     * delay this response. A node that was powered off still needs the
     * physical BOOT-button recovery -- this doesn't change that. `mac`
     * (already parsed above) travels as the frame's target_mac so only
     * this node acts on it, not every node paired to this hub. */
    swarm_broadcast_forget(mac);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* Fires ~1.5s after the response for a role change to "node" (or a pairing
 * retry) goes out, same "let the client see the response first" pattern as
 * wifi_post's apply_creds_cb. Both cases fundamentally change the boot path
 * (radio-only wifi while searching/paired, no webserver at all once
 * paired), so they're applied via a clean reboot rather than trying to
 * tear down webserver/wifi_manager live. */
static void delayed_restart_cb(TimerHandle_t t)
{
    xTimerDelete(t, 0);
    esp_restart();
}

static void schedule_restart(const char *timer_name)
{
    TimerHandle_t t = xTimerCreate(timer_name, pdMS_TO_TICKS(1500), pdFALSE, NULL, delayed_restart_cb);
    if (t) {
        xTimerStart(t, 0);
    } else {
        /* Whatever state change the caller made is already persisted, so a
         * manual power cycle still picks it up even if we can't schedule
         * the restart. */
        ESP_LOGE(TAG, "failed to create %s timer; reboot manually to apply", timer_name);
    }
}

static esp_err_t pair_retry_post(httpd_req_t *req)
{
    if (!role_change_ok(req)) return api_send_401(req);
    if (swarm_store_set_pair_failed(false) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "retry failed");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
    schedule_restart("pair_retry");
    return ESP_OK;
}

static esp_err_t role_post(httpd_req_t *req)
{
    if (!role_change_ok(req)) return api_send_401(req);

    char body[64];
    if (req->content_len == 0 || req->content_len > sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
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
    const cJSON *role_j = cJSON_GetObjectItem(json, "role");
    swarm_role_t new_role = SWARM_ROLE_UNSET;
    bool valid = cJSON_IsString(role_j);
    if (valid) {
        if (strcmp(role_j->valuestring, "main") == 0) new_role = SWARM_ROLE_MAIN;
        else if (strcmp(role_j->valuestring, "node") == 0) new_role = SWARM_ROLE_NODE;
        else valid = false;
    }
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid role");
        return ESP_OK;
    }

    swarm_role_t old_role = swarm_store_role();
    if (swarm_store_set_role(new_role) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "role persist failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");

    /* UNSET and MAIN both already run the exact same live boot path (see
     * main.c), so a transition between only those two needs no restart --
     * that's what lets onboarding's "choose main hub" drop straight into
     * the Network tab with no reboot. But any transition touching NODE on
     * either side -- becoming one, or recovering from one via the Config
     * tab's "switch back to main hub" button on an unpaired node sitting
     * in its own portal -- changes what's actually running (webserver +
     * wifi_manager + swarm_start_main vs. radio-only ESP-NOW) and needs a
     * clean reboot to take effect. */
    if (new_role == SWARM_ROLE_NODE || old_role == SWARM_ROLE_NODE) schedule_restart("role_restart");
    return ESP_OK;
}

/* GET /api/v1/config -- unauthenticated, like every GET in this file.
 * Secrets (mqtt.pass, influx.token) are never echoed -- only *_set booleans,
 * so the webui can show "a secret is already saved" without ever pulling it
 * back down to the browser. */
static esp_err_t config_get(httpd_req_t *req)
{
    integr_config_t cfg;
    integr_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();
    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", cfg.mqtt.enabled);
    cJSON_AddStringToObject(mqtt, "uri", cfg.mqtt.uri);
    cJSON_AddStringToObject(mqtt, "user", cfg.mqtt.user);
    cJSON_AddBoolToObject(mqtt, "pass_set", cfg.mqtt.pass[0] != '\0');

    cJSON *influx = cJSON_AddObjectToObject(root, "influx");
    cJSON_AddBoolToObject(influx, "enabled", cfg.influx.enabled);
    cJSON_AddStringToObject(influx, "url", cfg.influx.url);
    cJSON_AddStringToObject(influx, "org", cfg.influx.org);
    cJSON_AddStringToObject(influx, "bucket", cfg.influx.bucket);
    cJSON_AddBoolToObject(influx, "token_set", cfg.influx.token[0] != '\0');

    return send_json(req, root);
}

static esp_err_t config_send_invalid(httpd_req_t *req)
{
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"error\":\"invalid config\"}");
    return ESP_OK;
}

/* POST /api/v1/config -- merge semantics: start from the current config; a
 * present section ("mqtt"/"influx") replaces its non-secret fields
 * wholesale (missing keys within a present section fall back to the
 * zero value, matching a full-section resubmit from the webui form); an
 * absent section leaves that whole section untouched. mqtt.pass and
 * influx.token only replace the stored secret when present AND non-empty --
 * there is no clear-secret path in v1 (disable the integration instead).
 * Config changes apply on next boot only (integrations_start() runs once at
 * boot); no live restart of esp-mqtt/influx tasks is attempted here. */
static esp_err_t config_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    /* Worst case (both sections, full field lengths, JSON-escaped) is well
     * under this; oversized bodies get 413 below rather than truncated. */
    char body[1536];
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return ESP_OK;
    }
    if (req->content_len > sizeof(body) - 1) {
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
    if (!json) return config_send_invalid(req);

    integr_config_t cfg;
    integr_config_get(&cfg);

    const cJSON *mqtt_j = cJSON_GetObjectItem(json, "mqtt");
    if (cJSON_IsObject(mqtt_j)) {
        cfg.mqtt.enabled = cJSON_IsTrue(cJSON_GetObjectItem(mqtt_j, "enabled"));
        const cJSON *uri = cJSON_GetObjectItem(mqtt_j, "uri");
        strlcpy(cfg.mqtt.uri, cJSON_IsString(uri) ? uri->valuestring : "", sizeof(cfg.mqtt.uri));
        const cJSON *user = cJSON_GetObjectItem(mqtt_j, "user");
        strlcpy(cfg.mqtt.user, cJSON_IsString(user) ? user->valuestring : "", sizeof(cfg.mqtt.user));
        const cJSON *pass = cJSON_GetObjectItem(mqtt_j, "pass");
        if (cJSON_IsString(pass) && pass->valuestring[0] != '\0')
            strlcpy(cfg.mqtt.pass, pass->valuestring, sizeof(cfg.mqtt.pass));
    }

    const cJSON *influx_j = cJSON_GetObjectItem(json, "influx");
    if (cJSON_IsObject(influx_j)) {
        cfg.influx.enabled = cJSON_IsTrue(cJSON_GetObjectItem(influx_j, "enabled"));
        const cJSON *url = cJSON_GetObjectItem(influx_j, "url");
        strlcpy(cfg.influx.url, cJSON_IsString(url) ? url->valuestring : "", sizeof(cfg.influx.url));
        const cJSON *org = cJSON_GetObjectItem(influx_j, "org");
        strlcpy(cfg.influx.org, cJSON_IsString(org) ? org->valuestring : "", sizeof(cfg.influx.org));
        const cJSON *bucket = cJSON_GetObjectItem(influx_j, "bucket");
        strlcpy(cfg.influx.bucket, cJSON_IsString(bucket) ? bucket->valuestring : "", sizeof(cfg.influx.bucket));
        const cJSON *token = cJSON_GetObjectItem(influx_j, "token");
        if (cJSON_IsString(token) && token->valuestring[0] != '\0')
            strlcpy(cfg.influx.token, token->valuestring, sizeof(cfg.influx.token));
    }
    cJSON_Delete(json);

    if (integr_config_set(&cfg) != ESP_OK) return config_send_invalid(req);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart_required\":true}");
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
    httpd_uri_t sensors = { .uri = "/api/v1/sensors", .method = HTTP_GET, .handler = sensors_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sensors));
    httpd_uri_t history = { .uri = "/api/v1/sensors/*", .method = HTTP_GET, .handler = history_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &history));
    httpd_uri_t rename = { .uri = "/api/v1/sensors/*", .method = HTTP_POST, .handler = sensor_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rename));
    httpd_uri_t timep = { .uri = "/api/v1/time", .method = HTTP_POST, .handler = time_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &timep));
    httpd_uri_t claimu = { .uri = "/api/v1/claim", .method = HTTP_POST, .handler = claim_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &claimu));
    httpd_uri_t unclaimu = { .uri = "/api/v1/unclaim", .method = HTTP_POST, .handler = unclaim_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &unclaimu));
    httpd_uri_t ota = { .uri = "/api/v1/ota", .method = HTTP_POST, .handler = ota_post_handler };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota));
    httpd_uri_t nodes = { .uri = "/api/v1/nodes", .method = HTTP_GET, .handler = nodes_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &nodes));
    httpd_uri_t nodes_pair = { .uri = "/api/v1/nodes/pair", .method = HTTP_POST, .handler = nodes_pair_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &nodes_pair));
    /* Registered AFTER nodes_pair above so that exact "/api/v1/nodes/pair"
     * route keeps winning (see node_post_dispatch()'s comment). Handles
     * rename, OTA start and OTA abort -- all under this one route, per
     * that same comment. */
    httpd_uri_t node_post = { .uri = "/api/v1/nodes/*", .method = HTTP_POST, .handler = node_post_dispatch };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &node_post));
    httpd_uri_t node_forget = { .uri = "/api/v1/nodes/*", .method = HTTP_DELETE, .handler = node_forget_delete };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &node_forget));
    httpd_uri_t node_ota_status = { .uri = "/api/v1/nodes/*", .method = HTTP_GET, .handler = node_ota_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &node_ota_status));
    httpd_uri_t role = { .uri = "/api/v1/role", .method = HTTP_POST, .handler = role_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &role));
    httpd_uri_t pair_retry = { .uri = "/api/v1/pair/retry", .method = HTTP_POST, .handler = pair_retry_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &pair_retry));
    httpd_uri_t config_g = { .uri = "/api/v1/config", .method = HTTP_GET, .handler = config_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_g));
    httpd_uri_t config_p = { .uri = "/api/v1/config", .method = HTTP_POST, .handler = config_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &config_p));
}
