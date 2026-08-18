#include "actor_table.h"
#include "action.h"
#include <stddef.h>
#include <string.h>

#define ACTOR_WINDOW_S 3600u

static actor_device_t *find_row(actor_table_t *t, int dev_idx)
{
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++)
        if (t->devices[i].dev_idx == dev_idx) return &t->devices[i];
    return NULL;
}

static actor_device_t *find_free_row(actor_table_t *t)
{
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++)
        if (t->devices[i].dev_idx == -1) return &t->devices[i];
    return NULL;
}

static actor_slot_t *find_slot(actor_device_t *row, uint8_t action_id)
{
    for (int i = 0; i < ACTOR_MAX_ACTIONS; i++)
        if (row->actions[i].action_id == action_id) return &row->actions[i];
    return NULL;
}

static actor_slot_t *find_free_slot(actor_device_t *row)
{
    return find_slot(row, ACTION_NONE);
}

/* True iff `slot`'s current hourly window (opened at window_start_s) is
 * still within ACTOR_WINDOW_S of `now_s` -- extracted (Task 11) so
 * actor_table_check()'s own rate guard and actor_table_pair_state()'s
 * `activations_this_hour` (below) share one answer to "is this count
 * stale" and can never disagree. Same backward-clock-safe reasoning as
 * the cooldown check just above its one call site: now_s < window_start_s
 * reads as still current, not elapsed. window_count == 0 (never fired) is
 * never "current" either way, since actor_table_record() always leaves it
 * >= 1 once anything has fired. */
static bool window_still_open(const actor_slot_t *slot, uint32_t now_s)
{
    return slot->window_count > 0 &&
           (now_s < slot->window_start_s || (now_s - slot->window_start_s) < ACTOR_WINDOW_S);
}

void actor_table_init(actor_table_t *t)
{
    t->full_drops = 0;
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        t->devices[i].dev_idx = -1;
        t->devices[i].lockout = false;
        memset(t->devices[i].key, 0, ACTOR_DEVICE_KEY_LEN);
        for (int j = 0; j < ACTOR_MAX_ACTIONS; j++) {
            actor_slot_t *s = &t->devices[i].actions[j];
            s->action_id = ACTION_NONE;
            s->flags = 0;
            s->param_max = 0;
            s->cooldown_s = 0;
            s->max_per_hour = 0;
            s->window_count = 0;
            s->last_fire_s = 0;
            s->window_start_s = 0;
        }
    }
}

bool actor_table_add(actor_table_t *t, int dev_idx, uint8_t action_id,
                      uint16_t param_max, uint8_t flags)
{
    /* -1 is find_free_row()'s own sentinel for an unused row, and also the
     * canonical not-found return of registry_find()/data_core_find_index()
     * -- a caller that resolved a device id and got "not found" must not
     * have that treated as a legitimate, addable device. Reject every
     * negative dev_idx, not just -1, since none is a real index. */
    if (dev_idx < 0) return false;

    const action_t *a = action_get(action_id);
    if (!a) return false;

    actor_device_t *row = find_row(t, dev_idx);
    if (!row) {
        row = find_free_row(t);
        if (!row) { t->full_drops++; return false; }
        row->dev_idx = dev_idx;
        row->lockout = false;
        memset(row->key, 0, ACTOR_DEVICE_KEY_LEN);
        for (int j = 0; j < ACTOR_MAX_ACTIONS; j++) row->actions[j].action_id = ACTION_NONE;
    }

    actor_slot_t *slot = find_slot(row, action_id);
    bool is_new_slot = (slot == NULL);
    if (!slot) slot = find_free_slot(row);
    if (!slot) { t->full_drops++; return false; }

    slot->action_id = action_id;
    slot->flags = flags;
    /* Effective bound: tightening the firmware's own hard bound (a lower
     * wrapper-declared param_max) is always allowed; a wrapper cannot
     * loosen it past what action.h already enforces. */
    slot->param_max = (param_max < a->param_max) ? param_max : a->param_max;
    if (is_new_slot) {
        /* Only a genuinely new (device, action) pair starts with a clean
         * guard and window state. Re-declaring a pair already tracked --
         * a wrapper re-parse, a re-discovery pass, an API-driven wrapper
         * update -- must not reset an operator's cooldown/rate guards or
         * erase an hourly budget already spent; row->lockout already
         * survives a re-add (it is only cleared when a NEW row is
         * claimed, above), guards must behave the same way. */
        slot->cooldown_s = 0;
        slot->max_per_hour = 0;
        slot->window_count = 0;
        slot->last_fire_s = 0;
        slot->window_start_s = 0;
    }
    return true;
}

actor_verdict_t actor_table_check(actor_table_t *t, int dev_idx, uint8_t action_id,
                                   uint16_t param, actor_source_t source, uint32_t now_s)
{
    if (dev_idx < 0) return ACTOR_REFUSED_UNKNOWN;

    actor_device_t *row = find_row(t, dev_idx);
    if (!row) return ACTOR_REFUSED_UNKNOWN;

    actor_slot_t *slot = find_slot(row, action_id);
    if (!slot) return ACTOR_REFUSED_UNKNOWN;

    /* action_param_ok() is the ONLY place the firmware's hard bound is
     * decided (action.h); this just adds the effective (possibly
     * wrapper-tightened) ceiling on top of it, rather than reimplementing
     * either check. BOUND refuses every source, ACTOR_SRC_SAFETY included:
     * it is a correctness check, not a rate limit, and a close carries no
     * parameter, so it never blocks a legitimate close. */
    if (!action_param_ok(action_id, param) || param > slot->param_max)
        return ACTOR_REFUSED_BOUND;

    /* The operator's stop button: refuses a rule, permits a manual press,
     * and permits the safety close -- a lockout that blocked the safety
     * close would strand an actuator open. */
    if (row->lockout && source == ACTOR_SRC_RULE)
        return ACTOR_REFUSED_LOCKOUT;

    /* ACTOR_SRC_SAFETY is exempt from every guard below this point: its
     * job is to close something already open, and a close-retry storm
     * (spec section 4.3/4.5) or a user-configured cooldown/rate cap on
     * switch.off must not be able to strand an actuator open -- the same
     * reasoning that exempts it from lockout above. Module contract:
     *
     *   unknown  -> refuses every source
     *   bound    -> refuses every source, SAFETY included
     *   lockout  -> refuses RULE; permits MANUAL and SAFETY
     *   cooldown -> refuses RULE and MANUAL; permits SAFETY
     *   rate     -> refuses RULE and MANUAL; permits SAFETY */
    if (source == ACTOR_SRC_SAFETY) return ACTOR_OK;

    /* window_count > 0 doubles as "this pair has fired at least once":
     * actor_table_record() always leaves it >= 1 after it runs, whether it
     * continues the current window or opens a new one, so a pair that has
     * never fired (window_count still 0, from actor_table_init() or a
     * fresh actor_table_add()) cannot be refused on a stale last_fire_s
     * of 0.
     *
     * now_s < last_fire_s (the clock has gone backward) is treated as
     * still inside the cooldown rather than as elapsed: plain unsigned
     * subtraction would otherwise wrap to a huge value and open the gate
     * early -- the opposite of what a safety guard should do under a bad
     * clock. */
    if (slot->cooldown_s > 0 && slot->window_count > 0 &&
        (now_s < slot->last_fire_s || (now_s - slot->last_fire_s) < slot->cooldown_s))
        return ACTOR_REFUSED_COOLDOWN;

    if (slot->max_per_hour > 0) {
        uint8_t count = window_still_open(slot, now_s) ? slot->window_count : 0;
        if (count >= slot->max_per_hour) return ACTOR_REFUSED_RATE;
    }

    return ACTOR_OK;
}

void actor_table_record(actor_table_t *t, int dev_idx, uint8_t action_id, uint32_t now_s)
{
    if (dev_idx < 0) return;

    actor_device_t *row = find_row(t, dev_idx);
    if (!row) return;
    actor_slot_t *slot = find_slot(row, action_id);
    if (!slot) return;

    slot->last_fire_s = now_s;
    /* Fixed window: a new window opens ACTOR_WINDOW_S after the window
     * START, not after the last fire (spec section 4.2) -- see this
     * file's header comment on why this is fixed, not sliding. A clock
     * that has gone backward (now_s < window_start_s) must not look like
     * an elapsed window either -- the same reasoning actor_table_check()
     * applies when reading this state, kept consistent here so the two
     * never disagree about which window a fire landed in. */
    bool window_elapsed = now_s >= slot->window_start_s &&
                           (now_s - slot->window_start_s) >= ACTOR_WINDOW_S;
    if (slot->window_count == 0 || window_elapsed) {
        slot->window_start_s = now_s;
        slot->window_count = 1;
    } else if (slot->window_count < UINT8_MAX) {
        slot->window_count++;
    }
}

bool actor_table_set_guards(actor_table_t *t, int dev_idx, uint8_t action_id,
                             uint16_t cooldown_s, uint8_t max_per_hour)
{
    if (dev_idx < 0) return false;

    actor_device_t *row = find_row(t, dev_idx);
    if (!row) return false;
    actor_slot_t *slot = find_slot(row, action_id);
    if (!slot) return false;

    slot->cooldown_s = cooldown_s;
    slot->max_per_hour = max_per_hour;
    return true;
}

void actor_table_set_lockout(actor_table_t *t, int dev_idx, bool on)
{
    if (dev_idx < 0) return;

    actor_device_t *row = find_row(t, dev_idx);
    if (row) row->lockout = on;
}

bool actor_table_remove(actor_table_t *t, int dev_idx)
{
    if (dev_idx < 0) return false;

    actor_device_t *row = find_row(t, dev_idx);
    if (!row) return false;

    /* Every field reset to exactly what actor_table_init() leaves, not just
     * dev_idx: a freed row is reused by the next actor_table_add(), and a
     * surviving window_count or last_fire_s would silently charge a
     * DIFFERENT device's first command against the removed one's spent
     * budget. full_drops is cumulative since the table was populated and is
     * deliberately not touched. */
    row->dev_idx = -1;
    row->lockout = false;
    memset(row->key, 0, ACTOR_DEVICE_KEY_LEN);
    for (int j = 0; j < ACTOR_MAX_ACTIONS; j++) {
        actor_slot_t *s = &row->actions[j];
        s->action_id = ACTION_NONE;
        s->flags = 0;
        s->param_max = 0;
        s->cooldown_s = 0;
        s->max_per_hour = 0;
        s->window_count = 0;
        s->last_fire_s = 0;
        s->window_start_s = 0;
    }
    return true;
}

uint32_t actor_table_full_drops(const actor_table_t *t)
{
    return t->full_drops;
}

bool actor_table_action_flags(const actor_table_t *t, int dev_idx, uint8_t action_id,
                               uint8_t *flags_out)
{
    if (dev_idx < 0) return false;

    /* A genuinely const scan (find_row()/find_slot() above take a mutable
     * pointer, for callers that go on to write through it) -- this
     * accessor never writes, so it does not borrow them. */
    const actor_device_t *row = NULL;
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        if (t->devices[i].dev_idx == dev_idx) { row = &t->devices[i]; break; }
    }
    if (!row) return false;

    for (int i = 0; i < ACTOR_MAX_ACTIONS; i++) {
        if (row->actions[i].action_id == action_id) {
            if (flags_out) *flags_out = row->actions[i].flags;
            return true;
        }
    }
    return false;
}

/* Const scan, shared by actor_table_pair_state()/actor_table_lockout()
 * below -- same reasoning as actor_table_action_flags()'s own comment. */
static const actor_device_t *find_row_const(const actor_table_t *t, int dev_idx)
{
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++)
        if (t->devices[i].dev_idx == dev_idx) return &t->devices[i];
    return NULL;
}

bool actor_table_pair_state(const actor_table_t *t, int dev_idx, uint8_t action_id,
                             uint32_t now_s, actor_pair_state_t *out)
{
    if (dev_idx < 0) return false;

    const actor_device_t *row = find_row_const(t, dev_idx);
    if (!row) return false;

    const actor_slot_t *slot = NULL;
    for (int i = 0; i < ACTOR_MAX_ACTIONS; i++) {
        if (row->actions[i].action_id == action_id) { slot = &row->actions[i]; break; }
    }
    if (!slot) return false;

    out->param_max = slot->param_max;
    out->cooldown_s = slot->cooldown_s;
    out->max_per_hour = slot->max_per_hour;
    out->has_fired = slot->window_count > 0;
    out->last_fire_s = slot->last_fire_s;

    bool win_open = window_still_open(slot, now_s);
    out->activations_this_hour = win_open ? slot->window_count : 0;

    /* Mirrors actor_table_check()'s own MANUAL-source tail exactly (see
     * this struct's doc comment in actor_table.h for why BOUND and LOCKOUT
     * can never appear here). Order matches actor_table_check(): cooldown
     * before rate. */
    if (slot->cooldown_s > 0 && slot->window_count > 0 &&
        (now_s < slot->last_fire_s || (now_s - slot->last_fire_s) < slot->cooldown_s)) {
        out->live_verdict = ACTOR_REFUSED_COOLDOWN;
    } else if (slot->max_per_hour > 0 && win_open && slot->window_count >= slot->max_per_hour) {
        out->live_verdict = ACTOR_REFUSED_RATE;
    } else {
        out->live_verdict = ACTOR_OK;
    }
    return true;
}

bool actor_table_lockout(const actor_table_t *t, int dev_idx, bool *out)
{
    if (dev_idx < 0) return false;

    const actor_device_t *row = find_row_const(t, dev_idx);
    if (!row) return false;

    if (out) *out = row->lockout;
    return true;
}

/* ---------------------------------------------------------------------
 * Whole-branch review, ruling FINAL-persist: the pure half of guard
 * persistence. actor_persist.c owns the bytes and the file; everything
 * that DECIDES anything lives here, where a host test runs it directly.
 * --------------------------------------------------------------------- */

/* All-zero is the "no key" sentinel (actor_table.h): it is not a real
 * device_id_t, and a row carrying it can never be matched back to a device
 * after a reboot, so it must never be persisted. */
static bool key_is_set(const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    for (int i = 0; i < ACTOR_DEVICE_KEY_LEN; i++)
        if (key[i] != 0) return true;
    return false;
}

static bool key_equal(const uint8_t a[ACTOR_DEVICE_KEY_LEN],
                       const uint8_t b[ACTOR_DEVICE_KEY_LEN])
{
    return memcmp(a, b, ACTOR_DEVICE_KEY_LEN) == 0;
}

/* The declared device carrying `key`, or NULL. Only rows with a key set
 * can match, so the sentinel can never alias a real device. */
static const actor_device_t *find_row_by_key(const actor_table_t *t,
                                              const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        const actor_device_t *row = &t->devices[i];
        if (row->dev_idx < 0 || !key_is_set(row->key)) continue;
        if (key_equal(row->key, key)) return row;
    }
    return NULL;
}

static bool row_declares_action(const actor_device_t *row, uint8_t action_id)
{
    for (int i = 0; i < ACTOR_MAX_ACTIONS; i++)
        if (row->actions[i].action_id == action_id) return true;
    return false;
}

void actor_table_set_key(actor_table_t *t, int dev_idx,
                          const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    if (dev_idx < 0 || key == NULL) return;
    actor_device_t *row = find_row(t, dev_idx);
    if (row) memcpy(row->key, key, ACTOR_DEVICE_KEY_LEN);
}

size_t actor_table_guard_merge(const actor_table_t *t, actor_guard_row_t *rows,
                                size_t n, size_t cap)
{
    if (rows == NULL || cap == 0) return 0;
    if (n > cap) n = cap;

    /* Rule 1: drop rows belonging to a device that IS declared right now
     * but no longer declares that action. Compacted in place, order of the
     * survivors preserved (nothing depends on it, but a stable file is
     * easier to diff by hand when something goes wrong on a rig). */
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        const actor_device_t *live = find_row_by_key(t, rows[r].key);
        if (live != NULL && !row_declares_action(live, rows[r].action_id)) continue;
        if (w != r) rows[w] = rows[r];
        w++;
    }
    n = w;

    /* Rule 2: write every live pair in. */
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        const actor_device_t *dev = &t->devices[i];
        if (dev->dev_idx < 0 || !key_is_set(dev->key)) continue;

        for (int j = 0; j < ACTOR_MAX_ACTIONS; j++) {
            const actor_slot_t *slot = &dev->actions[j];
            if (slot->action_id == ACTION_NONE) continue;

            size_t at = n;   /* n == "append" until proven otherwise */
            for (size_t r = 0; r < n; r++) {
                if (rows[r].action_id == slot->action_id && key_equal(rows[r].key, dev->key)) {
                    at = r;
                    break;
                }
            }
            if (at == n && n >= cap) {
                /* Full: evict the first row that belongs to no currently
                 * declared device. One provably exists -- cap is
                 * ACTOR_GUARD_ROWS_MAX, the exact number of pairs that can
                 * be declared at once, so a full table with a live pair
                 * still unwritten must contain at least one stale row. */
                for (size_t r = 0; r < n; r++) {
                    if (find_row_by_key(t, rows[r].key) == NULL) { at = r; break; }
                }
                if (at == n) continue;   /* unreachable by the argument above */
            }

            memcpy(rows[at].key, dev->key, ACTOR_DEVICE_KEY_LEN);
            rows[at].action_id = slot->action_id;
            rows[at].lockout = dev->lockout;
            rows[at].cooldown_s = slot->cooldown_s;
            rows[at].max_per_hour = slot->max_per_hour;
            rows[at].window_count = slot->window_count;
            rows[at].window_start_s = slot->window_start_s;
            rows[at].last_fire_s = slot->last_fire_s;
            if (at == n) n++;
        }
    }

    /* Rule 3 needs no code: anything not touched above is still where it
     * was, which is the whole point. */
    return n;
}

bool actor_table_guard_apply(actor_table_t *t, int dev_idx,
                              const actor_guard_row_t *row, uint32_t now_s)
{
    if (dev_idx < 0 || row == NULL) return false;

    actor_device_t *dev = find_row(t, dev_idx);
    if (!dev) return false;
    actor_slot_t *slot = find_slot(dev, row->action_id);
    if (!slot) return false;

    slot->cooldown_s = row->cooldown_s;
    slot->max_per_hour = row->max_per_hour;
    slot->window_count = row->window_count;
    /* See actor_table_guard_apply()'s doc comment for why the file's own
     * timestamps are re-based rather than believed. window_count == 0 is
     * "never fired", and both the cooldown check and window_still_open()
     * already treat it that way, so leaving the clocks at 0 there keeps
     * the restored slot byte-identical to a freshly declared one. */
    if (row->window_count > 0) {
        slot->window_start_s = now_s;
        slot->last_fire_s = now_s;
    } else {
        slot->window_start_s = 0;
        slot->last_fire_s = 0;
    }
    dev->lockout = row->lockout;
    return true;
}
