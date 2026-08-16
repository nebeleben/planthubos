/* bindkey.c -- NVS-backed per-device BTHome/wrapper bind-key store (M3 spec
 * §4). Not part of tests/host: it is pure ESP-IDF NVS I/O, the same reason
 * wrapper_store.c (its LittleFS analogue, see wrapper_index.h's header
 * comment) stays off run.sh. Its actual logic -- the dev_id -> NVS-key-name
 * hash, and the collision-safety check that makes a hashed-name collision
 * detectable and safe rather than merely unlikely -- lives in
 * bindkey_core.c instead, specifically so THAT part is host-testable
 * (test_bindkey_core.c); this file is a thin nvs_open/get/set/close shell
 * around it. See bindkey_core.h for the full rationale.
 *
 * Deliberately no RAM cache and no mutex: every call opens/closes its own
 * nvs_handle_t (same shape as swarm_store.c's write_blob()/erase_key()
 * helpers), which NVS itself keeps cheap (no flash erase on a same-size
 * blob overwrite). The read side (bindkey_get()) runs on adv_decoder_task
 * only, for one device at a time, once per encrypted BTHome advertisement --
 * there is no second task that could race it, unlike s_batt_tab in
 * ble_collector.c which genuinely is shared across two tasks and needs
 * s_batt_mutex. The write side is Task 7's future HTTP handler, on its own
 * task, touching a different key per call; NVS's own internal locking
 * (nvs_open/nvs_set_blob/nvs_commit) already makes a single blob write
 * atomic against a concurrent read or a crash mid-write. */
#include "bthome.h"
#include "bindkey_core.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include <string.h>

#define BINDKEY_NS "planthub"
static const char *TAG = "bindkey";

/* bindkey_core.h keeps its own copy of this number (rather than including
 * nvs.h) so it stays host-testable; this assert is what keeps that copy
 * from ever silently drifting away from the real ESP-IDF constant. */
_Static_assert(BINDKEY_NVS_KEY_BUF == NVS_KEY_NAME_MAX_SIZE,
               "bindkey_core.h's BINDKEY_NVS_KEY_BUF must match NVS_KEY_NAME_MAX_SIZE");

/* Reads and verifies the blob at dev_id's hashed NVS slot -- see
 * bindkey_blob_verify()'s doc comment for what "verifies" means and why:
 * a wrong-size/absent/differently-shaped blob, OR a blob that decodes fine
 * but is tagged for a DIFFERENT device id (a hashed-name collision), are
 * both treated identically to "no key for dev_id", never as "here is a
 * key, hope it's the right one". A collision is still logged (WARN,
 * naming both ids) so it is visible rather than a silent, permanently
 * mysterious decrypt failure. */
static bool read_verified_blob(nvs_handle_t h, const char *nvs_key, const char *dev_id,
                                bindkey_blob_t *out)
{
    size_t len = sizeof(*out);
    if (nvs_get_blob(h, nvs_key, out, &len) != ESP_OK || len != sizeof(*out)) return false;
    if (!bindkey_blob_verify(dev_id, out, NULL)) {
        ESP_LOGW(TAG, "NVS slot %s holds a key for '%.23s', not '%.23s' -- "
                      "hashed-name collision, treating as no key",
                 nvs_key, out->dev_id, dev_id);
        return false;
    }
    return true;
}

bool bindkey_get(const char *dev_id, uint8_t out[16])
{
    if (!dev_id || !out) return false;
    char nvs_key[BINDKEY_NVS_KEY_BUF];
    bindkey_nvs_key_for(dev_id, nvs_key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bindkey_blob_t blob;
    bool ok = read_verified_blob(h, nvs_key, dev_id, &blob);
    nvs_close(h);
    if (!ok) return false;

    memcpy(out, blob.key, BINDKEY_LEN);
    return true;
}

bool bindkey_set(const char *dev_id, const uint8_t key_bytes[16])
{
    if (!dev_id) return false;
    char nvs_key[BINDKEY_NVS_KEY_BUF];
    bindkey_nvs_key_for(dev_id, nvs_key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    esp_err_t err;
    if (key_bytes == NULL) {
        /* Clearing: only erase the slot if it actually verifies as
         * dev_id's -- a hashed-name collision must never let one device's
         * clear wipe a different device's key. An absent slot, a
         * wrong-shaped blob, or a blob tagged for a different device id
         * all mean "nothing to clear for dev_id", which is success, not
         * failure (same "clearing an absent key is not a failure"
         * reasoning the original version of this function already used,
         * just now gated on ownership too). */
        bindkey_blob_t existing;
        if (read_verified_blob(h, nvs_key, dev_id, &existing)) {
            err = nvs_erase_key(h, nvs_key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        } else {
            err = ESP_OK;
        }
    } else {
        /* A collision on WRITE is logged, not refused -- refusing would
         * mean a legitimate (re-)bind can permanently fail because some
         * unrelated device's id happened to hash the same. What actually
         * keeps the original device safe is read_verified_blob() above:
         * once overwritten, the original device's own bindkey_get() will
         * correctly report "no key" (its dev_id tag no longer matches this
         * slot) rather than being handed the new device's key material. */
        size_t len = sizeof(bindkey_blob_t);
        bindkey_blob_t existing;
        if (nvs_get_blob(h, nvs_key, &existing, &len) == ESP_OK && len == sizeof(existing) &&
            !bindkey_blob_verify(dev_id, &existing, NULL)) {
            ESP_LOGW(TAG, "NVS slot %s already holds a key for '%.23s' -- "
                          "hashed-name collision with '%.23s', overwriting",
                     nvs_key, existing.dev_id, dev_id);
        }

        bindkey_blob_t blob;
        bindkey_blob_build(dev_id, key_bytes, &blob);
        err = nvs_set_blob(h, nvs_key, &blob, sizeof(blob));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool bindkey_has(const char *dev_id)
{
    if (!dev_id) return false;
    char nvs_key[BINDKEY_NVS_KEY_BUF];
    bindkey_nvs_key_for(dev_id, nvs_key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bindkey_blob_t blob;
    bool ok = read_verified_blob(h, nvs_key, dev_id, &blob);
    nvs_close(h);
    return ok;
}
