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
 * claim.c and swarm_store.c already use -- distinct fixed-string keys
 * there, see NVS-key-length note below, so no collision), one 16-byte blob
 * per device.
 *
 * dev_id is the device-id STRING (device_id_format()'s output, e.g.
 * "ble:AABBCCDDEEFF") -- but that string does not become the literal NVS
 * key. ESP-IDF's NVS_KEY_NAME_MAX_SIZE is 16 bytes INCLUDING the null
 * terminator (nvs.h), i.e. a 15-char ceiling, and "ble:AABBCCDDEEFF" is
 * already 16 characters -- one over the limit -- before a longer
 * espnow:/zb: id is even considered (espnow: id = 19 chars, zb: id = 19
 * chars). A literal `nvs_open`/`nvs_set_blob(..., dev_id, ...)` would
 * therefore fail (or silently truncate, depending on IDF version) for
 * every BLE device this feature targets. bindkey.c instead hashes dev_id
 * (FNV-1a 32-bit, formatted as 8 lowercase hex chars) into the actual NVS
 * key -- well inside the 15-char limit for any device kind, and, with at
 * most REGISTRY_MAX_DEVICES (16) devices ever paired to one hub, a 32-bit
 * hash collision between two of them is astronomically unlikely
 * (birthday-bound ~16*15/2 / 2^32 ~= 3e-8). The blob VALUE stored is still
 * exactly the 16 raw key bytes -- the spec's "16 bytes per device-id
 * string" contract for what Task 7's POST /api/v1/devices/<id>/key route
 * writes, unpadded by any collision-detection tag. */
bool bindkey_get(const char *dev_id, uint8_t out[16]);
bool bindkey_set(const char *dev_id, const uint8_t key[16]);   /* key NULL clears */
bool bindkey_has(const char *dev_id);
