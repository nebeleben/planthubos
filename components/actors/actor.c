/* actor.c -- actor_request(), the command queue and its TTL (M5b Task 7).
 * See actor.h's top comment for the pure/impure split this file holds
 * itself to. */
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
}

bool actor_queue_push(actor_queue_t *q, const actor_cmd_t *cmd)
{
    if (q->count >= ACTOR_QUEUE_MAX) return false;
    q->cmds[q->count] = *cmd;
    q->count++;
    return true;
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
 * Impure wrapper -- actor_request()/actor_service() and the shared table/
 * queue instances they operate on. Every ESP-IDF-only line is gated so
 * this section still compiles (though nothing here is exercised) when
 * ESP_PLATFORM is undefined, i.e. under the host test's plain `cc`.
 * --------------------------------------------------------------------- */

static actor_table_t       s_table;
static actor_queue_t       s_queue;
static actor_dispatch_fn_t s_dispatch;

#ifdef ESP_PLATFORM

static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;

static inline void actor_lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void actor_unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static uint32_t now_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

#else

/* On host, actor_request()/actor_service() are compiled but never called
 * (see actor.h's top comment) -- these are safe stand-ins that keep the
 * file link-clean without pulling in FreeRTOS. */
static inline void actor_lock(void)   { }
static inline void actor_unlock(void) { }
static uint32_t now_uptime_s(void)    { return 0; }

#endif

void actor_init(void)
{
#ifdef ESP_PLATFORM
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
#endif
    actor_table_init(&s_table);
    actor_queue_init(&s_queue);
}

actor_table_t *actor_table_get(void)
{
    return &s_table;
}

void actor_set_dispatch_hook(actor_dispatch_fn_t fn)
{
    s_dispatch = fn;
}

/* Names the guard that refused a command (spec section 4.2 / 4.6: "a guard
 * refusal is not silent"). UNKNOWN and BOUND get EVENT_LEVEL_ALERT because
 * either one means something upstream is misconfigured (a rule targeting
 * an undeclared action, or a parameter that should never have reached
 * this door at all); LOCKOUT/COOLDOWN/RATE are the guards doing their
 * ordinary job and get EVENT_LEVEL_NOTIFY. */
static void alert_refusal(actor_verdict_t v, int dev_idx, uint8_t action_id, uint16_t param)
{
#ifdef ESP_PLATFORM
    uint8_t code, level;
    switch (v) {
    case ACTOR_REFUSED_UNKNOWN:  code = ALERT_CODE_UNKNOWN;  level = EVENT_LEVEL_ALERT;  break;
    case ACTOR_REFUSED_BOUND:    code = ALERT_CODE_BOUND;    level = EVENT_LEVEL_ALERT;  break;
    case ACTOR_REFUSED_LOCKOUT:  code = ALERT_CODE_LOCKOUT;  level = EVENT_LEVEL_NOTIFY; break;
    case ACTOR_REFUSED_COOLDOWN: code = ALERT_CODE_COOLDOWN; level = EVENT_LEVEL_NOTIFY; break;
    case ACTOR_REFUSED_RATE:     code = ALERT_CODE_RATE;     level = EVENT_LEVEL_NOTIFY; break;
    default: return; /* ACTOR_OK never reaches here */
    }
    alert_post(level, code, dev_idx, action_id, param);
#else
    (void)v; (void)dev_idx; (void)action_id; (void)param;
#endif
}

bool actor_request(int dev_idx, uint8_t action_id, uint16_t param,
                    actor_source_t source, uint32_t deadline_s)
{
    uint32_t now_s = now_uptime_s();

    actor_lock();
    actor_verdict_t v = actor_table_check(&s_table, dev_idx, action_id, param, source, now_s);
    if (v != ACTOR_OK) {
        actor_unlock();
        alert_refusal(v, dev_idx, action_id, param);
        return false;
    }

    actor_cmd_t cmd;
    cmd.dev_idx = (int8_t)dev_idx;
    cmd.action_id = action_id;
    cmd.source = (uint8_t)source;
    cmd.param = param;
    cmd.deadline_s = deadline_s;
    bool queued = actor_queue_push(&s_queue, &cmd);
    actor_unlock();

    if (!queued) {
#ifdef ESP_PLATFORM
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_QUEUE_FULL, dev_idx, action_id, param);
#endif
    }
    return queued;
}

void actor_service(void)
{
    uint32_t now_s = now_uptime_s();
    actor_cmd_t cmd;
    bool got;
    uint32_t before, after;
    int8_t   edev;
    uint8_t  eaction;

    actor_lock();
    before = actor_queue_expired(&s_queue);
    got = actor_queue_pop(&s_queue, &cmd, now_s);
    after = actor_queue_expired(&s_queue);
    edev = s_queue.last_expired_dev;
    eaction = s_queue.last_expired_action;
    if (got) actor_table_record(&s_table, cmd.dev_idx, cmd.action_id, now_s);
    actor_unlock();

    if (after > before) {
#ifdef ESP_PLATFORM
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_EXPIRED, edev, eaction,
                   (uint16_t)(after - before));
#else
        (void)edev; (void)eaction;
#endif
    }

    if (got && s_dispatch) s_dispatch(&cmd);
}
