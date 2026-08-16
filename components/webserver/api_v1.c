#include "api_v1.h"
#include "app_config.h"
#include "wifi_manager.h"
#include "data_core.h"
#include "devices_json.h"
#include "capability.h"
#include "storage.h"
#include "timekeeper.h"
#include "plants.h"
#include "claim.h"
#include "ota_post.h"
#include "swarm.h"
#include "swarm_store.h"
#include "ble_collector.h"
#include "node_ota.h"
#include "pairing.h"
#include "integr_config.h"
#include "espnow_link.h"
#include "rules.h"
#include "wrapper_index.h"
#include "unknown_capture.h"
#include "bthome.h"
#include "psvm.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "api_v1";
#define FW_VERSION "2.0.0-dev"

/* Shared by devices_get()/sensors_get()/plants_get()/plants_bind_post()
 * below (final M8 review, L5, carried forward by Task 6): each used to hold
 * its own private `static registry_t` + `static plants_table_t` pair
 * (~2.25KB combined, doubled for no reason). Safe to share one file-static
 * pair between them: esp_http_server invokes exactly one registered
 * handler at a time on the single httpd task (this is why each is
 * `static` -- too big for that task's stack -- in the first place), so
 * there is never a concurrent writer to race. */
static registry_t s_api_reg_snap;
static plants_table_t s_api_plant_snap;

/* mqtt_pub.c: MQTT retained-topic cleanup on plant delete / capability
 * unbind (spec Sec.6, M2 Task 7) -- same "no header of its own" convention
 * as mqtt_pub_event() there. Safe no-ops when MQTT is disabled/not
 * started. */
extern void mqtt_pub_plant_deleted(uint8_t plant_id);
extern void mqtt_pub_cap_unbound(uint8_t plant_id, uint8_t cap_id);
extern void mqtt_pub_device_cap_bound(const device_id_t *dev, uint8_t cap_id, uint8_t plant_id);

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");

    /* cJSON_PrintUnformatted returns NULL when it cannot allocate the one
     * contiguous buffer the serialised body needs. Passing that NULL to
     * httpd_resp_sendstr() ends the response cleanly, so the client receives
     * 200 OK with a ZERO-BYTE body -- a memory failure that reads as valid
     * emptiness. Observed on the M3 hardware gate: on the esp32c3, with the
     * heap fragmented after BLE bring-up, GET /api/v1/unknown returned 200
     * with no body while smaller endpoints kept working, and nothing in the
     * log said why.
     *
     * That shape of failure is worst precisely here: /api/v1/unknown is M4's
     * input contract, and an AI consumer cannot tell "this hub hears no
     * unknown devices" from "this hub ran out of memory" -- one is a fact
     * about the world, the other is a fact about the hub. Report it as the
     * server error it is. */
    if (!body) {
        ESP_LOGW(TAG, "response serialisation failed (out of contiguous heap), sending 503");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory serialising response\"}");
    }

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

/* GET /api/v1/health -- minimal liveness/readiness probe for monitoring
 * (e.g. Uptime Kuma). Unauthenticated like every GET here. Deliberately
 * cheap: no snapshots, no locks, no cJSON -- a monitor polling every few
 * seconds must not compete with real work. 200 = healthy; 503 = alive but
 * degraded, currently meaning "storage unmounted" (history/plants dead,
 * M1 recovery still works) or "heap critically low" (~16KB; the observed
 * healthy floor under full load is ~40KB, see PlanV1 8j). time_synced is
 * NOT a health signal -- offline operation is a designed mode. */
#define HEALTH_HEAP_CRITICAL 16384
static esp_err_t health_get(httpd_req_t *req)
{
    bool storage_ok = esp_littlefs_mounted("storage");
    uint32_t heap = esp_get_free_heap_size();
    bool healthy = storage_ok && heap >= HEALTH_HEAP_CRITICAL;

    char body[96];
    snprintf(body, sizeof(body),
             "{\"status\":\"%s\",\"storage\":%s,\"heap_free\":%u}",
             healthy ? "ok" : "degraded", storage_ok ? "true" : "false",
             (unsigned)heap);
    if (!healthy) httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body);
    return ESP_OK;
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
    bool fs_warn = false;
    if (esp_littlefs_info("storage", &fs_total, &fs_used) == ESP_OK) {
        cJSON_AddNumberToObject(root, "fs_total", fs_total);
        cJSON_AddNumberToObject(root, "fs_used", fs_used);
        /* Storage-pressure ruling (task-6-brief.md): neither chip's
         * partition can hold full history for all 16 plants, and
         * storage_append() failure degrades silently. Cheap by design --
         * no new scans, just thresholding the fs_total/fs_used this
         * handler already fetches -- with a one-shot ESP_LOGW the first
         * time usage crosses 85%, so it shows up in the log once rather
         * than on every poll of this endpoint, plus a fs_warn bool Task 8
         * renders as a UI warning. */
        if (fs_total > 0) {
            fs_warn = ((uint64_t)fs_used * 100 / fs_total) >= 85;
            static bool s_storage_warn_logged = false;
            if (fs_warn && !s_storage_warn_logged) {
                ESP_LOGW(TAG, "storage: %u/%u bytes used (%.0f%%) -- history writes may start "
                              "failing soon; oldest data goes first (ring buffer)",
                         (unsigned)fs_used, (unsigned)fs_total,
                         (double)fs_used * 100.0 / (double)fs_total);
                s_storage_warn_logged = true;
            }
        }
    }
    cJSON_AddBoolToObject(root, "fs_warn", fs_warn);
    cJSON_AddNumberToObject(root, "heap_free", esp_get_free_heap_size());
    /* M3 §1: raw-advert queue drop counter, so a saturated queue shows up
     * here instead of silently losing advertisements. */
    cJSON_AddNumberToObject(root, "adv_dropped", ble_collector_adv_dropped());
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

/* GET /api/v1/devices -- the device+capability surface (Task 6, spec
 * §6/§7): every physical device across radios, each with its live
 * capability readings (device_json(), devices_json.h) and which plants (if
 * any) currently bind it. Unauthenticated, like every GET here.
 *
 * plants_snapshot() is safe to call even when plants_init() never ran (see
 * its own doc comment) -- a pair-failed-portal NODE's webserver hits this
 * route too, and gets an all-unbound device list rather than a crash.
 *
 * `deprecated` also drives the GET /api/v1/sensors alias right below: same
 * body, plus a top-level "deprecated":true (task-6 brief: kept for one
 * milestone since it's what M1's own tooling calls). */
static cJSON *devices_root(bool deprecated)
{
    /* s_api_reg_snap/s_api_plant_snap: too big for the httpd task stack;
     * shared across this file's handlers, see their declaration comment (L5). */
    data_core_snapshot(&s_api_reg_snap);
    plants_snapshot(&s_api_plant_snap);
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        if (!s_api_reg_snap.devices[i].in_use) continue;
        cJSON_AddItemToArray(arr, device_json(&s_api_reg_snap.devices[i], &s_api_plant_snap, now_uptime_s));
    }
    if (deprecated) cJSON_AddBoolToObject(root, "deprecated", true);
    return root;
}

static esp_err_t devices_get(httpd_req_t *req)
{
    return send_json(req, devices_root(false));
}

/* GET /api/v1/sensors -- deprecated alias of GET /api/v1/devices, see
 * devices_root()'s comment above. Per-sensor history and rename routes are
 * gone (spec §4/§6) -- only POST /api/v1/sensors/{mac} (rename, still mac-keyed)
 * survives below, unchanged. */
static esp_err_t sensors_get(httpd_req_t *req)
{
    return send_json(req, devices_root(true));
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

/* Parses a decimal plant id from the start of s: one or more ASCII digits,
 * stopping at '/', '?' or the string's end; must start with a nonzero digit
 * and fit in a uint8_t (plant ids are 1-based, plants_table.h). 0 doubles as
 * this function's own "didn't parse" sentinel, the same convention
 * plants_table.h's own 0=none/full return values already use. On success,
 * *tail_out (when non-NULL) is set to the byte right after the digits --
 * '/', '?' or the terminating NUL -- so callers can dispatch on whatever
 * trailing path segment follows, same shape node_post_dispatch's suffix
 * handling below uses for mac-keyed routes.
 *
 * Stopping at '?' too (not just '/'/NUL) matters here in a way it doesn't
 * for parse_mac12(): esp_http_server's req->uri KEEPS the query string
 * (verified against this repo's installed IDF), so
 * "/api/v1/plants/3?foo=bar" would otherwise fail to parse an id at all --
 * *p would land on '?', neither '/' nor NUL, and the old strict check
 * rejected that outright. Every caller below must in turn treat a
 * *tail_out starting with '?' the same as an empty/absent suffix -- see
 * plants_post_dispatch()/plants_delete_delete() (history_get() below also
 * calls this, but against a query-parameter VALUE, which can't itself
 * contain '/' or '?', so that '?'-handling is moot there). */
static uint8_t parse_plant_id(const char *s, const char **tail_out)
{
    if (*s < '1' || *s > '9') return 0;   /* must start with a nonzero digit */
    unsigned long v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (unsigned long)(*p - '0');
        if (v > 255) return 0;
        p++;
    }
    if (*p != '\0' && *p != '/' && *p != '?') return 0;
    if (tail_out) *tail_out = p;
    return (uint8_t)v;
}

/* GET /api/v1/plants -- the primary list (M8 Task 6, spec §4).
 * Unauthenticated, like every GET here. plants_snapshot() is safe to call
 * even when plants_init() never ran (see its own doc comment in plants.c)
 * -- a pair-failed-portal NODE's webserver reaches this route too (Task 2's
 * review forward note), and gets an empty "plants":[] array rather than a
 * crash.
 *
 * Also drives the auto-create sweep (final M8 review, M3): with the old
 * sensor<->plant API bridge gone (DoS rule, see history_get()'s comment
 * below) and MQTT off by default, the sampler's periodic tick used to be
 * the ONLY driver of plant auto-creation -- a fresh hub could show "No
 * plants yet" for up to CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN minutes even
 * with a probe already reporting. plants_adopt_from_registry() is safe to
 * call from here specifically because `s_api_reg_snap` below is itself a
 * registry SNAPSHOT (data_core_snapshot()), never anything
 * request-supplied -- the DoS rule stays airtight: an unauthenticated
 * caller still can't seed a plant from data it controls, it can only nudge
 * an already-live sensor's plant into existing sooner. httpd task context;
 * sanctioned lazy driver per plants_adopt_from_registry()'s own doc comment
 * in plants.h. */
static esp_err_t plants_get(httpd_req_t *req)
{
    /* s_api_reg_snap: too big for the httpd task stack; shared across this
     * file's handlers, see its declaration comment (L5). One snapshot
     * serves both the adopt-sweep call below AND the response rendering
     * further down -- the sweep only reads the registry (never mutates
     * it), so unlike plants_snapshot() there is no reason to re-snapshot
     * after it (M2 Task 7: plants_adopt_from_registry() moved onto a real
     * registry_t*, removing the second, legacy-shaped snapshot this used
     * to need). */
    data_core_snapshot(&s_api_reg_snap);
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    plants_adopt_from_registry(&s_api_reg_snap, now_uptime_s,
                                CONFIG_PLANTHUB_SAMPLE_INTERVAL_MIN * 60);

    /* Re-snapshot the PLANTS table AFTER the sweep above (not reusing an
     * earlier one) so a plant it just created shows up in THIS response,
     * not the next poll -- the whole point of M3's latency fix. */
    plants_snapshot(&s_api_plant_snap);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "plants");
    for (int i = 0; i < PLANTS_MAX; i++) {
        if (!s_api_plant_snap.p[i].in_use) continue;
        cJSON_AddItemToArray(arr, plant_json(&s_api_plant_snap.p[i], &s_api_reg_snap));
    }
    return send_json(req, root);
}

/* Once c->failed is set we just return without sending further chunks; we
 * don't abort the underlying storage_query scan early, but that scan is
 * bounded (<=STORAGE_RAW_CAP == 2880 records) so letting it run to
 * completion costs at most a bounded, harmless amount of wasted work.
 *
 * c->map is the SAME history_map_t pointer this file's history_get() below
 * passes as storage_query()'s own map_out -- storage.h's doc comment on
 * storage_query() guarantees map_out is filled from the ring file's header
 * before the first row() call, so by the time this ever runs, c->map
 * already holds this plant's real column layout for the requested tier. */
typedef struct {
    httpd_req_t  *req;
    bool          first;
    bool          failed;
    uint8_t       cap_id;
    history_map_t map;
} api_hist_ctx_t;

static void api_hist_row(void *vctx, uint32_t epoch, const storage_rec_t *rec)
{
    api_hist_ctx_t *c = vctx;
    if (c->failed) return;
    int col = history_map_col(&c->map, c->cap_id);
    if (col < 0) return;   /* this plant never historised the requested capability */

    char line[48];
    int n;
    if (rec->col[col] == CAP_VALUE_NONE) {
        n = snprintf(line, sizeof(line), "%s[%lu,null]", c->first ? "" : ",", (unsigned long)epoch);
    } else {
        n = snprintf(line, sizeof(line), "%s[%lu,%.4f]", c->first ? "" : ",",
                     (unsigned long)epoch, (double)capability_decode(c->cap_id, rec->col[col]));
    }
    c->first = false;
    if (n >= (int)sizeof(line) || httpd_resp_sendstr_chunk(c->req, line) != ESP_OK)
        c->failed = true;
}

/* GET /api/v1/history?plant=<id>&cap=<id>&range=day|week|month -- one
 * capability's time series for one plant (spec §7 "History tab"), replacing
 * the old GET /api/v1/plants/{id}/history?tier=&from=&to= route (Task 3/5's
 * interim per-tier form): the M2 UI selects a capability, not a raw/hourly
 * tier directly -- `range` now picks BOTH the tier and the window (day ->
 * raw/24h; week/month -> hourly/7d or 30d -- the raw tier's shorter C5
 * retention, spec §3's chip-aware Kconfig table, can't cover a month).
 *
 * Deliberately does NOT auto-create (same DoS-prevention rule the old route
 * documented, see plants_get()'s comment above): an unknown/deleted plant
 * id just 404s rather than being seeded from a request-supplied id.
 *
 * "available" is every capability this plant's ring file for the chosen
 * tier has EVER historised (its column map, storage.h's history_map_t) --
 * not just its currently-bound capabilities: a plant that used to have a
 * capability bound (since cleared or re-pointed) keeps that column's
 * history, and the UI's capability selector should still offer it. */
static esp_err_t history_get(httpd_req_t *req)
{
    char query[96] = "", val[16];
    httpd_req_get_url_query_str(req, query, sizeof(query));

    if (httpd_query_key_value(query, "plant", val, sizeof(val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing plant");
        return ESP_OK;
    }
    const char *tail = NULL;
    uint8_t plant_id = parse_plant_id(val, &tail);
    if (plant_id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad plant id");
        return ESP_OK;
    }
    plants_table_t table;
    plants_snapshot(&table);
    if (plants_table_find_id(&table, plant_id) < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown plant");
        return ESP_OK;
    }

    if (httpd_query_key_value(query, "cap", val, sizeof(val)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing cap");
        return ESP_OK;
    }
    char *endptr = NULL;
    unsigned long cap_ul = strtoul(val, &endptr, 10);
    if (endptr == val || *endptr != '\0' || cap_ul >= CAPABILITY_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad cap");
        return ESP_OK;
    }
    uint8_t cap_id = (uint8_t)cap_ul;

    storage_tier_t tier = STORAGE_TIER_RAW;
    uint32_t window_s = 86400;
    const char *range = "day";
    if (httpd_query_key_value(query, "range", val, sizeof(val)) == ESP_OK) {
        if (strcmp(val, "day") == 0) { tier = STORAGE_TIER_RAW; window_s = 86400; range = "day"; }
        else if (strcmp(val, "week") == 0) { tier = STORAGE_TIER_HOURLY; window_s = 7UL * 86400; range = "week"; }
        else if (strcmp(val, "month") == 0) { tier = STORAGE_TIER_HOURLY; window_s = 30UL * 86400; range = "month"; }
        else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad range");
            return ESP_OK;
        }
    }

    bool synced = timekeeper_synced();
    uint32_t now = timekeeper_now(), to = 0, from = 0;
    if (synced) { to = now; from = now > window_s ? now - window_s : 0; }

    const capability_t *cap = capability_get(cap_id);
    httpd_resp_set_type(req, "application/json");
    char head[160];
    snprintf(head, sizeof(head),
             "{\"plant\":%u,\"cap\":%u,\"unit\":\"%s\",\"range\":\"%s\",\"tier\":\"%s\",\"synced\":%s,\"points\":[",
             plant_id, cap_id, cap ? cap->unit : "", range,
             tier == STORAGE_TIER_RAW ? "raw" : "hourly", synced ? "true" : "false");
    httpd_resp_sendstr_chunk(req, head);

    /* Called unconditionally (even when !synced, with an empty [0,0)
     * window that no real record's epoch can land in): storage_query()
     * fills ctx.map from the ring file's header regardless of whether any
     * row resolves an epoch, so this is the one call that gets both the
     * points AND "available" below, synced or not. */
    api_hist_ctx_t ctx = { .req = req, .first = true, .failed = false, .cap_id = cap_id };
    history_map_init(&ctx.map);
    storage_query("/storage", plant_id, tier, from, to, resolve_shim, NULL, api_hist_row, &ctx, &ctx.map);
    /* A chunk send already failed (client/socket gone) -- don't send the
     * trailing chunks over a dead connection, and return non-OK so
     * esp_http_server closes the session instead of believing it's still
     * alive. */
    if (ctx.failed) return ESP_FAIL;

    cJSON *avail = cJSON_CreateArray();
    for (int i = 0; i < HISTORY_COLS; i++) {
        if (ctx.map.cap[i] != CAP_NONE) cJSON_AddItemToArray(avail, cJSON_CreateNumber(ctx.map.cap[i]));
    }
    char *avail_str = cJSON_PrintUnformatted(avail);
    cJSON_Delete(avail);
    httpd_resp_sendstr_chunk(req, "],\"available\":");
    httpd_resp_sendstr_chunk(req, avail_str ? avail_str : "[]");
    free(avail_str);
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunked response */
    return ESP_OK;
}

/* GET /api/v1/capabilities -- capability.h's build-time table as JSON (spec
 * §6: "every downstream surface ... reads metadata from this table" -- the
 * UI is one of those surfaces, rendering unit/precision per capability
 * rather than hardcoding metric names). Static data, no snapshot needed. */
static esp_err_t capabilities_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "capabilities");
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
        const capability_t *c = capability_get(i);
        if (!c) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", c->id);
        cJSON_AddStringToObject(o, "name", c->name);
        cJSON_AddStringToObject(o, "unit", c->unit);
        cJSON_AddNumberToObject(o, "precision", c->precision);
        if (c->ha_device_class) cJSON_AddStringToObject(o, "ha_device_class", c->ha_device_class);
        else cJSON_AddNullToObject(o, "ha_device_class");
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, root);
}

/* GET /api/v1/notice / POST /api/v1/notice/dismiss -- the clean-start
 * notice (spec §5): data_fmt_apply() (app_config.h re-exports data_fmt.h)
 * latches this in NVS on a first-V2-boot wipe; the UI shows it once until
 * dismissed. */
static esp_err_t notice_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "pending", data_fmt_notice_pending());
    if (data_fmt_notice_pending()) {
        cJSON_AddStringToObject(root, "message",
            "Plant and history data did not carry over from the previous firmware version. "
            "WiFi, claim, pairing and rules were preserved.");
    }
    return send_json(req, root);
}

static esp_err_t notice_dismiss_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    data_fmt_dismiss_notice();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/plants {} -- pre-create an empty, bindingless plant (e.g.
 * before its physical device even exists yet -- plants_create() never
 * takes a binding, only the .../bind route below does). No body fields are
 * read (the spec's contract is a literal `{}`); nothing to validate,
 * mirroring nodes_pair_post's body-ignoring POST above. 409
 * {"error":"plant table full"} when all 16 (PLANTS_MAX) slots are taken. */
static esp_err_t plants_create_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    uint8_t id = plants_create();
    if (id == 0) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"plant table full\"}");
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "id", id);
    return send_json(req, root);
}

/* POST /api/v1/plants/{id} {"name":...} -- rename. Same body-reading idiom
 * every mutating POST handler in this file uses (role_post et al: fixed
 * stack buffer, explicit content_len bounds, 413 on oversize). Auth is
 * already checked by plants_post_dispatch() below before this is ever
 * reached. */
static esp_err_t plants_rename_post(httpd_req_t *req, uint8_t id)
{
    char body[128];
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
    if (!cJSON_IsString(name)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    esp_err_t err = plants_rename(id, name->valuestring);
    cJSON_Delete(json);

    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown plant");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        /* Covers plants_rename()'s ESP_ERR_INVALID_ARG (name too long, >32
         * chars) and a persist_table() NVS-write failure alike -- same
         * "collapse into 400" convention node_update_post above uses for
         * swarm_store_set_node_name()'s non-NOT_FOUND errors. */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/plants/{id}/bind {"cap":<id>|null,"device":"<id-string>"|null}
 * -- per-capability binding (spec §4/§7), replacing V1's single-probe
 * "/probe" route (task-6 brief step 2: removed in favour of this one).
 * Four body shapes:
 *   cap given, device given -> bind that ONE capability to that device
 *     (plants_bind_cap()); rebinding an already-bound capability replaces
 *     its device outright.
 *   cap given, device: null -> clear that ONE capability's binding
 *     (plants_bind_cap(..., NULL)).
 *   cap: null, device given -> bind-WHOLE-device: every capability
 *     `device` currently reports (plants_bind_device()), spec §4's
 *     one-click V1-parity flow.
 *   cap: null, device: null -> 400: plants.h has no "unbind every
 *     capability of this device" primitive, so there is nothing to do.
 * A device id that isn't well-formed (device_id_parse() rejects it) is 400
 * regardless of which branch it would have taken. 404 unknown plant.
 *
 * Unlike plants_resolve_or_create() (NEVER fed a request-supplied device
 * id -- see history_get()'s comment above and the Task 3 review's binding
 * table-exhaustion finding it cites), this endpoint deliberately DOES
 * accept any well-formed device id from the request body, including one
 * the radio has never heard: that's the whole point of being able to
 * pre-bind a replacement device before it has ever transmitted a reading.
 * This is safe specifically because the route is authenticated (checked
 * once by plants_post_dispatch() before this is ever reached) -- an
 * attacker can't hit it for free the way an open GET could -- and neither
 * plants_bind_cap() nor plants_bind_device() ever CREATES a plant, only
 * (re)points bindings on an existing one. */
static esp_err_t plants_bind_post(httpd_req_t *req, uint8_t id)
{
    plants_table_t table;
    plants_snapshot(&table);
    if (plants_table_find_id(&table, id) < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown plant");
        return ESP_OK;
    }

    char body[96];
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
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
        return ESP_OK;
    }
    const cJSON *cap_j = cJSON_GetObjectItem(json, "cap");
    const cJSON *dev_j = cJSON_GetObjectItem(json, "device");
    bool cap_null = (!cap_j || cJSON_IsNull(cap_j));
    bool dev_null = (!dev_j || cJSON_IsNull(dev_j));

    uint8_t cap_id = 0;
    if (!cap_null) {
        if (!cJSON_IsNumber(cap_j) || cap_j->valuedouble < 0 || cap_j->valuedouble >= CAPABILITY_COUNT) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad cap");
            return ESP_OK;
        }
        cap_id = (uint8_t)cap_j->valuedouble;
    }

    device_id_t dev = {0};
    if (!dev_null) {
        if (!cJSON_IsString(dev_j) || !device_id_parse(dev_j->valuestring, &dev)) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device id");
            return ESP_OK;
        }
    }
    cJSON_Delete(json);

    bool ok;
    bool unbind = false;
    bool single_bind = false;
    bool whole_device_bind = false;
    if (!cap_null && !dev_null) {
        ok = plants_bind_cap(id, cap_id, &dev);
        single_bind = ok;
    } else if (!cap_null) {   /* dev_null: unbinding one capability */
        ok = plants_bind_cap(id, cap_id, NULL);
        unbind = true;
    } else if (!dev_null) {   /* cap_null */
        /* s_api_reg_snap: too big for the httpd task stack; shared across
         * this file's handlers, see its declaration comment (L5). */
        data_core_snapshot(&s_api_reg_snap);
        ok = plants_bind_device(id, &dev, &s_api_reg_snap) > 0;
        whole_device_bind = ok;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "cap and device both null");
        return ESP_OK;
    }

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bind failed");
        return ESP_OK;
    }
    /* Retained-topic cleanup (spec Sec.6, M2 Task 7):
     *   - a true unbind (cap given, device null) drops that capability's
     *     plant-form HA discovery entity -- see mqtt_pub_cap_unbound()'s
     *     doc comment. A plain rebind (cap AND device given, different
     *     device) keeps the SAME plant-form entity, just pointed at a new
     *     device, so no cleanup is needed there for the plant side.
     *   - either bind shape (single-cap or whole-device) can newly cover a
     *     capability that previously had no plant binding at all -- its
     *     DEVICE-form entity (if the amended spec's dedup rule ever
     *     published one) is now a duplicate and must be cleared
     *     (mqtt_pub_device_cap_bound()). Fired unconditionally on every
     *     successful bind (not just "first time bound"): plants_bind_cap()/
     *     plants_bind_device() don't report whether a capability was
     *     already covered before this call, and re-clearing an
     *     already-cleared/never-sent device-form topic is a free no-op --
     *     see cleanup_device_cap_discovery()'s own doc comment. Whole-device
     *     bind covers every capability `dev` currently reports
     *     (plants_bind_device()'s own contract) -- s_api_reg_snap (just
     *     snapshotted above for that call) already has exactly that set,
     *     via the same `caps[cap].valid` test plants_bind_device() itself
     *     used. */
    if (unbind) mqtt_pub_cap_unbound(id, cap_id);
    if (single_bind) mqtt_pub_device_cap_bound(&dev, cap_id, id);
    if (whole_device_bind) {
        int ridx = registry_find(&s_api_reg_snap, &dev);
        if (ridx >= 0) {
            const device_entry_t *de = &s_api_reg_snap.devices[ridx];
            for (uint8_t c = 0; c < CAPABILITY_COUNT; c++) {
                if (de->caps[c].valid) mqtt_pub_device_cap_bound(&dev, c, id);
            }
        }
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/plants/{id}[/bind] -- rename or bind a capability, both
 * under the SAME registered wildcard route ("/api/v1/plants/" + wildcard),
 * for the exact ESP-IDF wildcard-registration reason node_post_dispatch's
 * own long comment above explains in full: only one wildcard template can
 * ever be registered per method under a given prefix -- a second, more
 * specific one would be rejected outright at registration time, since the
 * existing wildcard already "covers" it. Unlike nodes, there's no separate
 * reserved exact-path route competing for this prefix (no
 * "/api/v1/plants/pair" equivalent needing to win over the wildcard the
 * way nodes_pair_post's "/api/v1/nodes/pair" does) -- every POST under
 * "/api/v1/plants/" is either a bare id (rename) or an id + "/bind", both
 * parsed and dispatched right here. Auth is checked once here, not in
 * either sub-handler, mirroring node_post_dispatch's own single
 * top-of-function check.
 *
 * Suffix matching tolerates a trailing query string ('?...') on either
 * branch, same fix as history_get()'s query parsing needed -- req->uri
 * keeps it, so "/3/bind?x=y" or "/3?x=y" must not be treated as an
 * unrecognised suffix. A real-world POST is unlikely to carry a query
 * string, but being consistent here costs nothing. */
static esp_err_t plants_post_dispatch(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/plants/");
    const char *suffix = NULL;
    uint8_t id = parse_plant_id(tail, &suffix);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (strncmp(suffix, "/bind", 5) == 0 && (suffix[5] == '\0' || suffix[5] == '?'))
        return plants_bind_post(req, id);
    if (suffix[0] == '\0' || suffix[0] == '?') return plants_rename_post(req, id);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return ESP_OK;
}

/* DELETE /api/v1/plants/{id} -- deletes the plant and its history ring
 * files (plants_delete()); its sensor, if any, becomes unassigned (its
 * registry entry is untouched -- deleting a plant is not the same as
 * forgetting a sensor, which stays discoverable and can be assigned to a
 * different/new plant afterwards).
 *
 * MEDIUM fix (review): the suffix after the id used to be silently
 * discarded, so DELETE /api/v1/plants/3/anything deleted plant 3 -- a
 * destructive route accepting an arbitrary trailing path. Require the
 * suffix be empty or query-string-only, exactly like plants_post_dispatch()
 * above; anything else 404s rather than deleting. */
static esp_err_t plants_delete_delete(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/plants/");
    const char *suffix = NULL;
    uint8_t id = parse_plant_id(tail, &suffix);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (suffix[0] != '\0' && suffix[0] != '?') {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    esp_err_t err = plants_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown plant");
        return ESP_OK;
    }
    /* Retained-topic cleanup (spec Sec.6, M2 Task 7): fired regardless of
     * the persist outcome checked below, same as plants_delete()'s own
     * remove_ring_files()/storage_drop() -- a persist failure still means
     * the RAM table (what plants_get() etc. actually serve) already
     * dropped the plant; see plants_delete()'s doc comment. */
    mqtt_pub_plant_deleted(id);
    if (err != ESP_OK) {
        /* Unlike rename/probe's "collapse into 400" convention: there is no
         * client-input interpretation of a delete failure here (the id was
         * valid, plants_table_delete() itself is infallible for a known id)
         * -- only a persist_table() NVS-write failure reaches this branch,
         * a genuine server-side problem. */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "delete failed");
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

/* M7: power_mode's wire string -> swarm_power_mode_t. Mirrors (does not
 * share code with) swarm.c's power_mode_str() -- see that function's
 * comment for why this stays a small independent table rather than a
 * shared header. Only the three exact strings below are accepted; anything
 * else (wrong type, typo, unknown value) returns false so the caller can
 * answer 400 "bad power_mode" per the HTTP contract. */
static bool power_mode_from_str(const char *s, swarm_power_mode_t *out)
{
    if (!s) return false;
    if (strcmp(s, "always_on") == 0)  { *out = SWARM_PM_ALWAYS_ON;  return true; }
    if (strcmp(s, "battery_15") == 0) { *out = SWARM_PM_BATTERY_15; return true; }
    if (strcmp(s, "battery_60") == 0) { *out = SWARM_PM_BATTERY_60; return true; }
    return false;
}

/* POST /api/v1/nodes/{MAC12} body {"name":"...","power_mode":"..."} --
 * rename (empty name clears) and/or set the node's DESIRED power mode
 * (swarm_store_set_node_desired_mode(), Task 3 -- what GET /api/v1/nodes'
 * "power_mode"/"power_mode_pending" fields, added alongside this handler,
 * report). Either field may be omitted entirely; but a field that IS
 * present must be well-formed -- a present-but-wrong-type "name" or
 * "power_mode" always 400s (see the two checks below), it is never
 * silently ignored just because the other field is valid. At least one of
 * a valid "name" or a valid "power_mode" must be present, same as the
 * name-only contract this handler had before M7. Both fields are
 * validated before any store write, so a bad field never leaves a
 * partially-applied rename/mode-change behind (400, body untouched) --
 * unknown mac is checked by the store calls themselves and reported as 404
 * either way. 128 bytes
 * comfortably covers the worst case (a full 24-byte SWARM_NODE_NAME_LEN
 * name plus a "battery_15"/"battery_60" power_mode in the same body, ~61
 * bytes of JSON) with headroom. */
static esp_err_t node_update_post(httpd_req_t *req, const uint8_t mac[6])
{
    char body[128];
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
    const cJSON *pm = cJSON_GetObjectItem(json, "power_mode");

    swarm_power_mode_t mode = SWARM_PM_ALWAYS_ON;
    bool have_mode = false;
    if (pm != NULL) {
        if (!cJSON_IsString(pm) || !power_mode_from_str(pm->valuestring, &mode)) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad power_mode");
            return ESP_OK;
        }
        have_mode = true;
    }
    /* Symmetric with power_mode above: a PRESENT-but-wrong-type field is a
     * malformed request (400), not "not requested" -- only outright
     * absence (cJSON_GetObjectItem returning NULL) means the caller didn't
     * intend to touch that field at all. This was the pre-M7 contract for
     * "name" (this route unconditionally 400'd "invalid name" whenever the
     * body's "name" wasn't a string) and adding power_mode must not weaken
     * it: a present-but-malformed name must 400 even when a valid
     * power_mode also came along in the same body, exactly like a
     * present-but-malformed power_mode above 400s regardless of "name". */
    bool have_name = false;
    if (name != NULL) {
        if (!cJSON_IsString(name)) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
            return ESP_OK;
        }
        have_name = true;
    }

    if (!have_mode && !have_name) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid name");
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if (have_name) err = swarm_store_set_node_name(mac, name->valuestring);
    if (err == ESP_OK && have_mode) err = swarm_store_set_node_desired_mode(mac, mode);
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
    return node_update_post(req, mac);
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

    /* M7: p.state carries NODE_OTA_ST_PENDING_WAKE (4, node_ota.h) verbatim
     * when node_ota_start() parked this session waiting for the target
     * battery node's next CHECKIN -- it lives in the same uint8_t as the
     * wire OTA_ST_* values (swarm_frame.h, which only defines 0-3) and
     * needs no extra mapping here to reach the client as "state":4. This
     * handler intentionally has no opinion on what state 4 MEANS to a
     * human; that wording belongs to the UI (Task 7), same as every other
     * numeric state value already returned here. */
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

    /* (Reviewer HIGH, M9) A role change must also drop any operator-set
     * region. Without this, a device that had a region set from its
     * PREVIOUS role -- e.g. a hub with "US" set, converted to a node --
     * keeps that region at TOP precedence in espnow_link.c's country
     * setup, permanently outranking the country it should instead LEARN
     * from its new hub's PAIR_ACK once it pairs (PlanV1 3.3's
     * inheritance). If the new hub happens to sit on a channel the stale
     * region forbids (e.g. a leftover "US" region blocking channels
     * 12-13), the node's very first pairing sweep silently never reaches
     * it -- no error anywhere except the pair-failed portal, with nothing
     * in it pointing at region as the cause. Clearing here means every
     * role change starts from a clean slate and correctly inherits from
     * whatever hub it actually pairs with next. Best-effort: a failure to
     * clear is logged, not surfaced as a 500 -- the role change itself
     * already succeeded and must not be rolled back over this. */
    char cleared_region[3];
    bool had_region = swarm_store_region(cleared_region);
    esp_err_t region_clear_err = swarm_store_set_region("");
    if (region_clear_err != ESP_OK) {
        ESP_LOGW(TAG, "role_post: failed to clear operator region on role change: %s",
                 esp_err_to_name(region_clear_err));
    } else if (had_region) {
        ESP_LOGI(TAG, "role_post: cleared stale operator region %s on role change (%d -> %d)",
                 cleared_region, old_role, new_role);
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
 * back down to the browser.
 *
 * "region" (M9) is top-level, not nested under mqtt/influx -- it's a
 * swarm_store-backed device setting (espnow_link.c's country precedence),
 * not an integr_config_t field, so it's read separately here. null means
 * "unset" (build default / learned hub country applies, see
 * swarm_store.h); one of the four codes the webui offers otherwise. */
static esp_err_t config_get(httpd_req_t *req)
{
    integr_config_t cfg;
    integr_config_get(&cfg);

    cJSON *root = cJSON_CreateObject();

    char hubname[16];
    app_config_hub_name(hubname);
    cJSON_AddStringToObject(root, "name", hubname);

    char region[3];
    if (swarm_store_region(region)) {
        cJSON_AddStringToObject(root, "region", region);
    } else {
        cJSON_AddNullToObject(root, "region");
    }

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
 * boot, the AP SSID and the swarm's regulatory domain are likewise fixed at
 * their init) -- so instead of attempting any live restart of esp-mqtt /
 * influx / wifi, a successful save schedules a clean reboot, same
 * schedule_restart() pattern as role_post. The response says so
 * ("rebooting":true) and the webui reloads itself after the hub is back. */
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

    /* "name": optional top-level hub rename. Validation (charset, length)
     * lives in app_config_set_hub_name(); the name feeds the setup-AP SSID
     * and the MQTT topic prefix, both fixed at init -- the reboot below is
     * what applies it. Absent key leaves the name untouched; "" clears
     * back to the MAC-derived default. */
    const cJSON *name_j = cJSON_GetObjectItem(json, "name");
    if (name_j) {
        esp_err_t name_err = cJSON_IsString(name_j)
            ? app_config_set_hub_name(name_j->valuestring) : ESP_ERR_INVALID_ARG;
        if (name_err != ESP_OK) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"bad name: letters, digits, - and _ only, max 15\"}");
            return ESP_OK;
        }
    }

    /* "region" (M9): top-level, like GET's response -- validated and
     * applied BEFORE mqtt/influx below, so an invalid region 400s cleanly
     * without touching them. null or "" clears back to unset; one of the
     * four codes swarm_store_set_region() accepts sets it (that's also
     * where the actual validation lives -- see its comment for why a
     * strict length check matters for HTTP-sourced input specifically);
     * anything else -> ESP_ERR_INVALID_ARG -> 400. Absent key leaves it
     * untouched (same merge semantics as the mqtt/influx sections below).
     * Lives in its own swarm_store NVS key, independent of integr_config_t
     * below it, so this write's success/failure is not coupled to
     * mqtt/influx's -- consistent with how every other swarm_store field
     * (role, hub, hub_cc, ...) is already its own independently-committed
     * key. */
    const cJSON *region_j = cJSON_GetObjectItem(json, "region");
    if (region_j) {
        const char *region_cc = cJSON_IsString(region_j) ? region_j->valuestring
                               : cJSON_IsNull(region_j)   ? ""
                               : NULL;  /* neither string nor null: always invalid */
        esp_err_t region_err = region_cc ? swarm_store_set_region(region_cc) : ESP_ERR_INVALID_ARG;
        if (region_err == ESP_ERR_INVALID_ARG) {
            cJSON_Delete(json);
            httpd_resp_set_status(req, "400 Bad Request");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"bad region\"}");
            return ESP_OK;
        }
        if (region_err != ESP_OK) {
            /* NVS write failure, not a bad request -- logged, not surfaced
             * as an error response, matching how the rest of this file
             * treats swarm_store write failures (e.g. role_post()'s
             * swarm_store_set_role() is the only one that 500s; every
             * other setter here just logs and lets the RAM cache stand
             * for the rest of this boot). */
            ESP_LOGW(TAG, "config_post: swarm_store_set_region failed: %s", esp_err_to_name(region_err));
        }
    }

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
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    schedule_restart("cfg-restart");
    return ESP_OK;
}

/* POST /api/v1/sensors/{MAC12} {"name":"..."} -- probe display name, NVS-
 * backed via app_config_set_sensor_name() (<=32 chars, "" clears). Any
 * well-formed mac is accepted, registry-known or not: names key off the
 * mac alone, and pre-naming a probe that hasn't been heard yet is
 * harmless (the same reasoning that lets plants pre-assign one). Purely
 * cosmetic -- plant identity/history never key off this. */
static esp_err_t sensors_rename_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/sensors/");
    uint8_t mac[6];
    if (!parse_mac12(tail, mac) || (tail[12] != '\0' && tail[12] != '?')) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown sensors route");
        return ESP_OK;
    }

    char body[128];
    if (req->content_len == 0 || req->content_len > sizeof(body) - 1) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    int r = httpd_req_recv(req, body, req->content_len);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
        return ESP_OK;
    }
    body[r] = '\0';

    cJSON *json = cJSON_Parse(body);
    const cJSON *name = json ? cJSON_GetObjectItem(json, "name") : NULL;
    esp_err_t err = cJSON_IsString(name)
        ? app_config_set_sensor_name(mac, name->valuestring) : ESP_ERR_INVALID_ARG;
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"bad name (max 32 chars)\"}");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/factory_reset {"wipe_data":bool} -- network-triggered
 * factory reset, claimed hubs only. Two deliberate gates: a 403 when the
 * hub is unclaimed (an unclaimed hub's mutations are otherwise open, but
 * "anyone on the LAN can wipe the device" is not an acceptable default --
 * the BOOT hold and rapid power-cycling remain the unclaimed recovery
 * paths), then the usual 401 bearer check against the claim key.
 *
 * wipe_data=false matches the BOOT-hold reset exactly: claim, WiFi and
 * swarm role/pairings cleared; plants, history, integrations config and
 * names survive. wipe_data=true is the full return-to-fresh-flash: format
 * the LittleFS data partition and erase the whole NVS partition (claim,
 * wifi, swarm, integrations, region, all names -- boot re-inits an erased
 * NVS from scratch). The erase happens inline before the response; the
 * 1.5s restart window after it is accepted -- hub-side NVS/FS writers are
 * event-driven and rare, same pragmatism as the BOOT-hold path. */
static esp_err_t factory_reset_post(httpd_req_t *req)
{
    if (!claim_is_claimed()) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"factory reset over the network requires a claimed hub\"}");
        return ESP_OK;
    }
    if (!api_auth_ok(req)) return api_send_401(req);

    bool wipe = false;
    char body[128];
    if (req->content_len > 0 && req->content_len < sizeof(body)) {
        int r = httpd_req_recv(req, body, req->content_len);
        if (r > 0) {
            body[r] = '\0';
            cJSON *json = cJSON_Parse(body);
            if (json) {
                wipe = cJSON_IsTrue(cJSON_GetObjectItem(json, "wipe_data"));
                cJSON_Delete(json);
            }
        }
    }

    ESP_LOGW(TAG, "factory reset via api (wipe_data=%d)", (int)wipe);
    claim_reset();
    if (wipe) {
        esp_err_t fs_err = esp_littlefs_format("storage");
        if (fs_err != ESP_OK) ESP_LOGE(TAG, "littlefs format failed: %s", esp_err_to_name(fs_err));
        esp_err_t nvs_err = nvs_flash_erase();
        if (nvs_err != ESP_OK) ESP_LOGE(TAG, "nvs erase failed: %s", esp_err_to_name(nvs_err));
    } else {
        app_config_clear_wifi();
        swarm_store_reset_all();
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    schedule_restart("freset-restart");
    return ESP_OK;
}

/* ---- Rules + events HTTP API (Task 6, spec §6) --------------------------
 *
 * Note: GET /api/v1/events itself is NOT registered here -- it's the SAME
 * URI as the SSE stream sse.c already owns, and ESP-IDF's httpd rejects an
 * exact-URI duplicate registration for the same method
 * (ESP_ERR_HTTPD_HANDLER_EXISTS). sse.c's events_get() branches on the
 * "after" query key instead; see its own comment. Every other route below
 * follows this file's existing auth/body-size/404 idioms verbatim (mirror
 * of the plants_ and node_ handlers above).
 */

/* Shared, static: too big for the 8KB httpd task stack, and (like
 * s_api_reg_snap/s_api_plant_snap above) safe to share because
 * esp_http_server only ever runs one registered handler at a time on this
 * one task.
 *
 * The four buffers right below (s_http_body/s_http_source/s_http_psbc/
 * s_http_list) go one step further than a rules-only share: each is shared
 * BETWEEN the rules API (this block) and the wrappers API further down this
 * file (M1's rules routes and M3's wrappers routes each used to allocate a
 * full private set of these, doubling ~16KB of static RAM for nothing --
 * M3 hardware gate, ram-reclaim task). Same invariant as every other shared
 * static in this file -- single-task sequential httpd dispatch means two
 * handlers can never be mid-flight together -- PLUS a second condition
 * specific to sharing ACROSS the rules/wrappers boundary: verified (below,
 * per buffer) that no single handler invocation needs two of a buffer's
 * aliases live at once, and that no pointer into any of these buffers is
 * ever retained past its filling handler's httpd_resp_send*() call. Adding
 * a new handler onto one of these: re-check both conditions against the
 * owner list in that buffer's own comment, or give your handler a private
 * buffer instead. */
static rules_test_ref_t    s_rules_test_refs[PSVM_MAX_REFS];
static rules_test_action_t s_rules_test_acts[16];   /* generous cap for a dry-run capture; extra actions beyond this are silently not captured (rules_engine.c's capture_sink), never a crash */

/* Request body scratch. Owners (each its own httpd handler invocation, never
 * nested): rules_upsert_from_body() [POST/PUT /api/v1/rules[/{id}]],
 * wrapper_upsert_from_body() [POST/PUT /api/v1/wrappers[/{id}]],
 * wrappers_test_post() [POST /api/v1/wrappers/{id}/test, optional {hex?}
 * body]. Every bound check against this buffer below uses sizeof(...) on
 * the variable itself, never a macro, so the merge changes nothing there;
 * both original sizes were 8192 regardless (rules and wrappers reasoning is
 * identical): name?(<=48)+source(<=4096, JSON-escaped -- worst realistic
 * case around 1.5x for compiler-produced source, not the 2x pathological
 * worst case) + bytecode_b64 (<=~2732 chars for a 2048-byte PSBC) + a few
 * numeric/bool fields. 8192 is generous headroom over that. */
#define HTTP_BODY_MAX 8192
static char s_http_body[HTTP_BODY_MAX];

/* Rule/wrapper source-text scratch. Owners: rules_get_one()
 * [rules_get_source()], wrappers_get_one() [wrapper_store_get_source()].
 * RULES_SRC_MAX == WRAPPER_SRC_MAX == 4096, so the +1 NUL headroom is
 * identical for both original owners. */
static char s_http_source[RULES_SRC_MAX + 1];

/* Decoded-bytecode scratch, three owners: rules_upsert_from_body() [decodes
 * bytecode_b64 for a rule], wrapper_upsert_from_body() [same, for a
 * wrapper], wrappers_test_post() [reads an EXISTING wrapper's bytecode for
 * a dry run]. Each is a separate httpd handler invocation -- none ever runs
 * nested inside another -- and none stashes a pointer into this buffer
 * past its own httpd_resp_send*() call, so all three folding into one
 * buffer is safe. RULES_PSBC_MAX == WRAPPER_PSBC_MAX == 2048.
 *
 * wrappers_test_post()'s read is deliberately NOT the shared wrapper arena
 * (wrapper_arena_get()): that pointer is decoder-task-exclusive
 * (wrapper_arena.h's own doc comment names this exact endpoint as a caller
 * that MUST NOT touch it from the httpd task). It reads straight off
 * LittleFS via wrapper_store_read_psbc() instead into this SAME buffer --
 * VFS/LittleFS's own internal locking already makes a concurrent read safe
 * against the decoder task's writes, and a manual dry-run is rare enough
 * that skipping the arena's cache costs nothing that matters. */
static uint8_t s_http_psbc[RULES_PSBC_MAX];
/* base64 length of a 2048-byte PSBC: 4*ceil(2048/3), checked BEFORE
 * decoding (brief: "validate sizes BEFORE base64 decode") so a wildly
 * oversized bytecode_b64 is rejected without ever calling into mbedtls; the
 * decode call below is still bounds-checked against sizeof(s_http_psbc)
 * regardless, as a second line of defense. Rules and wrappers land on the
 * identical value (RULES_B64_MAX == WRAPPER_B64_MAX == 2732); kept as two
 * separately-named macros (this one and WRAPPER_B64_MAX below) so each call
 * site's intent stays local rather than reaching across the file. */
#define RULES_B64_MAX 2732

/* List/lookup scratch for rules_list()/wrapper_store_list() snapshots.
 * Owners: rules_list_get(), rules_get_one(), rule_id_exists() [rules];
 * wrappers_list_get(), wrappers_get_one(), wrapper_id_exists(),
 * wrappers_test_post() [wrappers] -- see each function's body further down.
 * A union, not a flat byte array: the two owners store different element
 * types (rule_info_t vs wrapper_info_t), so a union lets each side keep
 * reading/writing its OWN natural array type with no cast, while
 * sizeof(union) is automatically the larger member (rules[RULES_MAX] ==
 * 2112 B vs wrappers[WRAPPERS_MAX] == 1792 B) -- "the shared buffer takes
 * the larger size" falls out for free. Critically, each member array is
 * STILL fully sized for its own max (RULES_MAX=16 rule_info_t /
 * WRAPPERS_MAX=16 wrapper_info_t independently), and every call site below
 * bounds itself with its own RULES_MAX/WRAPPERS_MAX constant, never
 * sizeof(s_http_list) -- so the size difference between the two owners can
 * never turn into a buffer overflow for either. */
static union {
    rule_info_t    rules[RULES_MAX];
    wrapper_info_t wrappers[WRAPPERS_MAX];
} s_http_list;

/* Parses a decimal rule id (u32, rules.h -- ids are 1-based monotonic,
 * never reused) from the start of s, stopping at '/', '?' or the string's
 * end -- same contract as parse_plant_id() above (see its comment for why
 * '?' must stop the scan too: req->uri keeps the query string in this
 * IDF). Accumulates in a 64-bit temporary so a too-long digit run is
 * detected as "doesn't fit in u32" rather than silently wrapping (unsigned
 * long is only 32 bits on this target, same width as uint32_t, so
 * comparing against a 32-bit bound while accumulating IN a 32-bit type
 * could never trigger). 0 is the "didn't parse" sentinel, same convention
 * as parse_plant_id(). */
static uint32_t parse_rule_id(const char *s, const char **tail_out)
{
    if (*s < '1' || *s > '9') return 0;
    uint64_t v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFULL) return 0;
        p++;
    }
    if (*p != '\0' && *p != '/' && *p != '?') return 0;
    if (tail_out) *tail_out = p;
    return (uint32_t)v;
}

/* POST body's "name" default (spec §6: "Name defaults from source"): pulls
 * the quoted name out of source's `rule "<name>"` header line (spec §1).
 * Copies up to outlen-1 bytes; false (out untouched) if the pattern isn't
 * found or would be empty -- rules_upsert_from_body()'s caller then falls
 * back to a placeholder, and rules_upsert()'s own name validation is what's
 * actually authoritative regardless. */
static bool name_from_source(const char *source, char *out, size_t outlen)
{
    const char *p = strstr(source, "rule \"");
    if (!p || outlen == 0) return false;
    p += 6;
    size_t i = 0;
    while (p[i] != '"' && p[i] != '\0' && i < outlen - 1) i++;
    if (p[i] != '"' || i == 0) return false;
    memcpy(out, p, i);
    out[i] = '\0';
    return true;
}

/* Short human text for a psvm_err_t (mirrors rules_store.c's own private
 * psvm_err_str() -- that one isn't exported via rules.h, and psvm_err_t's
 * value set is a fixed, spec'd contract (psvm.h), so duplicating this small
 * switch here is safe rather than fragile). Only used for last_error below
 * when a rule IS ready to resolve but its most recent run itself errored
 * (division by zero, step budget, ...) -- distinct from not_ready_reason,
 * which covers unresolved refs. */
static const char *psvm_err_short(psvm_err_t e)
{
    switch (e) {
    case PSVM_OK:            return "";
    case PSVM_ERR_HEADER:    return "bad bytecode header";
    case PSVM_ERR_LIMITS:    return "bytecode exceeds a hub limit";
    case PSVM_ERR_TRUNCATED: return "bytecode truncated";
    case PSVM_ERR_BADOP:     return "bad opcode";
    case PSVM_ERR_STACK:     return "stack error";
    case PSVM_ERR_STEPS:     return "step budget exceeded";
    case PSVM_ERR_DIV0:      return "division by zero";
    case PSVM_ERR_JUMP:      return "bad jump target";
    case PSVM_ERR_TYPE:      return "type error";
    case PSVM_ERR_REF:       return "bad reference";
    default:                 return "vm error";
    }
}

/* Shared by rules_list_get()/rules_get_one() -- the meta+status fields
 * common to both spec §6 shapes (GET /rules/<id> adds "source" on top of
 * this). last_eval_ts/last_fire_ts in rule_info_t are MONOTONIC uptime
 * seconds, never epoch (Task 5 review finding -- see rules.h's own
 * comment): exposed here as ages (seconds since eval/fire) rather than raw
 * timestamps, so the API can never be mistaken for emitting epoch time;
 * null when the rule has never evaluated/fired (ts==0). */
static cJSON *rule_status_json(const rule_info_t *r, uint32_t now_uptime_s)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", r->id);
    cJSON_AddStringToObject(o, "name", r->name);
    cJSON_AddBoolToObject(o, "enabled", r->enabled);
    cJSON_AddNumberToObject(o, "mode", r->mode);
    cJSON_AddNumberToObject(o, "cooldown_s", r->cooldown_s);
    cJSON_AddNumberToObject(o, "every_s", r->every_s);
    cJSON_AddBoolToObject(o, "ready", r->ready);
    /* ready implies last_err == PSVM_OK (rules_engine.c never sets one
     * without the other) -- so when not ready, prefer the specific VM error
     * text over the generic "evaluation error" not_ready_reason whenever
     * one is available; otherwise fall back to not_ready_reason (e.g. "no
     * plant X"), which is always populated when !ready. */
    const char *last_error = "";
    if (!r->ready) last_error = (r->last_err != PSVM_OK) ? psvm_err_short(r->last_err) : r->not_ready_reason;
    cJSON_AddStringToObject(o, "last_error", last_error);
    if (r->last_eval_ts != 0) cJSON_AddNumberToObject(o, "last_eval_age_s", now_uptime_s - r->last_eval_ts);
    else cJSON_AddNullToObject(o, "last_eval_age_s");
    if (r->last_fire_ts != 0) cJSON_AddNumberToObject(o, "last_fire_age_s", now_uptime_s - r->last_fire_ts);
    else cJSON_AddNullToObject(o, "last_fire_age_s");
    cJSON_AddNumberToObject(o, "fire_count", r->fire_count);
    return o;
}

/* GET /api/v1/rules -- unauthenticated, like every GET in this file. */
static esp_err_t rules_list_get(httpd_req_t *req)
{
    size_t n = rules_list(s_http_list.rules, RULES_MAX);
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "rules");
    for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(arr, rule_status_json(&s_http_list.rules[i], now_uptime_s));
    return send_json(req, root);
}

/* GET /api/v1/rules/{id} -- meta + source + status (spec §6). Unauthenticated. */
static esp_err_t rules_get_one(httpd_req_t *req)
{
    const char *tail = req->uri + strlen("/api/v1/rules/");
    const char *suffix = NULL;
    uint32_t id = parse_rule_id(tail, &suffix);
    if (id == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    size_t n = rules_list(s_http_list.rules, RULES_MAX);
    const rule_info_t *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (s_http_list.rules[i].id == id) { found = &s_http_list.rules[i]; break; }
    }
    if (!found) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown rule");
        return ESP_OK;
    }

    if (!rules_get_source(id, s_http_source, sizeof(s_http_source))) s_http_source[0] = '\0';

    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    cJSON *root = rule_status_json(found, now_uptime_s);
    cJSON_AddStringToObject(root, "source", s_http_source);
    return send_json(req, root);
}

/* True iff id currently names a rule -- used by rules_update_put() to 404
 * before ever calling rules_upsert() (which folds "unknown id" into the
 * same ESP_ERR_INVALID_ARG as every other validation failure, not
 * distinguishable from its errbuf text alone). No race with a concurrent
 * delete: esp_http_server runs one handler at a time on this task. */
static bool rule_id_exists(uint32_t id)
{
    size_t n = rules_list(s_http_list.rules, RULES_MAX);
    for (size_t i = 0; i < n; i++) if (s_http_list.rules[i].id == id) return true;
    return false;
}

/* Sends a 400 with a properly JSON-escaped {"error":"<errbuf>"} body --
 * NOT a hand-rolled snprintf: rules_upsert()'s own errbuf text can contain
 * a literal '"' (its name-validation message is `...must not contain '"'`),
 * which would break hand-rolled JSON syntax outright. */
static void send_rules_error(httpd_req_t *req, const char *errbuf)
{
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *eroot = cJSON_CreateObject();
    cJSON_AddStringToObject(eroot, "error", errbuf);
    char *ebody = cJSON_PrintUnformatted(eroot);
    cJSON_Delete(eroot);
    httpd_resp_set_type(req, "application/json");
    if (ebody) {
        httpd_resp_sendstr(req, ebody);
        free(ebody);
    } else {
        httpd_resp_sendstr(req, "{\"error\":\"invalid rule\"}");
    }
}

/* Shared by rules_create_post() (POST /api/v1/rules, id_inout starts at 0)
 * and rules_update_put() (PUT /api/v1/rules/{id}, id_inout already set to
 * the URL's id): reads+validates the {name?, source, bytecode_b64, enabled,
 * mode, cooldown_s, every_s} body (spec §6) and calls rules_upsert().
 * Sends the 400/413 response itself and returns false on any failure --
 * caller has nothing left to do. Returns true (id_inout holding the
 * created/updated id, no response sent yet) on success, so the two callers
 * can each add their own success body ({"ok":true,"id":N} vs {"ok":true}). */
static bool rules_upsert_from_body(httpd_req_t *req, uint32_t *id_inout)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return false;
    }
    if (req->content_len > sizeof(s_http_body) - 1) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"payload too large\"}");
        return false;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, s_http_body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
            return false;
        }
        received += (size_t)r;
    }
    s_http_body[received] = '\0';

    cJSON *json = cJSON_Parse(s_http_body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return false;
    }

    const cJSON *name_j     = cJSON_GetObjectItem(json, "name");
    const cJSON *source_j   = cJSON_GetObjectItem(json, "source");
    const cJSON *b64_j      = cJSON_GetObjectItem(json, "bytecode_b64");
    const cJSON *enabled_j  = cJSON_GetObjectItem(json, "enabled");
    const cJSON *mode_j     = cJSON_GetObjectItem(json, "mode");
    const cJSON *cooldown_j = cJSON_GetObjectItem(json, "cooldown_s");
    const cJSON *every_j    = cJSON_GetObjectItem(json, "every_s");

    if (!cJSON_IsString(source_j) || !cJSON_IsString(b64_j)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing source/bytecode_b64");
        return false;
    }

    size_t b64len = strlen(b64_j->valuestring);
    if (b64len == 0 || b64len > RULES_B64_MAX) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bytecode_b64 too large");
        return false;
    }
    size_t psbc_len = 0;
    int mbrc = mbedtls_base64_decode(s_http_psbc, sizeof(s_http_psbc), &psbc_len,
                                      (const unsigned char *)b64_j->valuestring, b64len);
    if (mbrc != 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid base64");
        return false;
    }

    char name_buf[RULES_NAME_MAX + 1];
    const char *name;
    if (cJSON_IsString(name_j) && name_j->valuestring[0] != '\0') {
        name = name_j->valuestring;
    } else if (name_from_source(source_j->valuestring, name_buf, sizeof(name_buf))) {
        name = name_buf;
    } else {
        name = "rule";   /* rules_upsert() still enforces the real name grammar below */
    }

    bool enabled = cJSON_IsTrue(enabled_j);
    uint8_t mode = cJSON_IsNumber(mode_j) ? (uint8_t)mode_j->valuedouble : 0;
    uint32_t cooldown_s = cJSON_IsNumber(cooldown_j) ? (uint32_t)cooldown_j->valuedouble : 0;
    uint32_t every_s = cJSON_IsNumber(every_j) ? (uint32_t)every_j->valuedouble : 0;

    char errbuf[80];
    int err = rules_upsert(id_inout, name, source_j->valuestring, s_http_psbc, psbc_len,
                           enabled, mode, cooldown_s, every_s, errbuf, sizeof(errbuf));
    cJSON_Delete(json);
    if (err != ESP_OK) {
        send_rules_error(req, errbuf);
        return false;
    }
    return true;
}

/* POST /api/v1/rules -- create (auth). */
static esp_err_t rules_create_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    uint32_t id = 0;
    if (!rules_upsert_from_body(req, &id)) return ESP_OK;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "id", id);
    return send_json(req, root);
}

/* PUT /api/v1/rules/{id} -- update (auth). Same body as POST. */
static esp_err_t rules_update_put(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    const char *tail = req->uri + strlen("/api/v1/rules/");
    const char *suffix = NULL;
    uint32_t id = parse_rule_id(tail, &suffix);
    if (id == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (!rule_id_exists(id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown rule");
        return ESP_OK;
    }
    if (!rules_upsert_from_body(req, &id)) return ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* DELETE /api/v1/rules/{id} -- auth. */
static esp_err_t rules_delete_delete(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    const char *tail = req->uri + strlen("/api/v1/rules/");
    const char *suffix = NULL;
    uint32_t id = parse_rule_id(tail, &suffix);
    if (id == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (!rules_delete(id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown rule");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/rules/{id}/enable {"enabled":bool} -- auth (checked by
 * rules_post_dispatch() before this is ever reached). */
static esp_err_t rules_enable_post(httpd_req_t *req, uint32_t id)
{
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
    const cJSON *en = json ? cJSON_GetObjectItem(json, "enabled") : NULL;
    bool valid = cJSON_IsBool(en);
    bool want = valid && cJSON_IsTrue(en);
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid enabled");
        return ESP_OK;
    }
    if (!rules_set_enabled(id, want)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown rule");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* POST /api/v1/rules/{id}/test -- immediate dry-run evaluation (spec §6).
 * Auth (checked by rules_post_dispatch() before this is ever reached). */
static esp_err_t rules_test_post(httpd_req_t *req, uint32_t id)
{
    bool ready, cond, would_fire;
    size_t nrefs = PSVM_MAX_REFS;
    size_t nacts = sizeof(s_rules_test_acts) / sizeof(s_rules_test_acts[0]);
    int err = rules_test(id, &ready, &cond, &would_fire,
                         s_rules_test_refs, &nrefs, s_rules_test_acts, &nacts);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown rule");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "test failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON_AddBoolToObject(root, "cond", cond);
    cJSON_AddBoolToObject(root, "would_fire", would_fire);
    cJSON *refs_arr = cJSON_AddArrayToObject(root, "refs");
    for (size_t i = 0; i < nrefs; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ref", s_rules_test_refs[i].ref_desc);
        cJSON_AddNumberToObject(o, "value", s_rules_test_refs[i].value);
        cJSON_AddNumberToObject(o, "age_s", s_rules_test_refs[i].age_s);
        cJSON_AddBoolToObject(o, "ready", s_rules_test_refs[i].ready);
        cJSON_AddItemToArray(refs_arr, o);
    }
    cJSON *acts_arr = cJSON_AddArrayToObject(root, "actions");
    for (size_t i = 0; i < nacts; i++) cJSON_AddItemToArray(acts_arr, cJSON_CreateString(s_rules_test_acts[i].msg));
    return send_json(req, root);
}

/* POST /api/v1/rules/{id}[/enable|/test] -- all under the SAME registered
 * wildcard route, same ESP-IDF one-wildcard-per-prefix reason
 * node_post_dispatch()/plants_post_dispatch() above document in full. Auth
 * checked once here, not in either sub-handler. */
static esp_err_t rules_post_dispatch(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/rules/");
    const char *suffix = NULL;
    uint32_t id = parse_rule_id(tail, &suffix);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (strncmp(suffix, "/enable", 7) == 0 && (suffix[7] == '\0' || suffix[7] == '?'))
        return rules_enable_post(req, id);
    if (strncmp(suffix, "/test", 5) == 0 && (suffix[5] == '\0' || suffix[5] == '?'))
        return rules_test_post(req, id);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return ESP_OK;
}

/* ---- Wrappers + unknown-device discovery + bind-key HTTP API ----------
 * (Task 7, spec §6). Mirrors the rules block above's idioms verbatim
 * (route registration, api_auth_ok() gating, body-size limits, cJSON reply
 * helpers) -- the rules routes are this task's own closest template, per
 * the brief. Its list/body/source/psbc scratch buffers are the SAME
 * s_http_list/s_http_body/s_http_source/s_http_psbc statics the rules block
 * above declares and documents (ram-reclaim task) -- see those declaration
 * comments for the sharing invariant and the full owner list; nothing
 * wrapper-specific is declared again here.
 */

static unknown_dev_t s_unknown_buf[UNKNOWN_DEVICES];

/* base64 length of WRAPPER_PSBC_MAX (2048) bytes: 4*ceil(2048/3), checked
 * BEFORE decoding (brief: "validate base64 length bounds BEFORE decoding")
 * -- same two-line-of-defense reasoning RULES_B64_MAX's comment gives. */
#define WRAPPER_B64_MAX 2732

#define WRAPPER_TEST_HEX_MAX (PSVM_PAYLOAD_MAX * 2)   /* 62: a full 31-byte advert */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0xF];
    }
    out[len * 2] = '\0';
}

/* Parses a decimal wrapper id (u16, wrapper_index.h -- ids are 1-based
 * monotonic, never reused) from the start of s, stopping at '/', '?' or the
 * string's end -- same contract as parse_rule_id() above, just bounded to
 * 16 bits (wrapper_store_upsert()'s *id_inout is uint16_t*, not uint32_t*). */
static uint32_t parse_wrapper_id(const char *s, const char **tail_out)
{
    if (*s < '1' || *s > '9') return 0;
    uint64_t v = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFULL) return 0;
        p++;
    }
    if (*p != '\0' && *p != '/' && *p != '?') return 0;
    if (tail_out) *tail_out = p;
    return (uint32_t)v;
}

static const char *wmatch_kind_str(uint8_t k)
{
    switch (k) {
    case WMATCH_SERVICE:      return "service";
    case WMATCH_MANUFACTURER: return "manufacturer";
    case WMATCH_MAC_PREFIX:   return "mac_prefix";
    }
    return "?";
}

/* Shared by wrappers_list_get()/wrappers_get_one() -- the shape spec §6
 * gives GET /api/v1/wrappers's list entries: {id,name,match:{kind,key},
 * enabled,last_error,match_count}. GET /wrappers/{id} adds "source" on top
 * of this, same "list shape + one extra field" pattern rule_status_json()
 * uses for rules. */
static cJSON *wrapper_status_json(const wrapper_info_t *w)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", w->id);
    cJSON_AddStringToObject(o, "name", w->name);
    cJSON *m = cJSON_AddObjectToObject(o, "match");
    cJSON_AddStringToObject(m, "kind", wmatch_kind_str(w->match_kind));
    cJSON_AddNumberToObject(m, "key", w->match_key);
    cJSON_AddBoolToObject(o, "enabled", w->enabled);
    cJSON_AddStringToObject(o, "last_error", w->last_error);
    cJSON_AddNumberToObject(o, "match_count", w->match_count);
    return o;
}

/* GET /api/v1/wrappers -- unauthenticated, like every GET in this file. */
static esp_err_t wrappers_list_get(httpd_req_t *req)
{
    size_t n = wrapper_store_list(s_http_list.wrappers, WRAPPERS_MAX);
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "wrappers");
    for (size_t i = 0; i < n; i++) cJSON_AddItemToArray(arr, wrapper_status_json(&s_http_list.wrappers[i]));
    return send_json(req, root);
}

/* GET /api/v1/wrappers/{id} -- meta + source (spec §6). Unauthenticated. */
static esp_err_t wrappers_get_one(httpd_req_t *req)
{
    const char *tail = req->uri + strlen("/api/v1/wrappers/");
    const char *suffix = NULL;
    uint32_t id = parse_wrapper_id(tail, &suffix);
    if (id == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }

    size_t n = wrapper_store_list(s_http_list.wrappers, WRAPPERS_MAX);
    const wrapper_info_t *found = NULL;
    for (size_t i = 0; i < n; i++) {
        if (s_http_list.wrappers[i].id == id) { found = &s_http_list.wrappers[i]; break; }
    }
    if (!found) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown wrapper");
        return ESP_OK;
    }

    if (!wrapper_store_get_source((uint16_t)id, s_http_source, sizeof(s_http_source))) {
        s_http_source[0] = '\0';
    }
    cJSON *root = wrapper_status_json(found);
    cJSON_AddStringToObject(root, "source", s_http_source);
    return send_json(req, root);
}

/* True iff id currently names a wrapper -- used by wrappers_update_put() to
 * 404 before ever calling wrapper_store_upsert(), same reasoning
 * rule_id_exists() gives. */
static bool wrapper_id_exists(uint32_t id)
{
    size_t n = wrapper_store_list(s_http_list.wrappers, WRAPPERS_MAX);
    for (size_t i = 0; i < n; i++) if (s_http_list.wrappers[i].id == id) return true;
    return false;
}

/* Sends a 400 with a properly JSON-escaped {"error":"<errbuf>"} body -- same
 * "not a hand-rolled snprintf" reasoning send_rules_error()'s comment
 * gives (wrapper_store_upsert()'s own errbuf text can contain a literal
 * '"', e.g. its name-validation message). */
static void send_wrapper_error(httpd_req_t *req, const char *errbuf)
{
    httpd_resp_set_status(req, "400 Bad Request");
    cJSON *eroot = cJSON_CreateObject();
    cJSON_AddStringToObject(eroot, "error", errbuf);
    char *ebody = cJSON_PrintUnformatted(eroot);
    cJSON_Delete(eroot);
    httpd_resp_set_type(req, "application/json");
    if (ebody) {
        httpd_resp_sendstr(req, ebody);
        free(ebody);
    } else {
        httpd_resp_sendstr(req, "{\"error\":\"invalid wrapper\"}");
    }
}

/* Shared by wrappers_create_post() (POST /api/v1/wrappers, *id_inout==0)
 * and wrappers_update_put() (PUT /api/v1/wrappers/{id}, *id_inout already
 * set to the URL's id): reads+validates the {name, source, bytecode_b64,
 * enabled} body (spec §6) and calls wrapper_store_upsert(), which does
 * every real validation (sizes, the mandatory match header parsed out of
 * `source` itself, psvm_validate(dialect=2), the BTHome/MiFlora/duplicate
 * match-key guards) -- this function's only job is body plumbing, same
 * split rules_upsert_from_body()/rules_upsert() already use. Sends the
 * 400/413 response itself and returns false on any failure -- caller has
 * nothing left to do. Returns true (*id_inout holding the created/updated
 * id, no response sent yet) on success. */
static bool wrapper_upsert_from_body(httpd_req_t *req, uint16_t *id_inout)
{
    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no body");
        return false;
    }
    if (req->content_len > sizeof(s_http_body) - 1) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"payload too large\"}");
        return false;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, s_http_body + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
            return false;
        }
        received += (size_t)r;
    }
    s_http_body[received] = '\0';

    cJSON *json = cJSON_Parse(s_http_body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return false;
    }

    const cJSON *name_j    = cJSON_GetObjectItem(json, "name");
    const cJSON *source_j  = cJSON_GetObjectItem(json, "source");
    const cJSON *b64_j     = cJSON_GetObjectItem(json, "bytecode_b64");
    const cJSON *enabled_j = cJSON_GetObjectItem(json, "enabled");

    if (!cJSON_IsString(name_j) || name_j->valuestring[0] == '\0' ||
        !cJSON_IsString(source_j) || !cJSON_IsString(b64_j)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name/source/bytecode_b64");
        return false;
    }

    size_t b64len = strlen(b64_j->valuestring);
    if (b64len == 0 || b64len > WRAPPER_B64_MAX) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bytecode_b64 too large");
        return false;
    }
    size_t psbc_len = 0;
    int mbrc = mbedtls_base64_decode(s_http_psbc, sizeof(s_http_psbc), &psbc_len,
                                      (const unsigned char *)b64_j->valuestring, b64len);
    if (mbrc != 0) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid base64");
        return false;
    }

    bool enabled = cJSON_IsTrue(enabled_j);

    char errbuf[96];
    int err = wrapper_store_upsert(id_inout, name_j->valuestring, source_j->valuestring,
                                   s_http_psbc, psbc_len, enabled, errbuf, sizeof(errbuf));
    cJSON_Delete(json);
    if (err != ESP_OK) {
        send_wrapper_error(req, errbuf);
        return false;
    }
    return true;
}

/* POST /api/v1/wrappers -- create (auth). */
static esp_err_t wrappers_create_post(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    uint16_t id = 0;
    if (!wrapper_upsert_from_body(req, &id)) return ESP_OK;
    /* Marshal the reindex through the decoder task's request/perform split
     * -- never call ble_collector's match index/arena/memo directly from
     * this (httpd) task (wrapper_arena.h's FINDING 2, ble_collector.h's own
     * doc comment on ble_collector_wrapper_reindex_request()). */
    ble_collector_wrapper_reindex_request();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "id", id);
    return send_json(req, root);
}

/* PUT /api/v1/wrappers/{id} -- update (auth). Same body as POST. */
static esp_err_t wrappers_update_put(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    const char *tail = req->uri + strlen("/api/v1/wrappers/");
    const char *suffix = NULL;
    uint32_t id32 = parse_wrapper_id(tail, &suffix);
    if (id32 == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (!wrapper_id_exists(id32)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown wrapper");
        return ESP_OK;
    }
    uint16_t id = (uint16_t)id32;
    if (!wrapper_upsert_from_body(req, &id)) return ESP_OK;
    ble_collector_wrapper_reindex_request();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* DELETE /api/v1/wrappers/{id} -- auth. */
static esp_err_t wrappers_delete_delete(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);
    const char *tail = req->uri + strlen("/api/v1/wrappers/");
    const char *suffix = NULL;
    uint32_t id = parse_wrapper_id(tail, &suffix);
    if (id == 0 || (suffix[0] != '\0' && suffix[0] != '?')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (!wrapper_store_delete((uint16_t)id)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown wrapper");
        return ESP_OK;
    }
    ble_collector_wrapper_reindex_request();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* AD structure types this file's own tiny scanner (below) looks for --
 * same values ble_collector.c's NimBLE-side parse resolves fields.
 * svc_data_uuid16/mfg_data from (Bluetooth Core Assigned Numbers). */
#define WRAPPER_AD_TYPE_SERVICE_DATA_16 0x16
#define WRAPPER_AD_TYPE_MANUFACTURER    0xFF

/* Minimal raw BLE AD-structure scanner: walks a raw advertisement blob's
 * {len,type,data...} structures looking for `ad_type`, and returns its
 * first two data bytes as a little-endian u16 (the UUID/company-id
 * convention every AD structure of these two types uses). Deliberately
 * NOT NimBLE's ble_hs_adv_parse_fields() -- pulling the NimBLE host parser
 * into the httpd task for what is purely advisory, read-only bookkeeping
 * (picking which captured sample to preview in a dry-run) is unnecessary
 * weight; the REAL match, which this never influences, still only ever
 * happens on the decoder task via wrapper_index_lookup(). */
static bool wrapper_ad_find_u16(const uint8_t *adv, uint8_t len, uint8_t ad_type, uint16_t *out)
{
    uint16_t i = 0;
    while ((uint16_t)(i + 1) < len) {
        uint8_t seg_len = adv[i];
        if (seg_len == 0) break;
        if ((uint16_t)(i + 1 + seg_len) > len) break;
        uint8_t seg_type = adv[i + 1];
        if (seg_type == ad_type && seg_len >= 3) {
            *out = (uint16_t)(adv[i + 2] | (adv[i + 3] << 8));
            return true;
        }
        i = (uint16_t)(i + 1 + seg_len);
    }
    return false;
}

/* True iff captured sample `s` (from unknown device `d`) would resolve to
 * a wrapper declaring (match_kind,match_key) -- used only to pick which
 * captured payload POST /wrappers/{id}/test previews when the caller
 * supplies no `hex` of their own (spec §6: "runs against the supplied hex
 * or the device's newest captured sample"). WMATCH_MAC_PREFIX compares
 * against `d->mac` reversed into DISPLAY/human order first (M3 review fix
 * 3): wrapper_index.h's `key` is packed from the human-typed/API-displayed
 * prefix (`match mac_prefix 0xD0CF13`, parsed verbatim by wrapper_store.c,
 * and the same order GET /api/v1/unknown's own `id` uses -- see that
 * handler's own comment), and ble_collector.c's decode_adv_item() now
 * passes wrapper_index_lookup() mac_disp (display order), not raw
 * item->mac, for exactly this reason -- so this preview must reverse too,
 * or it would silently disagree with what the decoder task actually does.
 * unknown_capture.h's own top comment confirms `d->mac` is stored in raw
 * GAP order, same as adv_item_t.mac, so the reversal below is required. */
static bool wrapper_sample_matches(const unknown_dev_t *d, const unknown_sample_t *s,
                                   uint8_t match_kind, uint32_t match_key)
{
    switch (match_kind) {
    case WMATCH_SERVICE: {
        uint16_t uuid;
        return wrapper_ad_find_u16(s->payload, s->len, WRAPPER_AD_TYPE_SERVICE_DATA_16, &uuid) &&
               uuid == match_key;
    }
    case WMATCH_MANUFACTURER: {
        uint16_t mid;
        return wrapper_ad_find_u16(s->payload, s->len, WRAPPER_AD_TYPE_MANUFACTURER, &mid) &&
               mid == match_key;
    }
    case WMATCH_MAC_PREFIX: {
        uint32_t mac_key = ((uint32_t)d->mac[5] << 16) | ((uint32_t)d->mac[4] << 8) | (uint32_t)d->mac[3];
        return mac_key == match_key;
    }
    default:
        return false;
    }
}

/* Scans every captured unknown device's samples for the NEWEST one (by ts)
 * that would match (match_kind,match_key); copies it into out_payload
 * (capacity PSVM_PAYLOAD_MAX) and *out_len. False if none match. */
static bool wrapper_find_test_sample(uint8_t match_kind, uint32_t match_key,
                                     uint8_t *out_payload, uint8_t *out_len)
{
    size_t n = unknown_capture_list(s_unknown_buf, UNKNOWN_DEVICES);
    const unknown_sample_t *best = NULL;
    for (size_t i = 0; i < n; i++) {
        const unknown_dev_t *d = &s_unknown_buf[i];
        for (uint8_t j = 0; j < d->n; j++) {
            const unknown_sample_t *s = &d->s[j];
            if (!wrapper_sample_matches(d, s, match_kind, match_key)) continue;
            if (!best || s->ts > best->ts) best = s;
        }
    }
    if (!best) return false;
    memcpy(out_payload, best->payload, best->len);
    *out_len = best->len;
    return true;
}

/* Dry-run emit sink: captures {cap,value} pairs into a fixed local array
 * rather than data_core_submit_cap() -- spec §6: "returns the emitted
 * capability values without touching the registry". Deliberately reports
 * the VM's raw computed value, not what capability_encode()'s round-trip
 * would clamp/skip it to: a dry run is exactly the place a user needs to
 * see "the wrapper computed 137.4%", not have that silently vanish because
 * it's out of a real sensor's plausible range. */
typedef struct { uint8_t cap; float value; } wrapper_test_emit_t;
typedef struct { wrapper_test_emit_t *emits; size_t *n; size_t max; } wrapper_test_ctx_t;

static void wrapper_test_emit_sink(void *ctx, uint8_t capability, float value)
{
    wrapper_test_ctx_t *t = (wrapper_test_ctx_t *)ctx;
    if (*t->n >= t->max) return;
    t->emits[*t->n].cap = capability;
    t->emits[*t->n].value = value;
    (*t->n)++;
}

/* POST /api/v1/wrappers/{id}/test {hex?: "..."} -- dry-run (spec §6, "the
 * wrapper equivalent of M1's rule dry-run"). Auth checked by
 * wrappers_post_dispatch() before this is ever reached. Never touches the
 * registry, never touches the shared wrapper arena (see s_http_psbc's own
 * declaration comment, third owner) and never calls
 * ble_collector_wrapper_reindex_request() -- nothing about the installed
 * registry changes. */
static esp_err_t wrappers_test_post(httpd_req_t *req, uint32_t id)
{
    size_t n = wrapper_store_list(s_http_list.wrappers, WRAPPERS_MAX);
    const wrapper_info_t *w = NULL;
    for (size_t i = 0; i < n; i++) {
        if (s_http_list.wrappers[i].id == id) { w = &s_http_list.wrappers[i]; break; }
    }
    if (!w) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown wrapper");
        return ESP_OK;
    }

    /* Body is optional -- absent/empty means "use a captured sample". */
    char hexbuf[WRAPPER_TEST_HEX_MAX + 1];
    hexbuf[0] = '\0';
    if (req->content_len > 0) {
        if (req->content_len > sizeof(s_http_body) - 1) {
            httpd_resp_set_status(req, "413 Payload Too Large");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"error\":\"payload too large\"}");
            return ESP_OK;
        }
        size_t received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, s_http_body + received, req->content_len - received);
            if (r <= 0) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read error");
                return ESP_OK;
            }
            received += (size_t)r;
        }
        s_http_body[received] = '\0';
        cJSON *json = cJSON_Parse(s_http_body);
        const cJSON *hex_j = json ? cJSON_GetObjectItem(json, "hex") : NULL;
        if (cJSON_IsString(hex_j)) {
            /* Reject an oversized hex string outright rather than letting
             * strlcpy() silently truncate it into a shorter-but-still-valid
             * payload -- that would run the dry-run against DIFFERENT bytes
             * than the caller actually sent, with no error to explain why. */
            if (strlen(hex_j->valuestring) > (size_t)WRAPPER_TEST_HEX_MAX) {
                cJSON_Delete(json);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                    "hex must be an even-length hex string, up to 62 chars");
                return ESP_OK;
            }
            strlcpy(hexbuf, hex_j->valuestring, sizeof(hexbuf));
        }
        cJSON_Delete(json);
    }

    uint8_t payload[PSVM_PAYLOAD_MAX];
    uint8_t payload_len = 0;
    if (hexbuf[0] != '\0') {
        size_t hexlen = strlen(hexbuf);
        if (hexlen % 2 != 0 || hexlen > (size_t)WRAPPER_TEST_HEX_MAX) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "hex must be an even-length hex string, up to 62 chars");
            return ESP_OK;
        }
        payload_len = (uint8_t)(hexlen / 2);
        bool valid_hex = true;
        for (uint8_t i = 0; i < payload_len && valid_hex; i++) {
            int hi = hexval(hexbuf[i * 2]);
            int lo = hexval(hexbuf[i * 2 + 1]);
            if (hi < 0 || lo < 0) valid_hex = false;
            else payload[i] = (uint8_t)((hi << 4) | lo);
        }
        if (!valid_hex) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "hex must contain only hex digits");
            return ESP_OK;
        }
    } else if (!wrapper_find_test_sample(w->match_kind, w->match_key, payload, &payload_len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "no hex supplied and no captured sample matches this wrapper yet");
        return ESP_OK;
    }

    size_t psbc_len = 0;
    if (!wrapper_store_read_psbc((uint16_t)id, s_http_psbc, sizeof(s_http_psbc), &psbc_len)) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddArrayToObject(root, "emits");
        cJSON_AddStringToObject(root, "error", "bytecode unavailable");
        return send_json(req, root);
    }

    psvm_prog_t prog;
    psvm_err_t verr = psvm_validate(s_http_psbc, psbc_len, PSVM_DIALECT_WRAPPERS,
                                    CAPABILITY_COUNT - 1, 0, &prog);
    if (verr != PSVM_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", false);
        cJSON_AddArrayToObject(root, "emits");
        cJSON_AddStringToObject(root, "error", psvm_err_short(verr));
        return send_json(req, root);
    }

    wrapper_test_emit_t emits[PSVM_MAX_EMITS];
    size_t nemits = 0;
    wrapper_test_ctx_t tctx = { .emits = emits, .n = &nemits, .max = PSVM_MAX_EMITS };
    psvm_wrapper_io_t wio = {
        .payload = { .data = payload, .len = payload_len },
        .emit = wrapper_test_emit_sink,
        .emit_ctx = &tctx,
        /* AES_CCM stays unwired here too -- same "no wrapper-generic nonce
         * scheme exists in this codebase" reasoning wrapper_exec.h's own
         * top comment gives; a dry-run of a wrapper calling
         * aes_ccm_decrypt(...) fails identically to a real run. */
        .aes_ccm = NULL,
        .aes_ccm_ctx = NULL,
    };
    psvm_result_t res = psvm_run(&prog, NULL, &wio, NULL, NULL, false);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", res.err == PSVM_OK);
    cJSON *arr = cJSON_AddArrayToObject(root, "emits");
    for (size_t i = 0; i < nemits; i++) {
        const capability_t *cap = capability_get(emits[i].cap);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "cap", emits[i].cap);
        cJSON_AddStringToObject(o, "name", cap ? cap->name : "?");
        cJSON_AddNumberToObject(o, "value", emits[i].value);
        cJSON_AddStringToObject(o, "unit", cap ? cap->unit : "");
        cJSON_AddItemToArray(arr, o);
    }
    if (res.err != PSVM_OK) cJSON_AddStringToObject(root, "error", psvm_err_short(res.err));
    return send_json(req, root);
}

/* POST /api/v1/wrappers/{id}/test -- the only POST sub-route wrappers have
 * (unlike rules, no "/enable": PUT already carries `enabled`). Same single
 * wildcard-route-per-method-per-prefix reasoning rules_post_dispatch()'s
 * comment gives. */
static esp_err_t wrappers_post_dispatch(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/wrappers/");
    const char *suffix = NULL;
    uint32_t id = parse_wrapper_id(tail, &suffix);
    if (id == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad id");
        return ESP_OK;
    }
    if (strncmp(suffix, "/test", 5) == 0 && (suffix[5] == '\0' || suffix[5] == '?'))
        return wrappers_test_post(req, id);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    return ESP_OK;
}

/* GET /api/v1/unknown -- unauthenticated (spec §5/§6). This exact shape is
 * M4's input contract: {devices:[{id,rssi,last_seen_s,samples:[{hex,len,
 * ts}]}]} -- do not vary it. */
static esp_err_t unknown_get(httpd_req_t *req)
{
    size_t n = unknown_capture_list(s_unknown_buf, UNKNOWN_DEVICES);
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);

    /* Streamed one device at a time rather than built as one cJSON tree and
     * handed to send_json() (see its doc comment above): this endpoint's
     * whole-document shape -- up to 8 devices x 2 samples, hundreds of
     * small allocations followed by ONE contiguous ~1.2KB print request --
     * is exactly what fails on a heap fragmented by BLE bring-up (largest
     * free block ~7.6KB, measured on hardware). Peak allocation here is
     * roughly one device (~200B) instead of the whole array: build one
     * small cJSON object, print it, send it as a chunk, delete/free, move
     * on. The array wrapper, commas and closing brace are sent as literal
     * chunks around that.
     *
     * Headers and the 200 status are committed by the first chunk below, so
     * unlike send_json() there is no way to fall back to a 503 if an
     * allocation fails partway through -- see the oom handling at the
     * bottom of the loop. */
    httpd_resp_set_type(req, "application/json");

    /* Whether anything has actually been written yet -- NOT the same as
     * "i > 0": the `if (d->n == 0) continue;` skip below means an earlier
     * iteration can produce nothing, and deciding comma placement from the
     * loop index would then emit either a leading comma (skip was first) or
     * a doubled one (skip wasn't), either way malformed JSON. */
    bool emitted = false;
    bool oom = false;
    size_t oom_at = 0;

    /* Bytes pending in s_http_body -- see the coalescing comment in the loop.
     * The opening literal starts the buffer rather than being its own write,
     * so the common case is exactly one TCP write for the whole response. */
    static const char open_lit[] = "{\"devices\":[";
    size_t used = sizeof(open_lit) - 1;
    memcpy(s_http_body, open_lit, used);

    for (size_t i = 0; i < n; i++) {
        const unknown_dev_t *d = &s_unknown_buf[i];
        if (d->n == 0) continue;   /* defensive -- unknown_capture_add() never leaves a tracked device with zero samples, but this file doesn't get to assume that blindly across the component boundary */

        /* unknown_capture stores the RAW GAP-order MAC exactly as the radio
         * callback captured it (unknown_capture.h's own top comment: takes
         * primitives straight off adv_item_t, never reversed) -- EVERY
         * other MAC-bearing surface in this codebase (device_id_from_mac(),
         * the wrapper-dispatch path's mac_disp, the Devices tab) uses
         * REVERSED display order. Reversed here for the same reason
         * decode_bthome_item()/decode_adv_item() reverse it: otherwise an
         * operator can't correlate an unknown device with its own
         * Devices-tab row, and M4's AI (this payload's actual consumer)
         * would learn the wrong byte order for any mac_prefix match key it
         * writes into a new wrapper. */
        uint8_t disp[6];
        for (int b = 0; b < 6; b++) disp[b] = d->mac[5 - b];
        device_id_t did = device_id_from_mac(DEV_KIND_BLE, disp);
        char idbuf[24];
        device_id_format(&did, idbuf, sizeof(idbuf));

        cJSON *o = cJSON_CreateObject();
        if (!o) { oom = true; oom_at = i; break; }
        cJSON_AddStringToObject(o, "id", idbuf);
        cJSON_AddNumberToObject(o, "rssi", d->s[d->n - 1].rssi);   /* newest sample's rssi (s[] is oldest-first, newest-last) */
        cJSON_AddNumberToObject(o, "last_seen_s",
                                (now_uptime_s >= d->last_seen_s) ? (now_uptime_s - d->last_seen_s) : 0);

        cJSON *samples = cJSON_AddArrayToObject(o, "samples");
        if (!samples) { cJSON_Delete(o); oom = true; oom_at = i; break; }
        for (uint8_t j = 0; j < d->n; j++) {
            const unknown_sample_t *s = &d->s[j];
            char hex[2 * ADV_PAYLOAD_MAX + 1];
            hex_encode(s->payload, s->len, hex);
            cJSON *so = cJSON_CreateObject();
            if (!so) { oom = true; oom_at = i; break; }
            cJSON_AddStringToObject(so, "hex", hex);
            cJSON_AddNumberToObject(so, "len", s->len);
            cJSON_AddNumberToObject(so, "ts", s->ts);
            cJSON_AddItemToArray(samples, so);
        }
        if (oom) { cJSON_Delete(o); break; }

        char *body = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (!body) { oom = true; oom_at = i; break; }

        /* Coalesce into s_http_body rather than writing each device as its
         * own chunk. Streaming per device fixed the allocation failure this
         * endpoint used to die of, but replaced it with a latency failure:
         * ~18 small TCP writes for a 1.2 KB body, and with WiFi power-save
         * on (wifi:pm start) each one can wait out a DTIM cycle. Measured on
         * the C3, per-device chunks took 1.3-5.6 s when they worked and timed
         * out at 18 s when they didn't -- and a timeout wedges the socket
         * pool for ~90 s, so the failures cascade.
         *
         * s_http_body is the request-scoped shared scratch buffer (see its
         * declaration): this is a GET handler on the same single httpd task
         * as the POST/PUT handlers that use it for request bodies, so it is
         * free here, and using it costs no new static RAM on a target with
         * ~1.4 KB of margin. The whole response fits in one write at 8 KB;
         * the flush-when-full path exists so a future larger capture cannot
         * silently overflow it.
         *
         * Peak HEAP allocation is unchanged -- still one small device object
         * at a time, which is the property that fixed the original bug. */
        size_t need = strlen(body) + (emitted ? 1 : 0);
        if (used + need > sizeof(s_http_body)) {
            if (used > 0 && httpd_resp_send_chunk(req, s_http_body, used) != ESP_OK) {
                free(body);
                return ESP_FAIL;   /* client/socket gone; don't keep writing to a dead connection */
            }
            used = 0;
            /* A single device larger than the whole buffer cannot be split
             * without emitting malformed JSON, so treat it as the same
             * "cannot render this list" case as an allocation failure. */
            if (need > sizeof(s_http_body)) { free(body); oom = true; oom_at = i; break; }
        }
        if (emitted) s_http_body[used++] = ',';
        memcpy(s_http_body + used, body, need - (emitted ? 1 : 0));
        used += need - (emitted ? 1 : 0);
        free(body);
        emitted = true;
    }

    if (oom) {
        /* Headers and the 200 are already on the wire, so unlike
         * send_json()'s 503 there is no way to tell the client this list is
         * short -- close the array here (still valid JSON, just incomplete)
         * and log loudly, so an operator (or M4's own tooling watching the
         * log) can tell this apart from "that really is every unknown
         * device the hub heard". */
        ESP_LOGW(TAG, "unknown_get: allocation failed streaming device %u/%u, closing list early",
                 (unsigned)oom_at, (unsigned)n);
    }

    /* Close the array in the same buffer, so the common case leaves the
     * handler having done exactly one TCP write. */
    if (used + 2 > sizeof(s_http_body)) {
        if (httpd_resp_send_chunk(req, s_http_body, used) != ESP_OK) return ESP_FAIL;
        used = 0;
    }
    s_http_body[used++] = ']';
    s_http_body[used++] = '}';
    if (httpd_resp_send_chunk(req, s_http_body, used) != ESP_OK) return ESP_FAIL;
    httpd_resp_sendstr_chunk(req, NULL);   /* end chunked response */
    return ESP_OK;
}

/* POST /api/v1/devices/{id}/key {"key":"<32 hex chars>"|null} -- bind-key
 * set/clear (spec §4, auth). Only the "/api/v1/devices" wildcard route (POST)
 * has -- unlike plants/nodes/rules there is nothing else to dispatch on
 * here, but this still goes through the same wildcard-route-plus-suffix-
 * check shape as those for consistency and because "/api/v1/devices" (GET,
 * exact) already owns the bare prefix.
 *
 * Keys are WRITE-ONLY (bthome.h's bindkey_get()/bindkey_has() contract,
 * spec §4: "Keys are never returned by any GET") -- this handler only ever
 * calls bindkey_set(), never bindkey_get(), and the 200 response never
 * echoes key material back. */
static esp_err_t devices_post_dispatch(httpd_req_t *req)
{
    if (!api_auth_ok(req)) return api_send_401(req);

    const char *tail = req->uri + strlen("/api/v1/devices/");
    size_t taillen = strcspn(tail, "?");
    static const char key_suffix[] = "/key";
    size_t suflen = sizeof(key_suffix) - 1;
    if (taillen <= suflen || strncmp(tail + taillen - suflen, key_suffix, suflen) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
        return ESP_OK;
    }
    size_t idlen = taillen - suflen;
    char idbuf[40];
    if (idlen == 0 || idlen >= sizeof(idbuf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device id");
        return ESP_OK;
    }
    memcpy(idbuf, tail, idlen);
    idbuf[idlen] = '\0';

    device_id_t dev;
    if (!device_id_parse(idbuf, &dev)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad device id");
        return ESP_OK;
    }

    char body[128];
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
    const cJSON *key_j = json ? cJSON_GetObjectItem(json, "key") : NULL;
    if (!json || !key_j) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing key");
        return ESP_OK;
    }

    char dev_id_str[24];
    device_id_format(&dev, dev_id_str, sizeof(dev_id_str));

    bool ok;
    if (cJSON_IsNull(key_j)) {
        ok = bindkey_set(dev_id_str, NULL);
    } else if (cJSON_IsString(key_j) && strlen(key_j->valuestring) == 32) {
        uint8_t keybytes[16];
        bool valid_hex = true;
        for (int i = 0; i < 16 && valid_hex; i++) {
            int hi = hexval(key_j->valuestring[i * 2]);
            int lo = hexval(key_j->valuestring[i * 2 + 1]);
            if (hi < 0 || lo < 0) valid_hex = false;
            else keybytes[i] = (uint8_t)((hi << 4) | lo);
        }
        if (!valid_hex) {
            cJSON_Delete(json);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key must be 32 hex chars or null");
            return ESP_OK;
        }
        ok = bindkey_set(dev_id_str, keybytes);
    } else {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key must be 32 hex chars or null");
        return ESP_OK;
    }
    cJSON_Delete(json);

    if (!ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "failed to store key");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

void api_v1_register(httpd_handle_t server)
{
    httpd_uri_t health = { .uri = "/api/v1/health", .method = HTTP_GET, .handler = health_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &health));
    httpd_uri_t freset = { .uri = "/api/v1/factory_reset", .method = HTTP_POST, .handler = factory_reset_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &freset));
    httpd_uri_t status = { .uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));
    httpd_uri_t scan = { .uri = "/api/v1/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &scan));
    httpd_uri_t wifi = { .uri = "/api/v1/wifi", .method = HTTP_POST, .handler = wifi_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wifi));
    /* Devices: the device+capability surface (Task 6, spec §6/§7).
     * "/api/v1/sensors" (GET) stays registered as the deprecated alias
     * task-6 brief calls for; its POST (rename, still mac-keyed) is
     * unchanged. No per-sensor history route anymore -- devices' time
     * series live under the plant they're bound to (GET /api/v1/history
     * below), same as V1's demoted probe pool. */
    httpd_uri_t devices = { .uri = "/api/v1/devices", .method = HTTP_GET, .handler = devices_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices));
    httpd_uri_t caps = { .uri = "/api/v1/capabilities", .method = HTTP_GET, .handler = capabilities_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &caps));
    httpd_uri_t sensors_rn = { .uri = "/api/v1/sensors/*", .method = HTTP_POST, .handler = sensors_rename_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sensors_rn));
    httpd_uri_t sensors = { .uri = "/api/v1/sensors", .method = HTTP_GET, .handler = sensors_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &sensors));

    /* Plants: the primary surface (Task 6, spec §4/§7). "/api/v1/plants"
     * (exact) and its wildcarded "/api/v1/plants/" + "*" form are distinct
     * URI templates to ESP-IDF's matcher -- same non-collision already
     * relied on above for exact "/api/v1/nodes" vs its own wildcarded form
     * -- so registration order between them doesn't matter; each wildcard
     * method (POST/DELETE) gets its own single dispatch point below, same
     * pattern as the nodes routes' three separate wildcard handlers. No
     * wildcarded GET anymore -- per-plant history moved to the exact
     * "/api/v1/history" route below (spec §7's capability-driven history
     * tab, not a raw/hourly tier pass-through). */
    httpd_uri_t plants = { .uri = "/api/v1/plants", .method = HTTP_GET, .handler = plants_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &plants));
    httpd_uri_t plants_create = { .uri = "/api/v1/plants", .method = HTTP_POST, .handler = plants_create_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &plants_create));
    /* Handles rename ("/{id}") and capability bind ("/{id}/bind") -- both
     * under this one route, per plants_post_dispatch()'s comment. */
    httpd_uri_t plants_post = { .uri = "/api/v1/plants/*", .method = HTTP_POST, .handler = plants_post_dispatch };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &plants_post));
    httpd_uri_t plants_del = { .uri = "/api/v1/plants/*", .method = HTTP_DELETE, .handler = plants_delete_delete };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &plants_del));
    httpd_uri_t history = { .uri = "/api/v1/history", .method = HTTP_GET, .handler = history_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &history));

    /* Clean-start notice (spec §5). */
    httpd_uri_t notice_g = { .uri = "/api/v1/notice", .method = HTTP_GET, .handler = notice_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &notice_g));
    httpd_uri_t notice_d = { .uri = "/api/v1/notice/dismiss", .method = HTTP_POST, .handler = notice_dismiss_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &notice_d));

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

    /* Rules (Task 6, spec §6). "GET /api/v1/events?after=" is deliberately
     * NOT registered here -- see the block comment above rules_list_get()
     * for why: it shares sse.c's existing exact "/api/v1/events" GET route
     * instead of a second, colliding registration. */
    httpd_uri_t rules_g = { .uri = "/api/v1/rules", .method = HTTP_GET, .handler = rules_list_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_g));
    httpd_uri_t rules_c = { .uri = "/api/v1/rules", .method = HTTP_POST, .handler = rules_create_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_c));
    httpd_uri_t rules_get1 = { .uri = "/api/v1/rules/*", .method = HTTP_GET, .handler = rules_get_one };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_get1));
    httpd_uri_t rules_put = { .uri = "/api/v1/rules/*", .method = HTTP_PUT, .handler = rules_update_put };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_put));
    httpd_uri_t rules_del = { .uri = "/api/v1/rules/*", .method = HTTP_DELETE, .handler = rules_delete_delete };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_del));
    /* Handles "/{id}/enable" and "/{id}/test" -- both under this one route,
     * per rules_post_dispatch()'s comment. */
    httpd_uri_t rules_post = { .uri = "/api/v1/rules/*", .method = HTTP_POST, .handler = rules_post_dispatch };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &rules_post));

    /* Wrappers + unknown-device discovery + bind-key (Task 7, spec §6). */
    httpd_uri_t wrappers_g = { .uri = "/api/v1/wrappers", .method = HTTP_GET, .handler = wrappers_list_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_g));
    httpd_uri_t wrappers_c = { .uri = "/api/v1/wrappers", .method = HTTP_POST, .handler = wrappers_create_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_c));
    httpd_uri_t wrappers_get1 = { .uri = "/api/v1/wrappers/*", .method = HTTP_GET, .handler = wrappers_get_one };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_get1));
    httpd_uri_t wrappers_put = { .uri = "/api/v1/wrappers/*", .method = HTTP_PUT, .handler = wrappers_update_put };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_put));
    httpd_uri_t wrappers_del = { .uri = "/api/v1/wrappers/*", .method = HTTP_DELETE, .handler = wrappers_delete_delete };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_del));
    /* Handles "/{id}/test" -- the only wrapper POST sub-route, per
     * wrappers_post_dispatch()'s comment. */
    httpd_uri_t wrappers_post = { .uri = "/api/v1/wrappers/*", .method = HTTP_POST, .handler = wrappers_post_dispatch };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wrappers_post));

    httpd_uri_t unknown_g = { .uri = "/api/v1/unknown", .method = HTTP_GET, .handler = unknown_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &unknown_g));

    /* Bind-key set/clear. "/api/v1/devices" (GET, exact) already owns the
     * bare prefix -- this wildcard POST is a distinct URI template to
     * ESP-IDF's matcher, same non-collision every other exact+wildcard pair
     * in this file already relies on (e.g. "/api/v1/nodes" exact vs wildcard). */
    httpd_uri_t devices_post = { .uri = "/api/v1/devices/*", .method = HTTP_POST, .handler = devices_post_dispatch };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &devices_post));
}
