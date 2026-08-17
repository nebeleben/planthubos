#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "plants_table.h"

/* On-disk byte layout of the plants.bin blob (and the legacy M8-era NVS
 * blob -- see plants.c's top-of-file comment for the full "why LittleFS,
 * not NVS" story). PURE, no ESP includes, host compilable exactly like
 * plants_table.c/.h and plants_migrate.c/.h -- plants.c owns the actual
 * file/NVS I/O; this owns the byte shape and the format-version migration,
 * so both can be exercised from tests/host/test_plants_migrate.c without
 * dragging in ESP-IDF.
 *
 * Deliberately NOT a raw plants_table_t dump: that struct's `bool` fields
 * and any compiler-inserted padding are not a stable on-disk shape, so
 * plant_entry_blob_t pins every field to an explicit width/order instead
 * (packed, byte by byte).
 *
 * Format 3 (M5b Task 2): CAPABILITY_COUNT grew 8->9 (switch.state, id 8)
 * -- widening cap_bound[]/cap_dev[] -- and each plant gained
 * PLANT_ACTION_SLOTS action bindings (act_bound[]/act_id[]/act_dev[],
 * action.h). Unlike format 1->2 (M2 Task 4, a clean start: that hub had no
 * configured plants yet), a format-2 blob here is MIGRATED, not discarded
 * -- plants_blob_migrate_v2() below copies the eight existing capability
 * bindings into the new nine-slot array, leaves switch.state unbound, and
 * sets both action slots to ACTION_NONE. See task-2-report.md. */
#define PLANTS_BLOB_FORMAT 3

typedef struct __attribute__((packed)) {
    uint8_t id;
    uint8_t in_use;      /* bool, packed as u8 */
    uint8_t mac[6];
    uint8_t mac_valid;   /* bool, packed as u8 */
    char    name[PLANT_NAME_LEN + 1];
    uint8_t     cap_bound[CAPABILITY_COUNT];   /* bool, packed as u8, indexed by cap_id */
    device_id_t cap_dev[CAPABILITY_COUNT];     /* device_id_t is all-uint8_t: safe to embed directly */
    uint8_t     act_bound[PLANT_ACTION_SLOTS]; /* bool, packed as u8, indexed by slot */
    uint8_t     act_id[PLANT_ACTION_SLOTS];    /* ACTION_NONE when unbound */
    device_id_t act_dev[PLANT_ACTION_SLOTS];
} plant_entry_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t            format;
    uint8_t            next_id;
    plant_entry_blob_t p[PLANTS_MAX];
} plants_blob_t;

/* Packs a live plants_table_t into the current-format on-disk mirror shape,
 * and the reverse. Shared by plants.c's persist_table()/load_file(). */
void plants_blob_pack(const plants_table_t *in, plants_blob_t *out);
void plants_blob_unpack(const plants_blob_t *blob, plants_table_t *out);

/* ---- Format 2 (M2 Task 4 .. M5a): 8 capabilities, no action slots ---- */
#define PLANTS_BLOB_FORMAT_V2 2

typedef struct __attribute__((packed)) {
    uint8_t     id;
    uint8_t     in_use;
    uint8_t     mac[6];
    uint8_t     mac_valid;
    char        name[PLANT_NAME_LEN + 1];
    uint8_t     cap_bound[8];   /* CAPABILITY_COUNT was 8 in format 2 */
    device_id_t cap_dev[8];
} plant_entry_blob_v2_t;

typedef struct __attribute__((packed)) {
    uint8_t               format;
    uint8_t               next_id;
    plant_entry_blob_v2_t p[PLANTS_MAX];
} plants_blob_v2_t;

/* Migrates one format-2 blob into a live table: copies id/name/mac and the
 * eight existing capability bindings verbatim. The new switch.state
 * capability (id 8) and both action slots come back unbound/ACTION_NONE --
 * a real user's plants and their device bindings survive the upgrade
 * instead of M2's clean-start route (see PLANTS_BLOB_FORMAT's comment
 * above for why this format bump differs). `out` need not be pre-zeroed:
 * every field this writes is set explicitly. */
void plants_blob_migrate_v2(const plants_blob_v2_t *in, plants_table_t *out);

/* Interprets `len` raw bytes as either a current-format blob or a
 * migratable format-2 blob, dispatching on length first (so a format byte
 * that doesn't match what its length implies is still rejected) and then
 * on the format byte itself. Returns true and fills *out on success; false
 * for any other length or an unrecognised format byte, leaving *out
 * untouched -- callers (plants.c's load_file()/load_legacy_nvs_blob())
 * reset *out to plants_table_init() defaults first, same contract as
 * before this function existed. */
bool plants_blob_load(const uint8_t *bytes, size_t len, plants_table_t *out);
