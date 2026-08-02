#include "swarm_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "swarm_store";
#define NS "planthub"
#define KEY_ROLE  "role"
#define KEY_HUB   "sw_hub"
#define KEY_HUBCC "sw_hubcc"
#define KEY_NODES "sw_nodes"
#define KEY_PFAIL "sw_pfail"

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[SWARM_LMK_LEN];
    uint8_t channel;
} hub_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[SWARM_LMK_LEN];
    char    name[SWARM_NODE_NAME_LEN + 1];  /* "" = unset */
} node_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t format;
    uint8_t count;
    node_entry_t n[SWARM_MAX_NODES];
} nodes_blob_t;

/* M5a's on-disk layout: no format byte, no name, and a hard cap of 4. A
 * blob with exactly this length is the migration trigger -- see
 * load_nodes_blob() below. Kept private to this translation unit; nothing
 * outside the migration path should ever see this shape again. */
#define SWARM_STORE_V0_MAX_NODES 4

typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    uint8_t lmk[SWARM_LMK_LEN];
} node_entry_v0_t;

typedef struct __attribute__((packed)) {
    uint8_t count;
    node_entry_v0_t n[SWARM_STORE_V0_MAX_NODES];
} nodes_blob_v0_t;

static SemaphoreHandle_t s_mutex;
static swarm_role_t s_role;
static bool s_hub_set;
static hub_blob_t s_hub;
static bool s_hub_cc_set;
static char s_hub_cc[3];
static nodes_blob_t s_nodes;
static bool s_pair_failed;

static esp_err_t write_blob(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t erase_key(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Loads the KEY_NODES blob into *out, migrating an M5a-format blob in
 * place if that's what's stored.
 *
 * M5a accepted this blob on exact-length match only, and raising
 * SWARM_MAX_NODES from 4 to 8 changes that length (a longer fixed-size
 * array), so the naive "wrong length -> treat as absent" fallback that
 * already existed here for corrupt/foreign data would, without this
 * function, also silently discard every already-paired node's table on
 * the very first boot after this upgrade. Three cases, decided purely by
 * the blob's on-disk LENGTH (an M5a blob carries no format byte to key
 * off instead):
 *
 *   1. Length == sizeof(nodes_blob_t) (current format): read it and check
 *      the leading format byte. A match is loaded as-is. A MISMATCH means
 *      some future format bumped this again without a migration branch
 *      landing here yet, or on-flash corruption -- either way the layout
 *      cannot be trusted, so it is discarded (loudly) rather than risk
 *      misreading node MACs/LMKs from a different shape as real ones.
 *   2. Length == sizeof(nodes_blob_v0_t) (M5a's exact shape, no format
 *      byte): read with the OLD parser, copy every entry across (name
 *      left empty -- M5a had no names), and immediately persist the
 *      result in the new format. This is the migration: a device
 *      carrying an M5a blob keeps its pairing, gains an empty name per
 *      node, and never has to re-pair.
 *   3. Anything else (including "key absent", the fresh-install case):
 *      start with an empty table. This matches M5a's own behaviour for a
 *      blob it didn't recognise.
 *
 * out is fully populated with a sane default (format=SWARM_STORE_FORMAT,
 * count=0, no entries) before any of the above, so every return path
 * leaves *out valid even on read/parse failure. */
static void load_nodes_blob(nvs_handle_t h, nodes_blob_t *out)
{
    memset(out, 0, sizeof(*out));
    out->format = SWARM_STORE_FORMAT;

    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, KEY_NODES, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;  /* fresh install: no node table yet */
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nodes blob length query failed: %s; starting with an empty node table",
                 esp_err_to_name(err));
        return;
    }

    if (len == sizeof(nodes_blob_t)) {
        nodes_blob_t blob;
        size_t rlen = sizeof(blob);
        if (nvs_get_blob(h, KEY_NODES, &blob, &rlen) != ESP_OK || rlen != sizeof(blob)) {
            ESP_LOGW(TAG, "nodes blob read failed at its expected current-format length; "
                          "starting with an empty node table");
            return;
        }
        if (blob.format != SWARM_STORE_FORMAT) {
            ESP_LOGE(TAG, "nodes blob has unknown format byte %u (expected %u) at the "
                          "current-format length -- DISCARDING it rather than trusting an "
                          "unrecognised layout; every paired node is lost until re-paired",
                     blob.format, (unsigned)SWARM_STORE_FORMAT);
            return;
        }
        if (blob.count > SWARM_MAX_NODES) blob.count = SWARM_MAX_NODES;  /* defensive */
        *out = blob;
        return;
    }

    if (len == sizeof(nodes_blob_v0_t)) {
        nodes_blob_v0_t old;
        size_t rlen = sizeof(old);
        if (nvs_get_blob(h, KEY_NODES, &old, &rlen) != ESP_OK || rlen != sizeof(old)) {
            ESP_LOGW(TAG, "M5a-format nodes blob read failed; starting with an empty node table");
            return;
        }
        uint8_t n = old.count;
        if (n > SWARM_STORE_V0_MAX_NODES) n = SWARM_STORE_V0_MAX_NODES;  /* defensive: M5a's own cap */
        if (n > SWARM_MAX_NODES) n = SWARM_MAX_NODES;
        ESP_LOGW(TAG, "migrating M5a-format node table (%u node(s)) to format %u",
                 n, (unsigned)SWARM_STORE_FORMAT);

        nodes_blob_t migrated;
        memset(&migrated, 0, sizeof(migrated));
        migrated.format = SWARM_STORE_FORMAT;
        migrated.count = n;
        for (uint8_t i = 0; i < n; i++) {
            memcpy(migrated.n[i].mac, old.n[i].mac, 6);
            memcpy(migrated.n[i].lmk, old.n[i].lmk, SWARM_LMK_LEN);
            /* migrated.n[i].name stays "" -- M5a had no names to carry over */
        }

        esp_err_t werr = write_blob(KEY_NODES, &migrated, sizeof(migrated));
        if (werr != ESP_OK) {
            /* Keep the migrated data in RAM for this boot even though the
             * flash write failed -- the alternative is losing the
             * pairing THIS boot despite having just proven we could read
             * it, which is strictly worse. The next successful write
             * (a rename, a forget, a fresh pairing) persists it. */
            ESP_LOGE(TAG, "failed to persist migrated node table: %s (kept in RAM for this "
                          "boot only; will retry on the next write)", esp_err_to_name(werr));
        } else {
            ESP_LOGI(TAG, "node table migration complete, %u node(s) preserved", n);
        }
        *out = migrated;
        return;
    }

    ESP_LOGW(TAG, "nodes blob has unrecognised length %d (expected %d for the current format or "
                  "%d for M5a); starting with an empty node table",
             (int)len, (int)sizeof(nodes_blob_t), (int)sizeof(nodes_blob_v0_t));
}

esp_err_t swarm_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_role = SWARM_ROLE_UNSET;
    s_hub_set = false;
    memset(&s_hub, 0, sizeof(s_hub));
    s_hub_cc_set = false;
    memset(s_hub_cc, 0, sizeof(s_hub_cc));
    memset(&s_nodes, 0, sizeof(s_nodes));
    s_nodes.format = SWARM_STORE_FORMAT;
    s_pair_failed = false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;  /* fresh NVS = defaults */

    uint8_t role_byte;
    if (nvs_get_u8(h, KEY_ROLE, &role_byte) == ESP_OK) {
        if (role_byte == SWARM_ROLE_UNSET || role_byte == SWARM_ROLE_MAIN || role_byte == SWARM_ROLE_NODE) {
            s_role = (swarm_role_t)role_byte;
        } else {
            ESP_LOGW(TAG, "invalid stored role byte %u; defaulting to UNSET", role_byte);
            s_role = SWARM_ROLE_UNSET;
        }
    }

    size_t hub_len = sizeof(s_hub);
    s_hub_set = nvs_get_blob(h, KEY_HUB, &s_hub, &hub_len) == ESP_OK && hub_len == sizeof(s_hub);
    if (!s_hub_set) memset(&s_hub, 0, sizeof(s_hub));

    /* Own key, deliberately separate from KEY_HUB -- see swarm_store.h.
     * Absent (ESP_ERR_NVS_NOT_FOUND, e.g. this device paired under
     * protocol v1, or was factory-reset) just means "nothing learned
     * yet"; s_hub_cc_set stays false and callers fall back to the
     * compile-time default. */
    size_t cc_len = sizeof(s_hub_cc);
    s_hub_cc_set = nvs_get_blob(h, KEY_HUBCC, s_hub_cc, &cc_len) == ESP_OK && cc_len == sizeof(s_hub_cc);
    if (!s_hub_cc_set) memset(s_hub_cc, 0, sizeof(s_hub_cc));

    load_nodes_blob(h, &s_nodes);

    uint8_t pfail_byte;
    s_pair_failed = nvs_get_u8(h, KEY_PFAIL, &pfail_byte) == ESP_OK && pfail_byte != 0;

    nvs_close(h);
    /* hub_channel logged unconditionally (0 when !hub_paired) so a stored
     * channel is visible at a glance at every boot, not just inferred from
     * a separate pairing-time log line -- makes a hub/node channel
     * mismatch obvious in the console. */
    ESP_LOGI(TAG, "role=%d hub_paired=%d hub_channel=%u hub_country=%s nodes=%d pair_failed=%d",
             s_role, s_hub_set, s_hub.channel, s_hub_cc_set ? s_hub_cc : "(default)",
             s_nodes.count, s_pair_failed);
    return ESP_OK;
}

swarm_role_t swarm_store_role(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    swarm_role_t r = s_role;
    xSemaphoreGive(s_mutex);
    return r;
}

esp_err_t swarm_store_set_role(swarm_role_t r)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_ROLE, (uint8_t)r);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) s_role = r;
    xSemaphoreGive(s_mutex);
    return err;
}

bool swarm_store_hub(uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN], uint8_t *channel_out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_hub_set;
    if (ok) {
        if (mac_out) memcpy(mac_out, s_hub.mac, 6);
        if (lmk_out) memcpy(lmk_out, s_hub.lmk, SWARM_LMK_LEN);
        if (channel_out) *channel_out = s_hub.channel;
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t swarm_store_set_hub(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN], uint8_t channel)
{
    hub_blob_t blob;
    memcpy(blob.mac, mac, 6);
    memcpy(blob.lmk, lmk, SWARM_LMK_LEN);
    blob.channel = channel;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = write_blob(KEY_HUB, &blob, sizeof(blob));
    if (err == ESP_OK) {
        s_hub = blob;
        s_hub_set = true;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_set_channel(uint8_t channel)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_hub_set) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    hub_blob_t blob = s_hub;
    blob.channel = channel;
    esp_err_t err = write_blob(KEY_HUB, &blob, sizeof(blob));
    if (err == ESP_OK) s_hub = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

bool swarm_store_hub_country(char out[3])
{
    if (!out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = s_hub_cc_set;
    if (ok) memcpy(out, s_hub_cc, sizeof(s_hub_cc));
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t swarm_store_set_hub_country(const char cc[3])
{
    if (!cc) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = write_blob(KEY_HUBCC, cc, 3);
    if (err == ESP_OK) {
        memcpy(s_hub_cc, cc, sizeof(s_hub_cc));
        s_hub_cc_set = true;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_clear_hub(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = erase_key(KEY_HUB);
    /* Best-effort, same reasoning as swarm_store_reset_all(): the country
     * is meaningless once unpaired from the hub that reported it, so clear
     * it too, but don't let a failure here mask the hub-blob erase result
     * above (the more important of the two). */
    esp_err_t cc_err = erase_key(KEY_HUBCC);
    if (err == ESP_OK) {
        s_hub_set = false;
        memset(&s_hub, 0, sizeof(s_hub));
    }
    if (cc_err == ESP_OK) {
        s_hub_cc_set = false;
        memset(s_hub_cc, 0, sizeof(s_hub_cc));
    }
    xSemaphoreGive(s_mutex);
    return err;
}

int swarm_store_node_count(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_nodes.count;
    xSemaphoreGive(s_mutex);
    return n;
}

bool swarm_store_node_at(int idx, uint8_t mac_out[6], uint8_t lmk_out[SWARM_LMK_LEN])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = idx >= 0 && idx < s_nodes.count;
    if (ok) {
        if (mac_out) memcpy(mac_out, s_nodes.n[idx].mac, 6);
        if (lmk_out) memcpy(lmk_out, s_nodes.n[idx].lmk, SWARM_LMK_LEN);
    }
    xSemaphoreGive(s_mutex);
    return ok;
}

esp_err_t swarm_store_add_node(const uint8_t mac[6], const uint8_t lmk[SWARM_LMK_LEN])
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    nodes_blob_t blob = s_nodes;
    blob.format = SWARM_STORE_FORMAT;
    int idx = -1;
    for (int i = 0; i < blob.count; i++) {
        if (memcmp(blob.n[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (blob.count >= SWARM_MAX_NODES) {
            xSemaphoreGive(s_mutex);
            return ESP_ERR_NO_MEM;
        }
        idx = blob.count++;
        memset(&blob.n[idx], 0, sizeof(blob.n[idx]));  /* fresh slot: no name yet */
    }
    memcpy(blob.n[idx].mac, mac, 6);
    memcpy(blob.n[idx].lmk, lmk, SWARM_LMK_LEN);
    /* name is deliberately left as-is: re-adopting an already-known MAC
     * (see pairing.c's find_stored_lmk()/idempotent re-ack) must not wipe
     * an operator-assigned name out from under them. */

    esp_err_t err = write_blob(KEY_NODES, &blob, sizeof(blob));
    if (err == ESP_OK) s_nodes = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_set_node_name(const uint8_t mac[6], const char *name)
{
    if (!mac) return ESP_ERR_INVALID_ARG;
    size_t len = name ? strlen(name) : 0;
    if (len > SWARM_NODE_NAME_LEN) return ESP_ERR_INVALID_SIZE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nodes_blob_t blob = s_nodes;
    blob.format = SWARM_STORE_FORMAT;
    int idx = -1;
    for (int i = 0; i < blob.count; i++) {
        if (memcmp(blob.n[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    memset(blob.n[idx].name, 0, sizeof(blob.n[idx].name));
    if (len) memcpy(blob.n[idx].name, name, len);  /* len == 0 clears */

    esp_err_t err = write_blob(KEY_NODES, &blob, sizeof(blob));
    if (err == ESP_OK) s_nodes = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

bool swarm_store_node_name(const uint8_t mac[6], char out[SWARM_NODE_NAME_LEN + 1])
{
    if (!mac || !out) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_nodes.count; i++) {
        if (memcmp(s_nodes.n[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    bool found = idx >= 0;
    if (found) memcpy(out, s_nodes.n[idx].name, sizeof(s_nodes.n[idx].name));
    xSemaphoreGive(s_mutex);
    return found;
}

esp_err_t swarm_store_forget_node(const uint8_t mac[6])
{
    if (!mac) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nodes_blob_t blob = s_nodes;
    blob.format = SWARM_STORE_FORMAT;
    int idx = -1;
    for (int i = 0; i < blob.count; i++) {
        if (memcmp(blob.n[i].mac, mac, 6) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = idx; i < blob.count - 1; i++) blob.n[i] = blob.n[i + 1];
    blob.count--;
    memset(&blob.n[blob.count], 0, sizeof(blob.n[blob.count]));  /* clear the vacated tail slot */

    esp_err_t err = write_blob(KEY_NODES, &blob, sizeof(blob));
    if (err == ESP_OK) s_nodes = blob;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_clear_nodes(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = erase_key(KEY_NODES);
    if (err == ESP_OK) {
        memset(&s_nodes, 0, sizeof(s_nodes));
        s_nodes.format = SWARM_STORE_FORMAT;
    }
    xSemaphoreGive(s_mutex);
    return err;
}

bool swarm_store_pair_failed(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool f = s_pair_failed;
    xSemaphoreGive(s_mutex);
    return f;
}

esp_err_t swarm_store_set_pair_failed(bool failed)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_PFAIL, failed ? 1 : 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err == ESP_OK) s_pair_failed = failed;
    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t swarm_store_reset_all(void)
{
    /* Best-effort across all four: keep going and report the first failure
     * rather than bailing out partway and leaving some cleared and some
     * not -- a factory reset should end up as close to fully-clean as
     * possible even if one NVS write hiccups. */
    esp_err_t err = swarm_store_set_role(SWARM_ROLE_UNSET);
    esp_err_t e2 = swarm_store_clear_hub();
    esp_err_t e3 = swarm_store_clear_nodes();
    esp_err_t e4 = swarm_store_set_pair_failed(false);
    if (err == ESP_OK) err = e2;
    if (err == ESP_OK) err = e3;
    if (err == ESP_OK) err = e4;
    return err;
}
