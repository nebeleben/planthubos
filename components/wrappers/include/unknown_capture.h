/* unknown_capture.h -- M3 Task 6: the unknown-device advertisement capture
 * (spec section 5 "Unknown-device discovery"). Its own header, not folded
 * into wrapper_index.h alongside the match index and the LittleFS store
 * (controller ruling on task-6-brief.md: "the capture gets its OWN header,
 * not additions to wrapper_index.h. Three unrelated responsibilities in one
 * header is how files stop being reviewable." -- wrapper_index.h already
 * carries two, wrapper_arena.h a third of its own (Task 5's own ruling);
 * this capture is a fourth, independent one: a small rolling buffer of raw
 * advertisements that matched NOTHING (no BTHome, no wrapper, no MiFlora),
 * kept so a human -- or M4's AI wrapper generation, spec section 5's own
 * words -- can look at what the hub is hearing but doesn't understand yet.
 *
 * unknown_capture.c is pure C99 (no ESP-IDF/FreeRTOS/file-I/O includes),
 * same discipline wrapper_index.c and wrapper_arena.c's core hold
 * themselves to, so tests/host/test_unknown_capture.c compiles and
 * exercises this file directly.
 *
 * DELIBERATE SIGNATURE DEVIATION from task-6-brief.md's literal
 * `void unknown_capture_add(const adv_item_t *it)`: taking adv_item_t here
 * would require this header to #include "adv_queue.h", which lives in
 * components/ble_collector/include -- and components/ble_collector already
 * PRIV_REQUIRES wrappers (for wrapper_index.h/wrapper_arena.h/wrapper_exec.h,
 * genuine existing uses). Adding the reverse dependency (wrappers requires
 * ble_collector, for this one struct) would make the two components mutually
 * require each other, which is a real ESP-IDF build-graph cycle -- no
 * existing component pair in this codebase does that (checked: `grep
 * REQUIRES` across every component's CMakeLists.txt turns up zero cycles),
 * and per the controller's explicit "do NOT run idf.py build" instruction
 * for this task, introducing an untested one would be reckless. Decomposing
 * the advert into its raw fields instead is exactly the precedent
 * wrapper_exec.h already set for the same reason (see its own top comment):
 * `wrapper_exec_run(uint16_t id, const uint8_t mac[6], const uint8_t
 * *payload, uint8_t payload_len)` takes primitives, not an adv_item_t*,
 * despite conceptually operating on "one BLE advertisement" too.
 * ble_collector.c's decode_adv_item() (the only caller) already has a live
 * adv_item_t and just unpacks it at the call site -- see this header's own
 * unknown_capture_add() doc comment below. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Must match adv_queue.h's ADV_PAYLOAD_MAX (31) exactly -- both describe
 * the same "one AD structure's worth of raw advertisement bytes" quantity.
 * Guarded rather than #include "adv_queue.h" (see this header's top comment
 * on why that would create a circular component dependency): when
 * ble_collector.c includes adv_queue.h before this header (it does), the
 * macro is already defined to 31 and this is a no-op; the host test, which
 * never includes adv_queue.h at all, gets the same fallback wrapper_arena.h
 * already uses for CONFIG_PLANTHUB_WRAPPER_ARENA. */
#ifndef ADV_PAYLOAD_MAX
#define ADV_PAYLOAD_MAX 31
#endif

/* Spec section 5's fixed sizing: 8 devices x 2 most-recent samples,
 * ~700 B resident (spec section 7's budget line). See unknown_capture.c's
 * top comment for the measured byte cost. */
#define UNKNOWN_DEVICES 8
#define UNKNOWN_SAMPLES 2

typedef struct {
    uint8_t  payload[ADV_PAYLOAD_MAX];
    uint8_t  len;
    int8_t   rssi;
    uint32_t ts;
} unknown_sample_t;

typedef struct {
    bool     in_use;
    uint8_t  mac[6];
    uint32_t last_seen_s;
    unknown_sample_t s[UNKNOWN_SAMPLES];
    uint8_t  n;
} unknown_dev_t;

/* Resets the capture to empty. Called once at boot
 * (ble_collector.c's ble_collector_start(), alongside wrapper_arena_init()),
 * before the decoder task can run. Also creates the internal mutex. */
void unknown_capture_init(void);

/* Records one advertisement that matched nothing (spec section 5) --
 * ble_collector.c's decode_adv_item() calls this from every "nothing
 * dispatched this" exit on its no-match path (BTHome didn't claim it, no
 * wrapper's match key hit, and the native MiFlora check also missed).
 * mac/payload/len/rssi/ts are the item's own fields, unpacked at the call
 * site (see this header's top comment on why this takes primitives rather
 * than an adv_item_t*); `len` is clamped to ADV_PAYLOAD_MAX defensively
 * (the queue's own producer already guarantees this, but this module doesn't
 * get to trust a caller it wasn't compiled against).
 *
 * A first sighting of a MAC not already tracked creates a new device entry
 * (evicting the least-recently-seen tracked device first if all
 * UNKNOWN_DEVICES slots are in use -- spec section 5 "oldest device evicted
 * first", by last_seen_s, not insertion order). A MAC already tracked has
 * this sample appended to its rolling window: while fewer than
 * UNKNOWN_SAMPLES samples have been recorded it is appended to the next
 * free slot; once full, the oldest sample is dropped (shifted out) and this
 * one appended as the newest -- so `s[]` is always ordered oldest-first,
 * newest-last, and only the SAMPLE is rotated, never the device itself
 * (spec section 5: "8 devices x 2 most-recent payloads"). Every call
 * (first sighting or not) updates last_seen_s, which is what eviction reads.
 *
 * This function is internally synchronised and safe to call from any task. */
void unknown_capture_add(const uint8_t mac[6], const uint8_t *payload,
                          uint8_t len, int8_t rssi, uint32_t ts);

/* Removes a tracked device (spec section 5: "a device that later matches a
 * wrapper is removed from the capture"). Since BTHome and native MiFlora
 * are both checked in decode_adv_item() BEFORE it ever reaches the
 * unknown_capture_add() call sites, the only way a device already in this
 * table can later "start matching" is a wrapper install/reindex making a
 * previously-unindexed advert now resolve to a wrapper id -- so
 * ble_collector.c calls this from decode_adv_item()'s wrapper-match branch
 * (`wrapper_id >= 0`), not from do_wrapper_reindex() itself (reindexing
 * only rebuilds the index/arena/memo; it doesn't re-walk this table, and
 * doesn't need to -- the very next advertisement from a newly-matching
 * device reaches this call naturally). No-op (not an error) if mac isn't
 * currently tracked -- most calls here are exactly that, since most
 * matched devices were never unknown in the first place.
 *
 * This function is internally synchronised and safe to call from any task. */
void unknown_capture_forget(const uint8_t mac[6]);

/* Copies up to `max` in-use tracked devices into out[], returns the count
 * actually copied. Order is internal table-slot order (not recency-sorted);
 * spec section 5's `GET /api/v1/unknown` (a later task) is free to sort its
 * JSON response however it likes -- this is just a snapshot read.
 *
 * This function is internally synchronised and safe to call from any task. */
size_t unknown_capture_list(unknown_dev_t *out, size_t max);
