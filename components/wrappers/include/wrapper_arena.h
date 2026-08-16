/* wrapper_arena.h -- the M3 Task 5 shared wrapper-bytecode arena (spec
 * section 2 "Wrapper arena"). Deliberately its OWN header, not folded into
 * wrapper_index.h alongside the match index and the LittleFS store
 * (controller ruling on task-5-brief.md: "the arena gets its own header,
 * not additions to wrapper_index.h. Three unrelated responsibilities in one
 * header is how files stop being reviewable.") -- wrapper_index.h already
 * carries two (the pure matcher, wrapper_index.c; the flash-backed store,
 * wrapper_store.c), and the arena is a third, independent one: a shared
 * LRU-evicted cache of LOADED bytecode blobs, packed by their actual size
 * rather than a fixed slot each, sized by CONFIG_PLANTHUB_WRAPPER_ARENA
 * (2048 B on esp32c3, 4096 B on esp32c5-- spec section 7's budget line).
 *
 * wrapper_arena.c is pure C99 (no ESP-IDF/FreeRTOS/file-I/O includes), same
 * discipline as wrapper_index.c, so tests/host/test_wrapper_arena.c compiles
 * and exercises it directly. The one thing it cannot be pure about --
 * actually reading a blob from flash -- is injected as a function pointer
 * (wrapper_loader_t) via wrapper_arena_set_loader() rather than called
 * directly, so this file never references wrapper_store_read_psbc()'s
 * symbol at all and the host test can install a fake loader that hands back
 * fabricated blobs from an in-memory table. Firmware wires the real one
 * (ble_collector.c's ble_collector_start(), before the decoder task can run):
 * wrapper_store_read_psbc()'s signature (wrapper_index.h) was deliberately
 * left matching wrapper_loader_t's exactly back in Task 2, for this file to
 * use today.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wrapper_index.h"   /* WRAPPERS_MAX -- the arena directory's upper
                               * bound (it can never hold more RESIDENT
                               * entries than there are wrappers, total) */

/* Retention is Kconfig-driven (components/wrappers/Kconfig); the host test
 * build (tests/host/run.sh, plain `cc`, no sdkconfig.h) never defines the
 * CONFIG_ symbol, so fall back to the same default as the Kconfig's own
 * (esp32c3's 2048 B) -- same pattern components/storage/include/storage.h
 * already uses for CONFIG_PLANTHUB_HISTORY_RAW_CAP. */
#ifndef CONFIG_PLANTHUB_WRAPPER_ARENA
#define CONFIG_PLANTHUB_WRAPPER_ARENA 2048
#endif
#define WRAPPER_ARENA_SIZE CONFIG_PLANTHUB_WRAPPER_ARENA

/* Loader signature -- identical to wrapper_store_read_psbc()'s (see this
 * header's own top comment). Reads exactly wrapper `id`'s bytecode into buf
 * (capacity cap); *len_out set on success. On a "doesn't fit in cap"
 * failure the loader MAY still report the blob's TRUE size via *len_out
 * (wrapper_store_read_psbc()'s read_whole_file() does, cheaply, via one
 * extra ftell()) so wrapper_arena_get() can tell "would fit after evicting
 * N bytes" from "will never fit the whole arena, refuse without evicting
 * anything" -- see that function's own doc comment for the exact contract
 * and what happens when a loader can't/doesn't report it (*len_out left at
 * its caller-supplied 0). */
typedef bool (*wrapper_loader_t)(uint16_t id, uint8_t *buf, size_t cap, size_t *len_out);

/* Must be called once, any time before the first wrapper_arena_get() (the
 * host test and firmware both do this once, at startup) -- the arena has no
 * usable default loader; a NULL loader (the state before this is ever
 * called) makes every wrapper_arena_get() a permanent, well-defined miss
 * (NULL, never a crash) rather than needing a "loader wired yet?" check at
 * every call site. Independent of wrapper_arena_init()/wrapper_arena_evict_all()
 * below, which reset RESIDENT state only -- this is a one-time wiring, not
 * part of the "clear the cache" contract either of those two implement. */
void wrapper_arena_set_loader(wrapper_loader_t loader);

/* Resets the arena to empty (no resident blobs, no directory entries).
 * Called once at boot (after wrapper_arena_set_loader()), before the
 * decoder task can run. */
void wrapper_arena_init(void);

/* Returns a pointer to wrapper `id`'s loaded bytecode blob (loading it via
 * the injected loader on a cache miss, evicting least-recently-used
 * resident blobs one at a time -- oldest last_used first -- only as far as
 * needed to make room), or NULL when the blob cannot fit (even after
 * evicting every other resident blob) or cannot be read at all. *len_out is
 * set to the blob's length on a hit. A resident blob's "last used" order is
 * bumped on every successful call that returns it, hit or fresh load alike,
 * so a genuinely idle wrapper (never re-matched) is the one evicted first
 * under pressure -- true LRU, not merely insertion order. */
const uint8_t *wrapper_arena_get(uint16_t id, size_t *len_out);

/* Empties the arena (every resident blob evicted, unconditionally) without
 * touching the installed loader. Called on wrapper install/delete (any
 * change that could make a previously loaded blob for some id stale or
 * outright wrong) -- see ble_collector.h's ble_collector_wrapper_reindex(),
 * which pairs this with rebuilding the match index and clearing the
 * per-device memo, the three things spec section 2 says a wrapper
 * install/delete must invalidate together. */
void wrapper_arena_evict_all(void);
