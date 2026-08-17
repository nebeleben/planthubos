#include "gatt_sched.h"
#include <string.h>

/* {u32 last_ok_s, u32 last_attempt_s, u8 fails, u8 has_ok, u8 attempted,
 * u8 pad} = 12 B/device, 192 B total for GATT_SCHED_MAX_DEVICES (16) --
 * fix round 1's controller ruling on this task: the original brief's
 * single shared timestamp (u32 last_ok_s, u8 fails, u8 pad = 6 B/device,
 * 96 B total) could not serve both gatt_sched_due()'s need to anchor
 * backoff on the last ATTEMPT and gatt_sched_last_ok()'s need to report
 * the last SUCCESS -- see gatt_sched_fail()/gatt_sched_due()'s header doc
 * comments for why that conflict is real, not cosmetic.
 *
 * `attempted`/`has_ok` are fix round 2's controller ruling: `now_s == 0`
 * is a real, reachable value (an immediate NimBLE rejection needs no
 * radio round-trip, and uptime is 1-second resolution, so the first
 * second after boot is a real window, not a theoretical one) -- inferring
 * "never happened" from a zero TIMESTAMP therefore collides with a real
 * event that happens to land on t=0. Two explicit one-byte flags remove
 * that inference for both gatt_sched_due() (attempted) and
 * gatt_sched_last_ok() (has_ok) at zero extra cost: this is still the
 * natural (unpacked) layout -- two u32s (8 B, 4-B aligned) + 4 explicit
 * u8s (fails, has_ok, attempted, pad) -- already exactly 12 with no
 * rounding needed, so still no __attribute__((packed)). `pad` is the one
 * byte left over once the other three u8s are spoken for; explicit, not
 * left as compiler-inserted padding, so the layout is documented rather
 * than implied -- same discipline this task's brief asked for on the
 * handle-0 sentinel (a discipline this struct no longer leans on for
 * either timestamp). The amended spec budgeted 10 B/device; 12 is the
 * real, natural figure -- see this task's report for that reconciliation. */
typedef struct {
    uint32_t last_ok_s;
    uint32_t last_attempt_s;
    uint8_t  fails;
    uint8_t  has_ok;      /* true once gatt_sched_ok() has ever landed here */
    uint8_t  attempted;   /* true once gatt_sched_ok() or gatt_sched_fail() has ever landed here */
    uint8_t  pad;
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

/* --------------------------- read scheduler --------------------------- */

bool gatt_sched_due(int dev_idx, uint32_t interval_s, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return false;

    const gatt_sched_entry_t *e = &s_sched[dev_idx];
    if (!e->attempted) return true;   /* never attempted: due immediately */

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
    s_sched[dev_idx].has_ok = 1;
    s_sched[dev_idx].attempted = 1;
}

void gatt_sched_fail(int dev_idx, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return;
    /* last_ok_s/has_ok are deliberately untouched -- see this function's
     * header doc comment: a device failing every attempt must keep
     * reporting its true last success (or none at all) via
     * gatt_sched_last_ok(). */
    s_sched[dev_idx].last_attempt_s = now_s;
    s_sched[dev_idx].attempted = 1;
    if (s_sched[dev_idx].fails < 255) s_sched[dev_idx].fails++;
}

void gatt_sched_attempt(int dev_idx, uint32_t now_s)
{
    if (!dev_idx_valid(dev_idx)) return;
    /* Deliberately the intersection of the two above: the attempt bookkeeping
     * and backoff clearing of gatt_sched_ok(), with gatt_sched_fail()'s
     * hands-off treatment of last_ok_s/has_ok. See the header doc comment
     * for why neither of those two alone tells the truth about this case. */
    s_sched[dev_idx].last_attempt_s = now_s;
    s_sched[dev_idx].attempted = 1;
    s_sched[dev_idx].fails = 0;
}

uint8_t gatt_sched_fail_count(int dev_idx)
{
    if (!dev_idx_valid(dev_idx)) return 0;
    return s_sched[dev_idx].fails;
}

uint32_t gatt_sched_last_ok(int dev_idx)
{
    if (!dev_idx_valid(dev_idx)) return 0;
    if (!s_sched[dev_idx].has_ok) return 0;   /* explicit: never a successful read */
    return s_sched[dev_idx].last_ok_s;
}

void gatt_sched_reset(void)
{
    memset(s_sched, 0, sizeof(s_sched));
}
