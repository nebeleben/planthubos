#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "actor_table.h"

/* actor_persist -- the guard table across a reboot (M5b whole-branch
 * review, ruling FINAL-persist).
 *
 * WHY THIS EXISTS. actor_table.c is RAM only. After an OTA, a brownout or
 * an ordinary power cycle the operator's LOCKOUT came up off, every
 * cooldown came up 0 and every hourly cap came up unlimited -- and
 * ble_collector.c sets s_actors_wired before any operator could re-set
 * them, so the first sensor update could fire a rule straight into an
 * unlocked actuator. Worse, a reboot LOOP reset the flood counter on every
 * cycle, which directly negates spec section 4.2's "max 4 opens per hour
 * means the valve opens at most four times" -- the property that prevents a
 * flood.
 *
 * KEYED ON THE DEVICE, NOT ON dev_idx. The registry is RAM-only and claims
 * slots in discovery order (data_core.c), so a registry index is stable for
 * a device's lifetime but NOT across a restart. Rows are therefore keyed by
 * device_id_t bytes (actor_table.h's ACTOR_DEVICE_KEY_LEN) and matched back
 * to a device when it is next declared as an actuator, which is exactly the
 * moment ble_collector.c has both the index and the identity in hand.
 *
 * SHAPE. The same split pending_close.c already established in this
 * component: a pure, host-tested serialise/deserialise pair (plus the pure
 * merge/apply primitives in actor_table.c) and a thin ESP_PLATFORM-gated
 * LittleFS wrapper using tmp + rename, a format byte and a CRC. A corrupt
 * or truncated file yields NOTHING -- never a partial table, because half a
 * guard table is a guard table an operator did not configure.
 *
 * TASK OWNERSHIP. Every function in the impure half runs on
 * ble_collector.c's adv_decoder_task, the only task in this firmware
 * allowed to touch flash. A guard change arriving from the httpd task does
 * not write anything itself -- it sets a dirty flag that
 * actor_persist_service() drains on the next decoder tick (20 ms), the same
 * discipline the switch.state write and the pending-close deferral both
 * follow.
 *
 * WRITE POLICY. The file is rewritten on every change to guard CONFIG
 * (cooldown, hourly cap, lockout) AND on every ACTIVATION. Not throttled,
 * deliberately: the counter has to be durable at the moment it is spent, or
 * a crash refunds it and the reboot-loop hole this module exists to close
 * is only made narrower rather than closed. The wear is bounded and is not
 * a new cost of this module -- an activation requires a full BLE connect /
 * write / confirm / disconnect cycle (seconds, bounded by the engine's own
 * attempt deadline), and spec section 4.5 already costed and accepted one
 * small flash write per timed open on this same event. An operator who
 * cares sets max_per_hour, which caps it hard at 4 writes an hour per pair,
 * which is the guard's own purpose. */

#define ACTOR_PERSIST_FMT        1u
#define ACTOR_PERSIST_HEADER_LEN 4u
#define ACTOR_PERSIST_RECORD_LEN 23u
#define ACTOR_PERSIST_BUF_MAX \
    (ACTOR_PERSIST_HEADER_LEN + ACTOR_GUARD_ROWS_MAX * ACTOR_PERSIST_RECORD_LEN)

/* On-disk format, little-endian throughout:
 *
 *   u8  fmt = ACTOR_PERSIST_FMT
 *   u8  count                        (0..ACTOR_GUARD_ROWS_MAX)
 *   u16 crc                          CRC-16/CCITT-FALSE (poly 0x1021, init
 *                                     0xFFFF) over the count byte and every
 *                                     record byte -- not `fmt`, not itself.
 *                                     Same algorithm event_ring.c and
 *                                     pending_close.c both use.
 *   count x 23 B {
 *     u8  key[9]                     device_id_t bytes (kind + addr[8])
 *     u8  action_id
 *     u8  lockout                    0/1, device-level
 *     u16 cooldown_s
 *     u8  max_per_hour
 *     u8  window_count
 *     u32 window_start_s             uptime at save; re-based on load
 *     u32 last_fire_s                uptime at save; re-based on load
 *   }
 *
 * actor_persist_serialize() returns the number of BYTES written, or 0 on
 * any failure (n above ACTOR_GUARD_ROWS_MAX, or a `cap` too small) --
 * writing NOTHING rather than a truncated file.
 *
 * actor_persist_deserialize() returns the number of ROWS recovered, or 0
 * for ANY of: a short header, the wrong format byte, a length that is not
 * EXACTLY header + count * record (so a truncated file and one with
 * trailing garbage are both refused outright), a count above `cap`, a CRC
 * mismatch, a record naming an action_id this firmware does not know, or a
 * record with the all-zero "no key" sentinel. Nothing partial, ever. */
size_t actor_persist_serialize(const actor_guard_row_t *rows, size_t n,
                                uint8_t *buf, size_t cap);
size_t actor_persist_deserialize(const uint8_t *buf, size_t len,
                                  actor_guard_row_t *out, size_t cap);

/* Index of the first row at or after `from` whose key matches, or -1.
 * Pure, and the whole of what "find this device's saved guards" means --
 * a device has one row per declared action, so a caller walks it. */
int actor_persist_find(const actor_guard_row_t *rows, size_t n,
                        const uint8_t key[ACTOR_DEVICE_KEY_LEN], size_t from);

/* Loads the file into this module's in-RAM image. Call once at boot, after
 * actor_init() and before any device can be declared. Safe (a no-op image)
 * when the file is absent or unreadable. */
void actor_persist_init(void);

/* Re-applies the saved guards for `dev_idx`, whose stable identity is
 * `key`, to every action that device has just declared. Called from the
 * same code path that declares an actuator, so the guards are back in the
 * table before actor_table_check() can pass a command for it. */
void actor_persist_restore_device(int dev_idx, const uint8_t key[ACTOR_DEVICE_KEY_LEN]);

/* Folds the live table into the image and rewrites the file, if and only
 * if something has changed since the last call. Pumped from
 * adv_decoder_task's loop; a no-op flag test otherwise. */
void actor_persist_service(void);
