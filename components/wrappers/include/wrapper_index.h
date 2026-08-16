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
 *   - WMATCH_MAC_PREFIX:   the device's first 3 MAC bytes, packed
 *                          big-endian into `key`'s low 24 bits
 *                          (mac[0]<<16 | mac[1]<<8 | mac[2]).
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
 * svc_uuid/manu_id are 0xFFFFFFFF when absent. */
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
