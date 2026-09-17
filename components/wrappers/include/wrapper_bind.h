#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-device wrapper binding: a persisted mac -> wrapper_id table so an
 * operator can point an existing wrapper at a specific BLE device whose
 * advert key never matched. MAC is DISPLAY order (device_id_t.addr), the
 * same order ble_collector's mac_disp and the /api ids use. See
 * docs/superpowers/specs/2026-09-17-planthub-per-device-wrapper-binding-design.md
 *
 * File format (little-endian), CRC-16/CCITT-FALSE over every byte but the
 * crc field: [0]=fmt(=1) [1..2]=crc [3]=count, then count * { mac[6], id(2) }.
 */
#define WRAPPER_BIND_MAX 16
#define WRAPPER_BIND_FMT 1

typedef struct { uint8_t mac[6]; uint16_t wrapper_id; } wrapper_bind_t;

void     wrapper_bind_reset(void);
bool     wrapper_bind_set(const uint8_t mac[6], uint16_t wrapper_id);
bool     wrapper_bind_clear(const uint8_t mac[6]);
uint16_t wrapper_bind_lookup(const uint8_t mac[6]);
size_t   wrapper_bind_list(wrapper_bind_t *out, size_t max);
/* Drop every binding whose wrapper_id `exists(id)` returns false for.
 * Returns the number removed. */
size_t   wrapper_bind_prune(bool (*exists)(uint16_t id));

size_t   wrapper_bind_serialize(uint8_t *buf, size_t cap);
bool     wrapper_bind_deserialize(const uint8_t *buf, size_t len);

#ifdef ESP_PLATFORM
/* Load the persisted table at boot (no-op leaving an empty table if the file
 * is absent or corrupt). Save atomically after any change. */
void wrapper_bind_load(void);
bool wrapper_bind_save(void);
#endif
