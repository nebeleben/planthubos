#pragma once
#include "esp_err.h"
#include <stdint.h>

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
 * adv_decoder_task (Task 7's httpd handler will be) -- this only sets a
 * flag (ble_collector.c's s_wrapper_reindex_pending); the decoder task
 * itself performs the actual reindex at a safe point in its own loop,
 * never mid-decode. This is DELIBERATE, not a shortcut: s_wrapper_index,
 * the arena and s_wrapper_memo are all decoder-task-exclusive state with no
 * lock of their own, and wrapper_arena_get()'s returned pointer is
 * invalidated by ANY eviction regardless of caller (wrapper_arena.h's
 * FINDING 2 doc comment) -- a caller that rebuilt the index/evicted the
 * arena directly, from a different task, while the decoder task might be
 * mid-psvm_run() over a pointer into that same arena, would be memory
 * corruption, not merely a stale read. Do NOT change this back to an
 * immediate call, and do NOT add a mutex around wrapper_arena_get() to
 * "fix" a direct call instead -- keep new callers (Task 7's dry-run
 * endpoint included) routed through this request flag. */
void ble_collector_wrapper_reindex_request(void);
