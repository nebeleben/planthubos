/* wrapper_index.h -- the M3 matcher (spec §2 "Matcher and wrapper store")
 * plus wrapper_store.c's boot-time/read entry points. Two responsibilities,
 * one header, same reason adv_queue.h holds both the ring struct and its
 * pure API: ble_collector.c is the only firmware caller and wants a single
 * include; wrapper_index.c stays pure C99 (no file I/O, host-testable),
 * wrapper_store.c is the LittleFS side that fills a wrapper_index_t at boot.
 *
 * Match keys (spec §2 table): a wrapper declares exactly one match key in
 * its source header (`match <service|manufacturer|mac_prefix> <value>`), so
 * one wrapper id maps to exactly one wrapper_entry_t. Keys:
 *   - WMATCH_SERVICE:      the advert's 16-bit service-data UUID, widened
 *                          into `key`'s low 16 bits.
 *   - WMATCH_MANUFACTURER: the advert's 16-bit manufacturer-data company id,
 *                          same widening.
 *   - WMATCH_MAC_PREFIX:   the device's first 3 MAC bytes IN DISPLAY/HUMAN
 *                          ORDER -- the same order printed on the device,
 *                          shown in the Devices tab, and used by every other
 *                          MAC-bearing surface in this codebase
 *                          (device_id_from_mac(), GET /api/v1/unknown's
 *                          `id`, etc.) -- packed big-endian into `key`'s low
 *                          24 bits (mac[0]<<16 | mac[1]<<8 | mac[2], where
 *                          mac[0] is the FIRST byte a human reads/types, e.g.
 *                          0xD0 of D0:CF:13:E5:BC:CA). This is the OPPOSITE
 *                          of the raw GAP/on-air order ble_addr_t.val[]
 *                          uses, where val[0] is the LAST printed byte
 *                          (M3 review fix 3) -- wrapper_index_lookup()'s
 *                          `mac` parameter below must always be passed in
 *                          this display order, never raw GAP order; callers
 *                          that only have GAP order (ble_collector.c's
 *                          adv_item_t.mac) must reverse it first, exactly as
 *                          they already do to get mac_disp for
 *                          device_id_from_mac().
 * wrapper_index_lookup()'s svc_uuid/manu_id callers pass 0xFFFFFFFF for
 * "this advert had none" -- see its own comment -- so a real key (always
 * <= 0xFFFFFF) can never collide with the sentinel. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WRAPPERS_MAX      16
#define WRAPPER_SRC_MAX   4096
#define WRAPPER_PSBC_MAX  2048

typedef enum { WMATCH_SERVICE = 0, WMATCH_MANUFACTURER = 1, WMATCH_MAC_PREFIX = 2 } wmatch_kind_t;

typedef struct { uint8_t kind; uint32_t key; uint16_t id; uint8_t flags; } wrapper_entry_t;  /* 12 B */
typedef struct { wrapper_entry_t e[WRAPPERS_MAX]; uint8_t count; } wrapper_index_t;

void wrapper_index_init(wrapper_index_t *ix);
/* -1 when full or the (kind,key) pair is already taken. */
int  wrapper_index_add(wrapper_index_t *ix, uint8_t kind, uint32_t key, uint16_t id);
bool wrapper_index_remove(wrapper_index_t *ix, uint16_t id);
/* Returns the wrapper id, or -1. Caller supplies whatever the advert yielded:
 * svc_uuid/manu_id are 0xFFFFFFFF when absent. `mac` MUST be in display/
 * human order (see this header's top comment on WMATCH_MAC_PREFIX) -- NOT
 * the raw GAP order adv_item_t.mac/ble_addr_t.val[] use. */
int  wrapper_index_lookup(const wrapper_index_t *ix, uint32_t svc_uuid,
                          uint32_t manu_id, const uint8_t mac[6]);

/* Returns the WMATCH_* kind of the entry with id, or 0xFF if id isn't
 * indexed. Added for M3 Task 5 (wrapper execution): ble_collector.c's
 * decode_adv_item() needs to know WHICH slice of the raw advertisement to
 * hand a matched wrapper -- service-data after its 2-byte UUID,
 * manufacturer-data after its 2-byte company id, or the raw AD blob for a
 * mac_prefix match -- and wrapper_index_lookup() above only ever returns
 * the winning id, not its kind, so this is a small, separate, deliberately
 * non-breaking addition (an O(count) rescan of an id lookup() just
 * resolved) rather than widening lookup()'s own signature and disturbing
 * Task 2's already-reviewed contract/tests. */
uint8_t wrapper_index_kind_of(const wrapper_index_t *ix, uint16_t id);

/* ---------------- wrapper_store.c (LittleFS side) ----------------
 * One wrapper lives as three files under /storage/wrappers/, `<id>.wsrc`
 * (source text), `<id>.wbc` (bytecode) and `<id>.json` (meta: name, enabled,
 * match_kind, match_key -- Task 7 owns writing these; the shape mirrors
 * rules_store.c's rule meta exactly). id = u16 monotonic, never reused
 * (same invariant rules_store.c and plants_table.h document). */

/* mkdir the storage dir if missing, scan for `<id>.json` meta files and
 * index every ENABLED one into *ix (wrapper_index_init()'d first, so this
 * always leaves *ix in a known state even on an empty/missing directory).
 * A disabled wrapper's files are left alone but never indexed -- it cannot
 * match anything until re-enabled rebuilds the index (Task 7). Skips,
 * logs and continues past any wrapper whose meta is missing/corrupt or
 * whose match key collides/overflows the table, exactly like
 * rules_store_load_all()'s per-rule tolerance -- one bad wrapper must never
 * fail boot. Called once, before the decoder task can run. */
void wrapper_store_load_all(wrapper_index_t *ix);

/* Reads exactly one wrapper's bytecode into buf (capacity cap); *len_out is
 * set to the blob's length on success. False on any I/O/size problem -- EXCEPT
 * that a "file is longer than cap" failure specifically still sets *len_out
 * to the file's true size (wrapper_store.c's read_whole_file()), which M3
 * Task 5's wrapper_arena.c relies on to distinguish "would fit after
 * evicting" from "will never fit". This signature matches wrapper_arena.h's
 * wrapper_loader_t exactly -- wired as the real loader in
 * ble_collector.c's ble_collector_start() via wrapper_arena_set_loader(). */
bool wrapper_store_read_psbc(uint16_t id, uint8_t *buf, size_t cap, size_t *len_out);

/* ---------------- Task 7: wrapper CRUD + metadata (spec §6 API) ----------
 * Mirrors rules.h/rules_store.c's rule_info_t/rules_list()/rules_upsert()/
 * rules_delete() shape closely: a resident RAM metadata table (g_wrappers,
 * wrapper_store.c) that BOTH this task's httpd routes (api_v1.c: list/get/
 * create/update/delete) and the decoder task (wrapper_exec.c, once per
 * matched run -- wrapper_store_note_match()/note_error()) touch, so it is
 * guarded by its own mutex -- same "rules_engine.c already takes
 * g_rules_mutex briefly on every single rule evaluation" precedent already
 * in this codebase (a short, bounded critical section on a small metadata
 * table). This is NOT the match index/arena/device-memo triple
 * ble_collector.h's reindex-request split protects -- that trio stays
 * decoder-task-exclusive and untouched by any of this; a wrapper
 * install/update/delete still MUST go through
 * ble_collector_wrapper_reindex_request() afterwards (wrapper_store.c
 * cannot call it directly itself -- requiring ble_collector from wrappers
 * would be the exact component-dependency cycle this header's own
 * unknown_capture.h sibling already rules out -- so api_v1.c, which already
 * depends on ble_collector, is responsible for that call). */
#define WRAPPER_NAME_MAX 48

typedef struct {
    uint16_t id;
    char     name[WRAPPER_NAME_MAX + 1];
    bool     enabled;
    uint8_t  match_kind;      /* wmatch_kind_t */
    uint32_t match_key;
    uint32_t match_count;     /* diagnostic run counter -- see wrapper_store_note_match() */
    char     last_error[48];  /* "" = none -- see wrapper_store_note_error() */
} wrapper_info_t;

/* Copies up to `max` in-use wrappers (enabled or not) into out[]; returns
 * the count copied. Mutex-protected snapshot, safe to call from the httpd
 * task. Order is internal table-slot order, same "caller sorts if it cares"
 * convention unknown_capture_list() already uses. */
size_t wrapper_store_list(wrapper_info_t *out, size_t max);

/* Reads wrapper `id`'s source text into buf (capacity buflen), NUL-
 * terminated on success. False if id is unknown or the file is missing/
 * too large for buflen. Mirrors rules_get_source()'s contract exactly. */
bool wrapper_store_get_source(uint16_t id, char *buf, size_t buflen);

/* Create (*id_inout == 0) or update an existing wrapper. Validates name/
 * source/bytecode size limits (WRAPPER_NAME_MAX/WRAPPER_SRC_MAX/
 * WRAPPER_PSBC_MAX), parses the MANDATORY `wrapper "<name>" match
 * <service|manufacturer|mac_prefix> <key>` header out of `source` itself --
 * the wire body carries no separate match_kind/match_key field (spec §6),
 * so this is the only place that information exists -- validates the
 * bytecode via psvm_validate(dialect=2), and rejects:
 *   - a match key that collides with BTHome's built-in service UUID
 *     (0xFCD2) or MiFlora's native one (0xFE95): spec §4's "a user wrapper
 *     declaring the same key is rejected at install time" shadowing guard.
 *   - a (kind,key) pair already taken by any OTHER currently-tracked
 *     wrapper (wrapper_index_add()'s own dedup, surfaced here as
 *     ESP_ERR_INVALID_ARG/errbuf at install time rather than silently
 *     failing to index at the next reindex).
 * On success the three files (.wsrc/.wbc/.json) are written atomically and
 * *id_inout holds the id. Caller (api_v1.c) MUST call
 * ble_collector_wrapper_reindex_request() afterwards -- see this section's
 * own top comment for why this file cannot do that itself. Returns ESP_OK /
 * ESP_ERR_INVALID_ARG (reason in errbuf) / ESP_ERR_NO_MEM (table full,
 * WRAPPERS_MAX reached, create only). */
int wrapper_store_upsert(uint16_t *id_inout, const char *name, const char *source,
                         const uint8_t *psbc, size_t psbc_len, bool enabled,
                         char *errbuf, size_t errlen);

/* Deletes wrapper `id` (meta+source+bytecode files, RAM slot). False if id
 * is unknown. Same reindex-request obligation as wrapper_store_upsert()'s
 * doc comment. */
bool wrapper_store_delete(uint16_t id);

/* Bumps wrapper `id`'s match_count by one -- called from wrapper_exec.c's
 * wrapper_exec_run(), the decoder task, once per actual run (controller
 * ruling: "the only way a user can tell whether a hand-written wrapper is
 * matching anything"). Survives a reindex (wrapper_store_load_all() carries
 * it over by id, see wrapper_store.c); resets only on reboot. No-op if id
 * isn't currently tracked (defensive -- a stale race against a delete that
 * landed between the match-index lookup and this call is possible in
 * principle, even though wrapper_exec_run() is only ever invoked with an id
 * the index just resolved). */
void wrapper_store_note_match(uint16_t id);

/* Sets (msg non-NULL and non-empty, truncated to this table's 48-byte
 * field) or clears (msg NULL or "") wrapper `id`'s last_error -- called
 * from wrapper_exec.c after every run attempt (cleared on a clean PSVM_OK
 * run, set to a short description otherwise, including the arena-miss/
 * validation-failure exits that return before psvm_run() is ever called).
 * Same "survives a reindex, resets on reboot, no-op if id isn't tracked"
 * contract as wrapper_store_note_match(). */
void wrapper_store_note_error(uint16_t id, const char *msg);
