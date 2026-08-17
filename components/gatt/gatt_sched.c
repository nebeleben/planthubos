#include "gatt_sched.h"
#include <string.h>

/* {u16 uuid16, u16 handle}, 4 B, no padding (both members are 2-B aligned).
 * GATT_SCHED_MAX_DEVICES x GATT_CACHE_MAX_ENTRIES x 4 B = 16 x 4 x 4 =
 * 256 B, exactly this task's brief. */
typedef struct {
    uint16_t uuid16;
    uint16_t handle;
} gatt_cache_entry_t;

typedef struct {
    gatt_cache_entry_t e[GATT_CACHE_MAX_ENTRIES];
} gatt_cache_dev_t;

_Static_assert(sizeof(gatt_cache_entry_t) == 4,
               "gatt_cache_entry_t layout drifted from this file's documented 4 B");
_Static_assert(sizeof(gatt_cache_dev_t) == GATT_CACHE_MAX_ENTRIES * 4,
               "gatt_cache_dev_t layout drifted from this file's documented 16 B/device");

static gatt_cache_dev_t s_cache[GATT_SCHED_MAX_DEVICES];

/* {u32 last_ok_s, u32 last_attempt_s, u8 fails, u8 pad[3]} = 12 B/device,
 * 192 B total for GATT_SCHED_MAX_DEVICES (16) -- fix round 1's controller
 * ruling on this task: the original brief's single shared timestamp
 * (u32 last_ok_s, u8 fails, u8 pad = 6 B/device, 96 B total) could not
 * serve both gatt_sched_due()'s need to anchor backoff on the last
 * ATTEMPT and gatt_sched_last_ok()'s need to report the last SUCCESS --
 * see gatt_sched_fail()/gatt_sched_due()'s header doc comments for why
 * that conflict is real, not cosmetic. This is the natural (unpacked)
 * layout: two u32s (8 B, 4-B aligned) + fails (1 B) + 3 B of trailing pad
 * to keep the struct's own size a multiple of its largest member's
 * alignment (4) -- already exactly 12 with no rounding needed, so no
 * __attribute__((packed)) is required here (unlike the 6-B/device layout
 * this replaces). `pad` is explicit, not left as compiler-inserted
 * padding, so the layout is documented rather than implied -- same
 * discipline this task's brief asked for on the handle-0 sentinel. The
 * amended spec budgeted 10 B/device; 12 is the real, natural figure --
 * see this task's report for that reconciliation. */
typedef struct {
    uint32_t last_ok_s;
    uint32_t last_attempt_s;
    uint8_t  fails;
    uint8_t  pad[3];
} gatt_sched_entry_t;

_Static_assert(sizeof(gatt_sched_entry_t) == 12,
               "gatt_sched_entry_t layout drifted from this file's documented 12 B/device");
_Static_assert(sizeof(gatt_sched_entry_t) * GATT_SCHED_MAX_DEVICES == 192,
               "gatt scheduler table drifted from this file's documented 192 B total");

static gatt_sched_entry_t s_sched[GATT_SCHED_MAX_DEVICES];

static bool dev_idx_valid(int dev_idx)
{
    return dev_idx >= 0 && dev_idx < GATT_SCHED_MAX_DEVICES;
}

/* -------------------------- handle cache -------------------------- */

uint16_t gatt_cache_lookup(int dev_idx, uint16_t uuid16)
{
    if (!dev_idx_valid(dev_idx)) return 0;

    const gatt_cache_dev_t *d = &s_cache[dev_idx];
    for (int i = 0; i < GATT_CACHE_MAX_ENTRIES; i++) {
        if (d->e[i].handle != 0 && d->e[i].uuid16 == uuid16) return d->e[i].handle;
    }
    return 0;
}

void gatt_cache_store(int dev_idx, uint16_t uuid16, uint16_t handle)
{
    if (!dev_idx_valid(dev_idx)) return;

    gatt_cache_dev_t *d = &s_cache[dev_idx];
    int free_slot = -1;
    for (int i = 0; i < GATT_CACHE_MAX_ENTRIES; i++) {
        if (d->e[i].handle != 0 && d->e[i].uuid16 == uuid16) {
            d->e[i].handle = handle;   /* re-discovery: overwrite in place */
            return;
        }
        if (d->e[i].handle == 0 && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;   /* full of 4 DISTINCT uuid16s: refuse, don't evict */

    d->e[free_slot].uuid16 = uuid16;
    d->e[free_slot].handle = handle;
}

void gatt_cache_drop(int dev_idx)
{
    if (!dev_idx_valid(dev_idx)) return;
    memset(&s_cache[dev_idx], 0, sizeof(s_cache[dev_idx]));
}

void gatt_cache_reset(void)
{
    memset(s_cache, 0, sizeof(s_cache));
}

/* --------------------------- read scheduler --------------------------- */

bool gatt_sched_due(int dev_idx, uint32_t interval_s, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return false;

    const gatt_sched_entry_t *e = &s_sched[dev_idx];
    if (e->last_attempt_s == 0) return true;   /* never attempted: due immediately */

    /* 1x/2x/4x/8x for fails 0/1/2/>=3 -- capped, never shifted past the
     * cap, so this never risks undefined behaviour from shifting a u32 by
     * a width-or-greater amount even if fails has saturated at 255. */
    uint32_t scale = (e->fails >= 3) ? (uint32_t)GATT_SCHED_BACKOFF_CAP : (1u << e->fails);
    uint32_t effective_interval_s = interval_s * scale;

    /* Anchored on last_attempt_s, NOT last_ok_s -- see gatt_sched_due()'s
     * header doc comment for why anchoring on the last success instead
     * would defeat backoff entirely. */
    return (now_s - e->last_attempt_s) >= effective_interval_s;
}

void gatt_sched_ok(int dev_idx, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return;
    s_sched[dev_idx].last_ok_s = now_s;
    s_sched[dev_idx].last_attempt_s = now_s;
    s_sched[dev_idx].fails = 0;
}

void gatt_sched_fail(int dev_idx, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return;
    /* last_ok_s is deliberately untouched -- see this function's header
     * doc comment: a device failing every attempt must keep reporting
     * its true last success (or none at all) via gatt_sched_last_ok(). */
    s_sched[dev_idx].last_attempt_s = now_s;
    if (s_sched[dev_idx].fails < 255) s_sched[dev_idx].fails++;
}

uint8_t gatt_sched_fail_count(int dev_idx)
{
    if (!dev_idx_valid(dev_idx)) return 0;
    return s_sched[dev_idx].fails;
}

uint32_t gatt_sched_last_ok(int dev_idx)
{
    if (!dev_idx_valid(dev_idx)) return 0;
    return s_sched[dev_idx].last_ok_s;
}

void gatt_sched_reset(void)
{
    memset(s_sched, 0, sizeof(s_sched));
}
