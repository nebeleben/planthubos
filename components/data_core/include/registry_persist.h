#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "registry.h"

/* Latest-value persistence for the device registry.
 *
 * The registry (registry.c) is RAM-only, so after a reboot /api/v1/devices is
 * blank until each device reports again -- a sleepy sensor can be silent for
 * many minutes. This module snapshots the registry's LATEST per-device values
 * (not a timeline -- sensor history was deliberately kept out to leave the
 * storage partition for plant history) to one small LittleFS file, and
 * restores it on boot so last-known readings show immediately.
 *
 * Restored values must not masquerade as fresh: last_seen/updated are UPTIME
 * seconds (esp_timer), meaningless across a reboot, so a restored row is
 * flagged snapshot_only (registry.h) and rendered stale by devices_json with
 * its age taken from the snapshot's wall-clock epoch instead. The flag clears
 * on the device's first live report.
 *
 * Split like actor_persist.c: a pure, host-testable serialize/deserialize
 * core, and an #ifdef ESP_PLATFORM LittleFS wrapper (save on the sampler
 * tick, load once at boot).
 *
 * File layout (little-endian), CRC-16/CCITT-FALSE over every byte except the
 * crc field itself:
 *   [0]      fmt (= REGISTRY_PERSIST_FMT)
 *   [1..2]   crc16
 *   [3..6]   written_epoch (unix seconds when the snapshot was taken)
 *   [7]      device count (<= REGISTRY_MAX_DEVICES)
 *   then per device:
 *     kind(1) addr(8) via_valid(1) via(6) best_rssi(1) cap_count(1)
 *     then cap_count * cap row   -- only valid caps
 *
 *   FMT 1 (old, still readable): cap row = { cap_id(1) raw(2) }, 3 bytes.
 *   No endpoint info at all -- a FMT-1 snapshot loads with every device's
 *   cap_endpoint[]/extra_ep_cap_count left at 0 (untracked/legacy; single-
 *   endpoint devices are unaffected either way since registry_get_cap_ep()
 *   treats an untracked cap_endpoint as "endpoint 0 == the main slot").
 *
 *   FMT 2 (current): cap row = { cap_id(1) raw(2) endpoint(1) }, 4 bytes --
 *   the trailing `endpoint` byte is that cap's tracked DEFAULT endpoint
 *   (cap_endpoint[cap_id] in registry.h; 0 if the device predates endpoint
 *   tracking). Then, per device, right after that device's cap_count cap
 *   rows, an additive side-list section for JUST that device:
 *     extra_ep_cap_count(1)
 *     then extra_ep_cap_count * { cap_id(1) endpoint(1) raw(2) }
 *   mirroring registry.h's device_entry_t.extra_ep_caps 1:1 (same fields,
 *   same order, restored valid=true since only written rows are stored).
 */

#define REGISTRY_PERSIST_FMT        2
#define REGISTRY_PERSIST_HEADER_LEN 8
/* Worst case (FMT 2): 16 devices * (ROW_FIXED=18 + CAPABILITY_COUNT*4 +
 * 1 + REGISTRY_EXTRA_EP_CAPS*4) + header = 16 * (18+44+1+16) + 8 = 1272.
 * A fixed static buffer (this module is called from one task at a time). */
#define REGISTRY_PERSIST_MAX_BYTES  1536

/* Serialize the in-use rows of `r` into `buf` (capacity `cap`). Returns the
 * byte count written, or 0 if it does not fit or `r`/`buf` is NULL. */
size_t registry_persist_serialize(const registry_t *r, uint32_t written_epoch,
                                  uint8_t *buf, size_t cap);

/* Parse `buf` into `out` (which is fully reset first): every restored row is
 * marked in_use with snapshot_only = true and last_seen_s/updated_s = 0.
 * `*epoch_out` receives the snapshot's written_epoch. Returns false (and
 * leaves `out` empty) on a bad length, format, CRC, or an out-of-range field.
 */
bool registry_persist_deserialize(const uint8_t *buf, size_t len,
                                  registry_t *out, uint32_t *epoch_out);

#ifdef ESP_PLATFORM
/* Atomically write `len` already-serialized bytes to the snapshot file
 * (tmp + rename). The caller serializes under its own lock, then calls this
 * OUTSIDE the lock so the registry mutex is never held across flash I/O.
 * Returns false on an I/O failure (logged; the live registry is unaffected). */
bool registry_persist_write(const uint8_t *buf, size_t len);
/* Load the snapshot file into `out`. Returns the number of devices restored,
 * or -1 if the file is absent/unreadable/corrupt (then `out` is left empty).
 * `*epoch_out` receives the snapshot epoch (0 when it returns -1). */
int  registry_persist_load(registry_t *out, uint32_t *epoch_out);
#endif
