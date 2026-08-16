#pragma once
#include "esp_err.h"
#include <stdint.h>

esp_err_t ble_collector_start(void);

/* Advertisements dropped because the raw-advert queue (M3 spec §1) was
 * full when gap_event tried to push -- exposed at GET /api/v1/status as
 * "adv_dropped" (api_v1.c) so a saturated queue is visible, not silent. */
uint32_t ble_collector_adv_dropped(void);

/* M3 Task 5 (spec §2): rebuilds the wrapper match index from flash
 * (wrapper_store_load_all()) and invalidates BOTH the wrapper arena
 * (wrapper_arena_evict_all()) and the per-device "last matched wrapper"
 * memo. Must be called whenever a wrapper is installed, edited,
 * enabled/disabled or deleted -- Task 7's HTTP API is the intended caller
 * (none exists yet; wired here now so that task only needs to call this one
 * function rather than reach into three separate pieces of ble_collector.c
 * state). Not yet called by anything in M3 Task 5 for that reason.
 *
 * Callers: this touches s_wrapper_index and s_wrapper_memo, which today
 * only adv_decoder_task ever reads, with no lock of its own (same
 * reasoning ble_collector.c's own top-of-file comment gives for
 * s_wrapper_index needing none in Task 2) -- Task 7's caller runs on a
 * different task (the webserver's), so whoever wires the first real call to
 * this function is responsible for adding whatever synchronization that
 * cross-task access needs; nothing does yet because nothing calls this
 * yet. */
void ble_collector_wrapper_reindex(void);
