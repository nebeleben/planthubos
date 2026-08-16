/* bindkey_core.h -- the pure (no NVS, no ESP-IDF) half of the bind-key
 * store's logic: the dev_id -> NVS-key-name hash, and the blob layout that
 * makes a hashed-name collision DETECTABLE and SAFE rather than merely
 * unlikely (code review finding on the first version of this file, which
 * stored only the bare 16-byte key with no way to tell whose key a given
 * NVS slot actually held).
 *
 * Split out from bindkey.c specifically so this logic is host-testable
 * (test_bindkey_core.c, tests/host/run.sh) -- bindkey.c itself stays
 * ESP-IDF-only (nvs_open/nvs_get_blob/...), same as before, but now it is a
 * thin I/O shell around the functions declared here, which carry zero
 * platform dependency of their own. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BINDKEY_LEN 16

/* Worst-case device_id_format() output: "espnow:" (7) + 16 hex chars + NUL
 * = 24 bytes (capability.h's device_id_format() doc comment: "buf >= 24
 * bytes"). Sized to that exactly -- a BLE id ("ble:" + 12 hex + NUL = 17)
 * or a Zigbee one ("zb:" + 16 hex + NUL = 20) both fit with room to spare. */
#define BINDKEY_DEVID_MAX 24

/* NVS_KEY_NAME_MAX_SIZE (nvs.h): 16 bytes, INCLUDING the null terminator --
 * a 15-character ceiling on the key name itself. Given as its own constant
 * here (rather than including nvs.h, which would drag ESP-IDF into a file
 * meant to be host-testable) so this stays a single number that means the
 * same thing in both places; bindkey.c asserts at build time that it still
 * equals the real NVS_KEY_NAME_MAX_SIZE, so the two can never silently
 * drift apart. */
#define BINDKEY_NVS_KEY_BUF 16

/* device-id string tagged onto the 16-byte key so a reader can tell whose
 * key a given NVS slot actually holds -- see bindkey_blob_verify(). */
typedef struct {
    uint8_t key[BINDKEY_LEN];
    char    dev_id[BINDKEY_DEVID_MAX];   /* always NUL-terminated by bindkey_blob_build() */
} bindkey_blob_t;

/* FNV-1a 64-bit hash of dev_id, formatted as a 15-lowercase-hex-char
 * NUL-terminated string -- the full usable NVS key-name budget (not the
 * 8-char/32-bit truncation the first version of this file used, which
 * discarded 7 of the 15 available characters for no reason and needlessly
 * raised the collision rate). With the dev_id verification below, a
 * collision is no longer a correctness risk either way -- this widening is
 * purely defense in depth, making a collision rarer on top of harmless. */
void bindkey_nvs_key_for(const char *dev_id, char out[BINDKEY_NVS_KEY_BUF]);

/* Fills *blob ready to write under dev_id's hashed NVS key: copies key
 * verbatim and tags the blob with dev_id itself (truncated to
 * BINDKEY_DEVID_MAX - 1 chars if somehow longer -- never reachable for a
 * real device_id_format() output, see BINDKEY_DEVID_MAX's comment). */
void bindkey_blob_build(const char *dev_id, const uint8_t key[BINDKEY_LEN], bindkey_blob_t *blob);

/* The collision-safety check: verifies that *blob -- whatever was actually
 * read back from the NVS slot dev_id's hash names -- was written FOR
 * dev_id, not for some other device id that happens to hash to the same
 * name. On a match, copies the key into key_out (if non-NULL) and returns
 * true. On a mismatch, returns false and leaves *key_out untouched --
 * callers must treat this identically to "no key exists for dev_id", never
 * as "here is a key, hope it's the right one". Pure comparison, no I/O. */
bool bindkey_blob_verify(const char *dev_id, const bindkey_blob_t *blob, uint8_t key_out[BINDKEY_LEN]);
