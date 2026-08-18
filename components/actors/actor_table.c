#include "actor_table.h"
#include "action.h"
#include <stddef.h>

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

void actor_table_init(actor_table_t *t)
{
    t->full_drops = 0;
    for (int i = 0; i < ACTOR_MAX_DEVICES; i++) {
        t->devices[i].dev_idx = -1;
        t->devices[i].lockout = false;
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
        /* Same backward-clock reasoning as the cooldown check above:
         * now_s < window_start_s is treated as still the current window,
         * not as a freshly elapsed one. */
        bool same_window = slot->window_count > 0 &&
                            (now_s < slot->window_start_s ||
                             (now_s - slot->window_start_s) < ACTOR_WINDOW_S);
        uint8_t count = same_window ? slot->window_count : 0;
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
