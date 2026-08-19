#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

esp_err_t ble_collector_start(void);

/* Advertisements dropped because the raw-advert queue (M3 spec §1) was
 * full when gap_event tried to push -- exposed at GET /api/v1/status as
 * "adv_dropped" (api_v1.c) so a saturated queue is visible, not silent. */
uint32_t ble_collector_adv_dropped(void);

/* M3 Task 5 (spec §2), request/perform split (Task 5 review FINDING 4 --
 * added on review, replacing an earlier direct ble_collector_wrapper_reindex()
 * that any caller could invoke immediately): REQUESTS that the wrapper
 * match index be rebuilt from flash (wrapper_store_load_all()) and BOTH the
 * wrapper arena (wrapper_arena_evict_all()) and the per-device "last
 * matched wrapper" memo be invalidated -- the three things spec §2 says a
 * wrapper install/delete must invalidate together. Must be called whenever
 * a wrapper is installed, edited, enabled/disabled or deleted -- Task 7's
 * HTTP API is the intended caller (none exists yet; wired here now so that
 * task only needs to call this one function). Not yet called by anything
 * in M3 Task 5 for that reason.
 *
 * Safe to call from ANY task, including a different one than
 * adv_decoder_task (this task's own httpd handler is one) -- this only sets
 * a flag (ble_collector.c's s_wrapper_reindex_pending); the decoder task
 * itself performs the actual reindex at a safe point in its own loop, never
 * mid-decode. This is DELIBERATE, not a shortcut: s_wrapper_index and the
 * arena are decoder-task-exclusive state with no lock of their own (no
 * reader anywhere else), and wrapper_arena_get()'s returned pointer is
 * invalidated by ANY eviction regardless of caller (wrapper_arena.h's
 * FINDING 2 doc comment) -- a caller that rebuilt the index/evicted the
 * arena directly, from a different task, while the decoder task might be
 * mid-psvm_run() over a pointer into that same arena, would be memory
 * corruption, not merely a stale read. s_wrapper_memo is invalidated by the
 * same reindex and so is bound by the same rule, even though (M5a Task 7
 * fix round 1) it is no longer read-exclusive to the decoder task --
 * see ble_collector_wrapper_for_device() below and ble_collector.c's
 * s_wrapper_reindex_pending doc comment for the corrected single-writer/
 * multi-reader shape. Do NOT change this back to an immediate call, and do
 * NOT add a mutex around wrapper_arena_get() to "fix" a direct call instead
 * -- keep new callers (the dry-run endpoint included) routed through this
 * request flag. */
void ble_collector_wrapper_reindex_request(void);

/* M5a Task 7 (spec §5/§6): read-only access to the device -> wrapper memo
 * (s_wrapper_memo, ble_collector.c) for GET /api/v1/devices' "gatt" object
 * (devices_json.c) -- it needs to know which wrapper a device last matched
 * so it can ask wrapper_exec_plan_get() whether that wrapper carries a
 * connect plan at all. Returns the wrapper id, or -1 when dev_idx is out of
 * range or has no memo recorded (WRAPPER_MEMO_NONE -- "no memo", NOT
 * "confirmed no wrapper matches"; see s_wrapper_memo's own doc comment).
 *
 * s_wrapper_memo is single-writer (adv_decoder_task), multi-reader
 * (adv_decoder_task's own per-advertisement check, and this function from
 * the httpd task) -- see ble_collector.c's s_wrapper_reindex_pending doc
 * comment for the full correction (an earlier version of that comment
 * claimed decoder-task exclusivity for this table too; false once this
 * function existed, fixed on review). Two different write shapes to weigh
 * against this read, not one:
 *
 *   - The per-advertisement store (decode_adv_item()) writes one
 *     naturally-aligned uint16_t element. A concurrent read of that same
 *     element can never be torn on this target -- same reasoning
 *     ble_collector_adv_dropped() and gatt_sched.h's module comment already
 *     rely on for their own cross-task scalar reads. Worst case: this
 *     function returns last advertisement's match instead of the one just
 *     decoded.
 *   - do_wrapper_reindex() (wrapper install/edit/delete) instead
 *     memset()s the WHOLE table to WRAPPER_MEMO_NONE (0xFFFF), byte by
 *     byte, not as one atomic multi-word store. A read racing that memset
 *     can therefore observe a byte-mixed value for one element -- neither
 *     the old id nor the sentinel. Because every byte memset() writes is
 *     0xFF, that mixed value is still bounded (never uninitialised memory,
 *     never a value memset() didn't put some of the bytes of), but it CAN
 *     numerically alias a real, currently-installed wrapper id, which
 *     wrapper_exec_plan_get() would then happily answer for -- one httpd
 *     response could show an unrelated wrapper's declared interval for that
 *     device. Vanishingly narrow (the reindex's memset is a handful of
 *     instructions, gated behind an operator installing/editing/deleting a
 *     wrapper) and self-correcting on the Devices tab's next 10s poll, so
 *     this is deliberately left unsynchronised rather than given a lock --
 *     but the possibility is real and belongs on the record, not papered
 *     over by a "never torn" claim that only covers the other write path. */
int ble_collector_wrapper_for_device(int dev_idx);

/* The connect-plan interval memoised for dev_idx by adv_decoder_task, in
 * seconds; 0 means "no connect plan" (or nothing memoised yet). This is how
 * GET /api/v1/devices and the SSE device payload learn that a device has a
 * plan at all -- they must NOT ask wrapper_exec_plan_get(), because that
 * reaches wrapper_arena_get() and the arena's only concurrency guarantee is
 * the decoder-task exclusivity this header's reindex-request comment above
 * insists on. See s_plan_interval_memo in ble_collector.c for the full
 * argument and for why an unsynchronised read of it is safe. */
uint32_t ble_collector_plan_interval_for_device(int dev_idx);

/* Hold (true) or release (false) passive scanning. Held, the collector
 * stays deliberately deaf: start_scan() becomes a no-op, so the GATT
 * engine's and battery poller's own scan resumes cannot lift the hold.
 *
 * The one caller is the Zigbee permit-join window -- see the measurement
 * table above s_scan_hold in ble_collector.c for why BLE scanning and
 * Zigbee joining cannot share the antenna. Calls are idempotent, so a
 * second hold or a release with no hold outstanding is harmless. */
void ble_collector_scan_hold(bool hold);

/* True while a Zigbee permit-join window holds the radio (see
 * ble_collector_scan_hold). Read by the other BLE connection owners so
 * they can defer their own work for the window's duration. */
bool ble_collector_scan_is_held(void);
