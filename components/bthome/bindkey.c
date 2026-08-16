/* bindkey.c -- NVS-backed per-device BTHome/wrapper bind-key store (M3 spec
 * §4). Not part of tests/host: it is pure ESP-IDF NVS I/O, the same reason
 * wrapper_store.c (its LittleFS analogue, see wrapper_index.h's header
 * comment) stays off run.sh. See bthome.h's own doc comment for why the
 * NVS key actually written is a hash of dev_id, not dev_id itself
 * (NVS_KEY_NAME_MAX_SIZE's 15-char ceiling).
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
 * atomic against a concurrent read of survives-a-crash. */
#include "bthome.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

#define BINDKEY_NS  "planthub"
#define BINDKEY_LEN 16

/* FNV-1a 32-bit -- see bthome.h's doc comment: turns an arbitrary-length
 * device-id string into an 8-hex-char NVS key, always well inside the
 * 15-char (NVS_KEY_NAME_MAX_SIZE - 1) limit regardless of device kind. */
static uint32_t fnv1a32(const char *s)
{
    uint32_t h = 0x811C9DC5u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 0x01000193u;
    }
    return h;
}

static void nvs_key_from_devid(const char *dev_id, char out[NVS_KEY_NAME_MAX_SIZE])
{
    /* 8 hex chars + NUL = 9 bytes, comfortably under NVS_KEY_NAME_MAX_SIZE
     * (16); snprintf's own bound is belt-and-braces, not load-bearing. */
    snprintf(out, NVS_KEY_NAME_MAX_SIZE, "%08x", (unsigned)fnv1a32(dev_id));
}

bool bindkey_get(const char *dev_id, uint8_t out[16])
{
    if (!dev_id || !out) return false;
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_key_from_devid(dev_id, key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = BINDKEY_LEN;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == BINDKEY_LEN;
}

bool bindkey_set(const char *dev_id, const uint8_t key_bytes[16])
{
    if (!dev_id) return false;
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_key_from_devid(dev_id, key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    esp_err_t err;
    if (key_bytes == NULL) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   /* clearing an absent key is not a failure */
    } else {
        err = nvs_set_blob(h, key, key_bytes, BINDKEY_LEN);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool bindkey_has(const char *dev_id)
{
    if (!dev_id) return false;
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_key_from_devid(dev_id, key);

    nvs_handle_t h;
    if (nvs_open(BINDKEY_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    nvs_close(h);
    return err == ESP_OK && len == BINDKEY_LEN;
}
