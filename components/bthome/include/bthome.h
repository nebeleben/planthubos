/* bthome.h -- M3 spec §4 "Native BTHome and bind keys". BTHome v2 is decoded
 * in native C, not as a wrapper: it is a published standard whose single
 * decoder covers every conforming device, matched by its service UUID
 * (0xFCD2) BEFORE the 16-wrapper index is ever consulted (ble_collector.c) --
 * it is built in and costs none of the user's wrapper slots.
 *
 * bthome.c is pure C99 (no ESP-IDF headers besides mbedtls's CCM, which is
 * itself plain C and already linked into the firmware for claiming --
 * see components/claiming/CMakeLists.txt), so it is host-testable exactly
 * like mibeacon.c. bindkey.c is the LittleFS/NVS-touching half and is NOT
 * host-tested, matching the wrapper_index.c/wrapper_store.c split
 * (wrapper_index.h's own header comment) and this repo's existing
 * convention that flash-touching code stays off run.sh.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define BTHOME_SVC_UUID 0xFCD2
#define BTHOME_MAX_EMITS 8

typedef struct { uint8_t cap_id; float value; } bthome_emit_t;
typedef enum { BTHOME_OK = 0, BTHOME_ERR_FORMAT, BTHOME_ERR_TRUNCATED,
               BTHOME_ERR_ENCRYPTED_NO_KEY, BTHOME_ERR_DECRYPT } bthome_err_t;

/* Decodes a BTHome v2 service-data payload (the bytes AFTER the 0xFCD2 UUID).
 * key may be NULL; it is required only when the payload's encryption bit is
 * set. Unknown object ids are skipped, not fatal. Writes at most
 * BTHOME_MAX_EMITS -- out[i].value is a REAL-UNIT float (deg C, %, lux, ...),
 * ready for capability_encode(); this file never calls capability_encode()
 * itself so it stays free of the "never store CAP_VALUE_NONE" discipline --
 * that is the caller's job (ble_collector.c decode_adv_item(), per the M3
 * Task 3 brief and data_core.c's existing skip-on-out-of-range precedent).
 *
 * mac[6] MUST be the device's Bluetooth address in HUMAN-READABLE / DISPLAY
 * byte order (the order printed as AA:BB:CC:DD:EE:FF, and the order
 * device_id_from_mac()/the bind-key API use) -- NOT NimBLE's raw
 * ble_addr_t.val[] wire order, which is reversed (val[5] carries the
 * top/type bits per nimble/ble.h's BLE_ADDR_IS_RPA et al, i.e. val[0] is the
 * first octet transmitted on air = the LEAST significant octet of the
 * displayed address). This matters because the encrypted form's AES-CCM
 * nonce is built from this MAC (see bthome.c's own comment on the nonce
 * layout) -- get the byte order wrong and every encrypted decrypt fails
 * with BTHOME_ERR_DECRYPT. Callers must reverse a raw GAP address before
 * calling this (see ble_collector.c's BTHome dispatch). */
bthome_err_t bthome_decode(const uint8_t *data, size_t len, const uint8_t key[16],
                           const uint8_t mac[6], bthome_emit_t *out, size_t *n_out);

/* ---------------- bindkey.c (NVS-backed key store) ----------------
 * NVS namespace "planthub" (the same shared namespace app_config.c,
 * claim.c and swarm_store.c already use -- distinct key names there, see
 * below, so no collision with THEM), one blob per device, keyed by dev_id
 * -- the device-id STRING (device_id_format()'s output, e.g.
 * "ble:AABBCCDDEEFF").
 *
 * That string does not become the literal NVS key: ESP-IDF's
 * NVS_KEY_NAME_MAX_SIZE is 16 bytes INCLUDING the null terminator (nvs.h),
 * i.e. a 15-char ceiling, and "ble:AABBCCDDEEFF" is already 16 characters
 * -- one over the limit -- before a longer espnow:/zb: id is even
 * considered. A literal `nvs_open`/`nvs_set_blob(..., dev_id, ...)` would
 * therefore fail for every BLE device this feature targets.
 *
 * bindkey_core.h/.c (host-testable, no NVS dependency) instead hash dev_id
 * (FNV-1a 64-bit, using the full 15-hex-char NVS key-name budget, not a
 * truncated 32-bit/8-char version) into the actual NVS key name, AND tag
 * the stored blob itself with dev_id (bindkey_blob_t: 16 key bytes + the
 * device-id string, see bindkey_core.h) so a collision between two
 * DIFFERENT device ids that happen to hash to the same 15-char name is
 * DETECTABLE and SAFE, not merely unlikely: bindkey_get()/bindkey_has()
 * verify the stored tag against the id actually being asked about and
 * treat a mismatch identically to "no key exists" (logged, never handing
 * back the wrong device's key material), and bindkey_set()'s clear path
 * (key == NULL) only ever erases a slot it has verified belongs to the
 * calling dev_id. See bindkey_core.h's own doc comments for the exact
 * contract of each pure helper, and test_bindkey_core.c
 * (tests/host/run.sh) for the collision-safety proof. */
bool bindkey_get(const char *dev_id, uint8_t out[16]);
bool bindkey_set(const char *dev_id, const uint8_t key[16]);   /* key NULL clears */
bool bindkey_has(const char *dev_id);
