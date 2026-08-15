/* Component-private interface shared by rules_store.c, rules_resolver.c and
 * rules_engine.c. Deliberately not in include/ -- nothing outside
 * components/rules should ever touch g_rules directly or call
 * rules_resolve()/rules_engine_sync_timer(); every external caller goes
 * through rules.h's public surface instead. Mirrors ble_collector's
 * ble_collector_internal.h (quoted include, resolved relative to this
 * directory, no INCLUDE_DIRS entry needed). */
#pragma once
#include "rules.h"
#include "rules_fsm.h"
#include "psvm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_err.h"

#define RULES_STORAGE_DIR "/storage/rules"

/* PSBC validation limits this firmware implements, spec §2: capability ids
 * 0..4 (battery.level is the highest M1 knows), builtins bitmap bit0 log /
 * bit1 notify. Shared by rules_store.c (rules_upsert()'s psvm_validate()
 * call at upload) and rules_engine.c (re-validating a rule's .psbc after
 * loading it back off LittleFS, since a psvm_prog_t's section pointers must
 * point into whichever buffer just read the blob -- they can't be cached
 * across calls). */
#define RULES_CAP_MAX_ID    4
#define RULES_BUILTINS_IMPL 0x3u

/* One in-RAM rule entry. Deliberately does NOT hold the source text or
 * bytecode blob -- at RULES_MAX (32) rules x (RULES_SRC_MAX 4096 +
 * RULES_PSBC_MAX 2048) that would be ~196KB of static RAM, more than this
 * class of chip can spare. Source/bytecode live only on LittleFS (raw
 * fopen()/fread() in rules_store.c, plus rules_store_read_psbc() below) and
 * are read into a scratch buffer on the paths that need them
 * (rules_get_source(), one evaluation's psvm_validate()). What stays
 * resident is metadata (a few
 * hundred bytes total across all 32 rules) plus the tiny RAM-only firing
 * state the spec's §4 "Firing state" bullet calls out as reset-on-reboot by
 * design. */
typedef struct {
    bool               in_use;
    uint32_t           id;
    char               name[RULES_NAME_MAX + 1];
    bool               enabled;
    uint8_t            mode;         /* rules_mode_t */
    uint32_t           cooldown_s;
    uint32_t           every_s;
    size_t             source_len;   /* cached so rules_list()/get_source() need no stat() */
    size_t             psbc_len;

    /* RAM-only firing state (spec §4): lost on reboot by design -- an
     * already-true condition fires once more after a reboot, documented as
     * acceptable for M1. */
    rules_fsm_state_t  fsm;
    bool               ready;
    char               not_ready_reason[48];
    psvm_err_t         last_err;
    uint32_t           last_eval_ts;  /* uptime seconds, see rules.h */
    uint32_t           last_fire_ts;  /* uptime seconds, see rules.h */
    uint32_t           fire_count;

    esp_timer_handle_t timer;         /* NULL unless every_s > 0 and creation succeeded */
    /* Set by this rule's own esp_timer callback (esp_timer task), cleared by
     * the engine task after evaluating it. Deliberately NOT under
     * g_rules_mutex -- esp_timer callbacks must stay short, and this is a
     * single bool with exactly one setter (the timer callback) and one
     * clearer (the engine task), so a torn read/write can at worst delay
     * noticing a periodic tick by one more pass through the event-group
     * wait, never lose or duplicate one (the timer is periodic: it fires
     * again). `volatile` only, same reasoning as any other single-writer
     * cross-task flag in this codebase. */
    volatile bool       due;
} rule_t;

/* Shared table. g_rules_mutex guards every field of every entry (RAM only,
 * never held across file I/O) -- same two-phase discipline plants.c documents
 * at its own s_mutex/s_persist_mutex split: callers mutate/read g_rules under
 * this mutex only, then release it before touching the filesystem. */
extern rule_t            g_rules[RULES_MAX];
extern SemaphoreHandle_t g_rules_mutex;
extern uint32_t          g_rules_next_id;   /* next id to allocate on create */

/* ---------------- rules_store.c internals ---------------- */

/* mkdir RULES_STORAGE_DIR if missing, scan for <id>.json meta files, load
 * each into g_rules (skipping/logging any rule whose files don't check out),
 * compute g_rules_next_id = (max existing id) + 1. Called once from
 * rules_init(), before the engine task starts. */
void rules_store_load_all(void);

/* Reads exactly one rule's bytecode into buf (capacity buflen); *len_out is
 * set to the blob's length on success. False on any I/O/size problem. Used
 * by the engine's evaluation path and by rules_test(). */
bool rules_store_read_psbc(uint32_t id, uint8_t *buf, size_t buflen, size_t *len_out);

/* ---------------- rules_engine.c internals ---------------- */

/* (Re)creates or destroys r->timer to match r->every_s (0 => no timer,
 * clamped to spec §1's [30s, 24h] range by the caller before this runs).
 * Called by rules_store.c's rules_upsert()/rules_delete() while holding
 * g_rules_mutex -- esp_timer_create()/_delete() do their own internal
 * locking and never block on I/O, so this is safe to call under the RAM
 * mutex, unlike file writes. */
void rules_engine_sync_timer(rule_t *r);

/* ---------------- rules_resolver.c ---------------- */

/* Resolves one ref of prog (index ref_idx < prog->ref_count) against the
 * live plants table + registry snapshot. On failure, *out is {0, 0, false}
 * and why (if non-NULL) gets a short human reason, spec §4 resolver bullet /
 * task-5 brief step 2. Safe to call from any task (only reads plants/data_core
 * snapshots + app_config lookups, all already internally synchronized) but
 * not from an ISR (plants_snapshot()/data_core_snapshot() both take mutexes). */
bool rules_resolve(const psvm_prog_t *prog, uint16_t ref_idx, psvm_ref_val_t *out,
                   char *why, size_t whylen);

/* Capability id -> dotted name ("soil.moisture", ...), "?" if out of range.
 * Shared between the resolver's not-ready reasons and rules_test()'s
 * ref_desc rendering. */
const char *rules_cap_name(uint8_t capability_id);
