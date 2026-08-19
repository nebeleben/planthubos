/* actor.c -- actor_request(), the command queue and its TTL (M5b Task 7).
 * See actor.h's top comment for the pure/impure split this file holds
 * itself to, including the fix-round-1 change: the DECISION logic
 * (actor_request_decide()/actor_service_step()) is pure and directly
 * host-tested; actor_request()/actor_service() are now thin ESP-IDF
 * wrappers around them. */
#include "actor.h"
#include "alert.h"
#include "event_log.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#endif

/* ---------------------------------------------------------------------
 * Pure queue -- no ESP-IDF anywhere below this line until the impure
 * section further down. tests/host/test_actor_queue.c links only this
 * file (plus actor_table.c/action.c, also pure) to prove it.
 * --------------------------------------------------------------------- */

void actor_queue_init(actor_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    q->last_expired_dev = -1;
    q->last_expired_action = ACTION_NONE;
    q->last_evicted_valid = false;
}

bool actor_queue_push(actor_queue_t *q, const actor_cmd_t *cmd)
{
    q->last_evicted_valid = false;

    if (q->count < ACTOR_QUEUE_MAX) {
        q->cmds[q->count] = *cmd;
        q->count++;
        return true;
    }

    /* Queue full. An ordinary command is simply refused -- only a safety
     * close is allowed to displace something already queued. */
    if (cmd->source != ACTOR_SRC_SAFETY) return false;

    /* Evict the OLDEST non-safety entry, if any, preserving FIFO order
     * among the survivors, then append the new safety command at the end
     * (it still competes on priority at pop time like any other safety
     * entry -- see actor_queue_pop()). Refusing here would strand the
     * actuator this push exists to close. */
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->cmds[i].source != ACTOR_SRC_SAFETY) {
            q->last_evicted = q->cmds[i];
            q->last_evicted_valid = true;
            for (uint8_t j = i; (uint8_t)(j + 1) < q->count; j++) q->cmds[j] = q->cmds[j + 1];
            q->cmds[q->count - 1] = *cmd;
            return true;
        }
    }

    /* Every queued entry is itself a safety command -- a genuine overload
     * with nothing left to evict. */
    return false;
}

/* Drops every entry whose deadline has passed (deadline_s < now_s),
 * compacting the survivors down in place so FIFO order among them is
 * preserved. Latches the identity of the LAST entry dropped (if any) so
 * actor_service() can name it in an alert -- see actor_queue_t's comment
 * in actor.h for why only the last one is kept rather than all of them. */
static void drop_expired(actor_queue_t *q, uint32_t now_s)
{
    uint8_t w = 0;
    for (uint8_t r = 0; r < q->count; r++) {
        if (q->cmds[r].deadline_s < now_s) {
            q->expired++;
            q->last_expired_dev = q->cmds[r].dev_idx;
            q->last_expired_action = q->cmds[r].action_id;
        } else {
            if (w != r) q->cmds[w] = q->cmds[r];
            w++;
        }
    }
    q->count = w;
}

bool actor_queue_pop(actor_queue_t *q, actor_cmd_t *out, uint32_t now_s)
{
    drop_expired(q, now_s);
    if (q->count == 0) return false;

    /* First ACTOR_SRC_SAFETY entry, if any, jumps ahead of FIFO order;
     * otherwise the oldest entry (index 0) wins. */
    uint8_t pick = 0;
    for (uint8_t i = 0; i < q->count; i++) {
        if (q->cmds[i].source == ACTOR_SRC_SAFETY) { pick = i; break; }
    }

    *out = q->cmds[pick];
    for (uint8_t i = pick; (uint8_t)(i + 1) < q->count; i++) q->cmds[i] = q->cmds[i + 1];
    q->count--;
    return true;
}

uint32_t actor_queue_expired(const actor_queue_t *q)
{
    return q->expired;
}

/* ---------------------------------------------------------------------
 * Pure decision logic (review fix round 1) -- also no ESP-IDF, also
 * linked directly into tests/host/test_actor_queue.c.
 * --------------------------------------------------------------------- */

actor_request_result_t actor_request_decide(actor_table_t *t, actor_queue_t *q,
    int dev_idx, uint8_t action_id, uint16_t param, actor_source_t source,
    uint32_t deadline_s, uint32_t now_s, bool retried)
{
    actor_request_result_t r;
    memset(&r, 0, sizeof(r));

    /* The guards run BEFORE the retry bit is looked at, and the bit is not
     * consulted here at all: a retry is checked exactly like a first
     * attempt (see actor_request_retry()). */
    r.verdict = actor_table_check(t, dev_idx, action_id, param, source, now_s);
    if (r.verdict != ACTOR_OK) return r;

    actor_cmd_t cmd;
    cmd.dev_idx = (int8_t)dev_idx;
    cmd.action_id = action_id;
    cmd.source = (uint8_t)source;
    cmd.retried = retried ? 1u : 0u;
    cmd.param = param;
    cmd.deadline_s = deadline_s;

    r.queued = actor_queue_push(q, &cmd);
    if (r.queued && q->last_evicted_valid) {
        r.evicted = true;
        r.evicted_cmd = q->last_evicted;
    }
    return r;
}

actor_service_result_t actor_service_step(actor_table_t *t, actor_queue_t *q, uint32_t now_s)
{
    actor_service_result_t r;
    memset(&r, 0, sizeof(r));

    uint32_t before = actor_queue_expired(q);
    bool got = actor_queue_pop(q, &r.cmd, now_s);
    uint32_t after = actor_queue_expired(q);
    r.ttl_dropped = after - before;
    r.ttl_last_dev = q->last_expired_dev;
    r.ttl_last_action = q->last_expired_action;

    if (!got) return r; /* dispatched stays false; cmd left zeroed */

    /* Review finding 2: re-examine the guard immediately before recording
     * or dispatching. Time has passed since this command was queued --
     * another command from another task may have consumed the rate cap,
     * or an operator may have set lockout -- so the enqueue-time OK is
     * not trusted to still hold. */
    actor_verdict_t v = actor_table_check(t, r.cmd.dev_idx, r.cmd.action_id, r.cmd.param,
                                           (actor_source_t)r.cmd.source, now_s);
    if (v != ACTOR_OK) {
        r.redecline = true;
        r.redecline_verdict = v;
        r.redecline_dev = r.cmd.dev_idx;
        r.redecline_action = r.cmd.action_id;
        r.redecline_param = r.cmd.param;
        return r; /* dispatched stays false: dropped, never sent */
    }

    actor_table_record(t, r.cmd.dev_idx, r.cmd.action_id, now_s);
    r.dispatched = true;
    return r;
}

/* Maps a guard refusal to the (level, code) pair alert_post() wants.
 * Deliberately NOT gated behind #ifdef ESP_PLATFORM (review finding 3's
 * "cheap and worth doing" list): this switch has no `default`, so a
 * future actor_verdict_t value that isn't added here trips -Werror=switch
 * on the host build too, not only in the ESP-IDF build. UNKNOWN/BOUND get
 * EVENT_LEVEL_ALERT because either one means something upstream is
 * misconfigured (a rule targeting an undeclared action, or a parameter
 * that should never have reached this door); LOCKOUT/COOLDOWN/RATE are a
 * guard doing its ordinary job and get EVENT_LEVEL_NOTIFY. */
static void verdict_alert(actor_verdict_t v, uint8_t *level_out, uint8_t *code_out)
{
    *level_out = EVENT_LEVEL_ALERT;
    *code_out  = ALERT_CODE_UNKNOWN;
    switch (v) {
    case ACTOR_REFUSED_UNKNOWN:  *code_out = ALERT_CODE_UNKNOWN;  break;
    case ACTOR_REFUSED_BOUND:    *code_out = ALERT_CODE_BOUND;    break;
    case ACTOR_REFUSED_LOCKOUT:  *level_out = EVENT_LEVEL_NOTIFY; *code_out = ALERT_CODE_LOCKOUT;  break;
    case ACTOR_REFUSED_COOLDOWN: *level_out = EVENT_LEVEL_NOTIFY; *code_out = ALERT_CODE_COOLDOWN; break;
    case ACTOR_REFUSED_RATE:     *level_out = EVENT_LEVEL_NOTIFY; *code_out = ALERT_CODE_RATE;     break;
    case ACTOR_OK: break; /* unreachable by contract: callers only invoke this on refusal */
    }
}

/* ---------------------------------------------------------------------
 * Impure wrapper -- actor_request()/actor_service(), the shared table/
 * queue instances they operate on, and the lock-taking accessors. Every
 * ESP-IDF-only line is gated so this section still compiles (though
 * nothing here is exercised) when ESP_PLATFORM is undefined, i.e. under
 * the host test's plain `cc`.
 * --------------------------------------------------------------------- */

static actor_table_t       s_table;
static actor_queue_t       s_queue;
/* One dispatcher per device kind (M6b Task 7). Was a single pointer
 * through M5b, when GATT was the only radio that could carry an action.
 * actor_request() is still the ONLY door in; this is only the exit
 * widening, and every guard in actor_table_check() still runs before a
 * command can reach any of these. Indexed by device_kind_t
 * (capability.h); DEV_KIND_COUNT is a plain #define, not a fourth
 * enumerator -- see its own comment for why. Zero-initialized by static
 * storage, so every kind starts out NULL/no-op, same posture a NULL
 * s_dispatch already had. */
static actor_dispatch_fn_t s_dispatch[DEV_KIND_COUNT];

/* Whole-branch review, ruling FINAL-persist: set whenever anything the
 * guard file records has changed, read-and-cleared by
 * actor_guards_take_dirty(). A flag rather than a write, because the
 * producers include the httpd task and flash belongs to adv_decoder_task
 * -- the same deferral shape the switch.state write and the pending-close
 * outcome already use.
 *
 * Unlike this tree's other cross-task flags, this one is set and cleared
 * INSIDE the actor mutex, not after releasing it. Those flags are
 * single-producer/single-consumer stores; this is a read-modify-write
 * (take_dirty reads then clears), so a producer landing between the two
 * would silently lose an update -- and the update it would lose is an
 * operator's lockout or a spent activation. Every setter below already
 * holds the lock at that point, so this costs nothing. (The mutex is a
 * real FreeRTOS mutex, not the portMUX spinlock alert.c must stay out of;
 * a plain store inside it is unremarkable.) 1 B of static. */
static bool s_guards_dirty;

#ifdef ESP_PLATFORM

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static inline void actor_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void actor_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

uint32_t actor_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

#else

/* On host, actor_request()/actor_service() are compiled but never called
 * (see actor.h's top comment) -- these are safe stand-ins that keep the
 * file link-clean without pulling in FreeRTOS. */
static inline void actor_lock(void)   { }
static inline void actor_unlock(void) { }
uint32_t actor_now_s(void)            { return 0; }

#endif

void actor_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
#endif
    actor_table_init(&s_table);
    actor_queue_init(&s_queue);
}

void actor_set_dispatch_hook(device_kind_t kind, actor_dispatch_fn_t fn)
{
    if ((unsigned)kind < DEV_KIND_COUNT)
        s_dispatch[kind] = fn;
}

bool actor_declare(int dev_idx, uint8_t action_id, uint16_t param_max, uint8_t flags)
{
    actor_lock();
    bool ok = actor_table_add(&s_table, dev_idx, action_id, param_max, flags);
    actor_unlock();
    return ok;
}

bool actor_configure_guards(int dev_idx, uint8_t action_id,
                             uint16_t cooldown_s, uint8_t max_per_hour)
{
    actor_lock();
    bool ok = actor_table_set_guards(&s_table, dev_idx, action_id, cooldown_s, max_per_hour);
    if (ok) s_guards_dirty = true;
    actor_unlock();
    return ok;
}

void actor_set_lockout(int dev_idx, bool on)
{
    actor_lock();
    actor_table_set_lockout(&s_table, dev_idx, on);
    /* Unconditional: actor_table_set_lockout() returns nothing, and this
     * is the operator's stop button -- an extra file write is a far better
     * error than a stop button that a power cycle un-presses. */
    s_guards_dirty = true;
    actor_unlock();
}

void actor_set_device_key(int dev_idx, const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    actor_lock();
    actor_table_set_key(&s_table, dev_idx, key);
    actor_unlock();
    /* Deliberately does NOT mark dirty. Learning a device's identity
     * changes nothing an operator configured, and the caller
     * (ble_collector.c) sets the key on the way IN to
     * actor_persist_restore_device() -- marking dirty here would schedule
     * a save of guards that have not been restored yet. */
}

bool actor_device_key(int dev_idx, uint8_t out[ACTOR_DEVICE_KEY_LEN])
{
    actor_lock();
    bool ok = actor_table_device_key(&s_table, dev_idx, out);
    actor_unlock();
    return ok;
}

int actor_find_by_key(const uint8_t key[ACTOR_DEVICE_KEY_LEN])
{
    actor_lock();
    int idx = actor_table_find_by_key(&s_table, key);
    actor_unlock();
    return idx;
}

bool actor_guards_dirty(void)
{
    actor_lock();
    bool was = s_guards_dirty;
    actor_unlock();
    return was;
}

bool actor_guards_take_dirty(void)
{
    actor_lock();
    bool was = s_guards_dirty;
    s_guards_dirty = false;
    actor_unlock();
    return was;
}

size_t actor_guards_merge(actor_guard_row_t *rows, size_t n, size_t cap)
{
    actor_lock();
    size_t out = actor_table_guard_merge(&s_table, rows, n, cap);
    actor_unlock();
    return out;
}

bool actor_guards_apply(int dev_idx, const actor_guard_row_t *row)
{
    actor_lock();
    bool ok = actor_table_guard_apply(&s_table, dev_idx, row, actor_now_s());
    actor_unlock();
    return ok;
}

bool actor_undeclare(int dev_idx)
{
    actor_lock();
    bool removed = actor_table_remove(&s_table, dev_idx);
    if (removed) s_guards_dirty = true;
    actor_unlock();
    /* Any command for this device still sitting in the queue is left where
     * it is on purpose: actor_service_step() re-runs actor_table_check()
     * before dispatching anything, so it will now be refused as
     * ACTOR_REFUSED_UNKNOWN with a named alert rather than silently
     * vanishing from the queue here. */
    return removed;
}

uint32_t actor_full_drops(void)
{
    actor_lock();
    uint32_t n = actor_table_full_drops(&s_table);
    actor_unlock();
    return n;
}

bool actor_action_flags(int dev_idx, uint8_t action_id, uint8_t *flags_out)
{
    actor_lock();
    bool ok = actor_table_action_flags(&s_table, dev_idx, action_id, flags_out);
    actor_unlock();
    return ok;
}

bool actor_pair_state(int dev_idx, uint8_t action_id, actor_pair_state_t *out)
{
    actor_lock();
    bool ok = actor_table_pair_state(&s_table, dev_idx, action_id, actor_now_s(), out);
    actor_unlock();
    return ok;
}

bool actor_lockout(int dev_idx, bool *out)
{
    actor_lock();
    bool ok = actor_table_lockout(&s_table, dev_idx, out);
    actor_unlock();
    return ok;
}

static bool request_common(int dev_idx, uint8_t action_id, uint16_t param,
                            actor_source_t source, uint32_t deadline_s, bool retried)
{
    uint32_t now_s = actor_now_s();

    actor_lock();
    actor_request_result_t r = actor_request_decide(&s_table, &s_queue, dev_idx, action_id,
                                                      param, source, deadline_s, now_s, retried);
    actor_unlock();

    if (r.verdict != ACTOR_OK) {
        uint8_t level, code;
        verdict_alert(r.verdict, &level, &code);
#ifdef ESP_PLATFORM
        alert_post(level, code, dev_idx, action_id, param);
#else
        (void)level; (void)code;
#endif
        return false;
    }

    if (r.evicted) {
#ifdef ESP_PLATFORM
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_EVICTED,
                   r.evicted_cmd.dev_idx, r.evicted_cmd.action_id, r.evicted_cmd.param);
#endif
    }

    if (!r.queued) {
        /* Only reachable when every queued entry is itself a safety
         * command (ordinary-source refusals are already covered by the
         * eviction path above) -- a genuine overload, CRITICAL. An
         * ordinary command refused by a queue full of ordinary commands
         * is the more common, less dire case and stays at ALERT. */
        uint8_t level = (source == ACTOR_SRC_SAFETY) ? EVENT_LEVEL_CRITICAL : EVENT_LEVEL_ALERT;
#ifdef ESP_PLATFORM
        alert_post(level, ALERT_CODE_QUEUE_FULL, dev_idx, action_id, param);
#else
        (void)level;
#endif
    }

    return r.queued;
}

bool actor_request(int dev_idx, uint8_t action_id, uint16_t param,
                    actor_source_t source, uint32_t deadline_s)
{
    return request_common(dev_idx, action_id, param, source, deadline_s, false);
}

bool actor_request_retry(int dev_idx, uint8_t action_id, uint16_t param,
                          actor_source_t source, uint32_t deadline_s)
{
    /* Deliberately the same body as actor_request() with one argument
     * different -- the guards, the alerts and the queue are identical, so
     * this is one door with a flag, not a second door (see actor.h's top
     * comment on why there is only ever one). */
    return request_common(dev_idx, action_id, param, source, deadline_s, true);
}

void actor_service(void)
{
    uint32_t now_s = actor_now_s();

    actor_lock();
    actor_service_result_t r = actor_service_step(&s_table, &s_queue, now_s);
    /* actor_service_step() has just charged this activation against the
     * hourly window (actor_table_record()), and that counter has to be
     * durable at the moment it is SPENT -- a crash between the fire and
     * the write refunds it, which is the reboot-loop hole ruling
     * FINAL-persist exists to close. */
    if (r.dispatched) s_guards_dirty = true;
    actor_unlock();

    if (r.ttl_dropped > 0) {
#ifdef ESP_PLATFORM
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_EXPIRED, r.ttl_last_dev, r.ttl_last_action,
                   (uint16_t)r.ttl_dropped);
#endif
    }

    if (r.redecline) {
        uint8_t level, code;
        verdict_alert(r.redecline_verdict, &level, &code);
#ifdef ESP_PLATFORM
        alert_post(level, code, r.redecline_dev, r.redecline_action, r.redecline_param);
#else
        (void)level; (void)code;
#endif
    }

    if (r.dispatched) {
        /* Resolve the dispatched command's device kind from its stable
         * identity (actor_device_key(), already lock-taking and already
         * plain C99 -- no data_core dependency needed here at all, so
         * this runs identically on host and target; the queue/table stay
         * exactly as pure as they were). key[0] is device_id_t.kind
         * (capability.h; ble_collector.c's _Static_assert pins
         * sizeof(device_id_t) == ACTOR_DEVICE_KEY_LEN, key[0] first). */
        uint8_t              key[ACTOR_DEVICE_KEY_LEN];
        actor_dispatch_fn_t  fn = NULL;
        if (actor_device_key(r.cmd.dev_idx, key) && key[0] < DEV_KIND_COUNT)
            fn = s_dispatch[key[0]];

        if (fn) {
            fn(&r.cmd);
        } else {
            /* No registered dispatcher for this kind (or the kind could
             * not be resolved at all): drop visibly, never silently -- a
             * queued command that just vanishes is exactly the failure
             * mode the TTL and re-check machinery above exists to make
             * observable, and this is that same principle applied to a
             * third way a command could otherwise disappear. */
#ifdef ESP_PLATFORM
            alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_NO_DISPATCHER,
                       r.cmd.dev_idx, r.cmd.action_id, r.cmd.param);
#endif
        }
    }
}
