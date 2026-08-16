/* rules_engine.c -- the rules task itself (spec §4 "Triggers"/"Firing
 * state"/"Actions", brief step 3): one FreeRTOS task blocking on an event
 * group, per-rule esp_timer for the optional `every` clause, and the shared
 * evaluate-one-rule path used both by the real firing loop and by
 * rules_test()'s dry run.
 *
 * Engine task stack is 8192. The original justification for that number was
 * wrong in its arithmetic and right in its conclusion, so both halves are
 * recorded here (M2 Task 4 hardware hotfix, round 4, which audited every
 * oversized allocation on the hub boot path after a boot heap trace showed
 * the device running on 2.6 KB of free heap):
 *
 *   - WRONG: "psvm_run()'s own stack frame is close to 3KB". Measured on the
 *     actual C3 build (`riscv32-esp-elf-objdump -d psvm.c.obj`, prologue
 *     `addi sp,sp,-N`), psvm_run()'s frame is 1632 B -- the compiler overlaps
 *     the block-scoped `tmp[PSVM_STACK]`/`msgbuf[PSVM_STRBUF]` locals with
 *     the function-scope `stack[32]`/`strbuf[512]` rather than stacking them.
 *
 *   - RIGHT: 8192 is still the correct size, for a reason the old comment
 *     never named. The deep path is not psvm_run() itself but what runs
 *     UNDERNEATH it: engine_task (16) -> evaluate_all (416) -> evaluate_real
 *     (368) -> psvm_run (1632) -> real_sink -> event_log_append (192) ->
 *     fopen/fwrite into LittleFS (VFS + lfs_dir_commit/lfs_dir_traverse
 *     chain, ~1.5-2 KB) -> then the SSE and MQTT hooks, which
 *     event_log_append calls before returning. All of that is live at once,
 *     because real_sink is invoked from inside psvm_run. Worst case measures
 *     out near 5 KB, so 8192 is roughly a 1.6x margin, not the 3x the old
 *     comment implied -- and cutting it to 6144 to reclaim 2 KB of heap would
 *     leave about 1 KB, which is not a margin worth having on a device whose
 *     failure mode is a stack-overflow panic.
 *
 * engine_task() logs its real high-water mark after the first evaluation
 * pass (see there) so a future round can size this from a boot log instead
 * of from a call-chain estimate. */
#include "rules_internal.h"
#include "event_log.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "rules_engine";

#define ENGINE_TASK_STACK 8192
#define ENGINE_TASK_PRIO   5
#define DEBOUNCE_MS        2000

#define VALUE_UPDATE_BIT (1u << 0)
#define PERIODIC_BIT     (1u << 1)

static EventGroupHandle_t s_events;
/* Serializes one evaluate-a-rule pass (resolve + psvm_run, which together
 * touch the shared scratch buffers below) against a second caller -- today
 * that is always just the engine task itself, but rules_test() (dry-run,
 * spec §6) shares this exact path and may end up reachable from the httpd
 * task once Task 6 wires it to POST /api/v1/rules/<id>/test. Taking this
 * for the full resolve+psvm_run duration is safe: psvm_run is bounded
 * compute (PSVM_MAX_STEPS), never I/O, so nothing that holds this mutex
 * ever blocks indefinitely while holding it -- same reasoning the rest of
 * this codebase already uses for portMAX_DELAY takes (see e.g. swarm.c's
 * record_stat()). */
static SemaphoreHandle_t s_eval_mutex;
/* Guards rules_notify_value_update() against being called before rules_init()
 * runs (brief's mandatory correction) -- app boot order means BLE/swarm
 * ingest can only start after rules_init() in practice (see main.c), but
 * this makes the function safe regardless of call order, not just lucky. */
static bool s_initialized;

/* Scratch buffers for one evaluation. `static`, not stack locals -- same
 * "big buffer off a small task stack" idiom as sampler.c's sample_once()
 * (see components/sampler/sampler.c) -- and safe for a second caller
 * precisely because every access is under s_eval_mutex (see above), unlike
 * sample_once()'s single-task assumption. */
static uint8_t        s_psbc[RULES_PSBC_MAX];
static psvm_ref_val_t s_resolved[PSVM_MAX_REFS];

/* Everything resolve_all() produces for one rule: whether its bytecode even
 * loaded/validated, and (if so) whether every ref it references is ready.
 * st->prog's section pointers point into s_psbc, so st only stays valid
 * until the next resolve_all() call (still under the same s_eval_mutex
 * hold). */
typedef struct {
    bool        loaded_ok;
    psvm_err_t  validate_err;   /* meaningful iff !loaded_ok */
    psvm_prog_t prog;           /* meaningful iff loaded_ok */
    bool        all_ready;
    char        reason[48];     /* first not-ready reason, meaningful iff !all_ready */
} resolve_state_t;

/* Loads rule_id's bytecode, validates it, and resolves every ref into
 * s_resolved. Takes an id, not a rule_t* -- the id is all it ever needed
 * (the on-disk .psbc is looked up by id), and callers now only ever have a
 * SNAPSHOT rule_t (copied by value, see evaluate_all()) rather than a live
 * pointer into g_rules[], so there is no slot to dereference here safely
 * anyway. refs_out/nrefs/refs_cap (all optional -- pass NULL/NULL/0 to
 * skip): fills one rules_test_ref_t per ref up to refs_cap, *nrefs left at
 * the actual count -- rules_test()'s ref list (spec §6). Caller must hold
 * s_eval_mutex. */
static void resolve_all(uint32_t rule_id, resolve_state_t *st,
                        rules_test_ref_t *refs_out, size_t *nrefs, size_t refs_cap)
{
    memset(st, 0, sizeof(*st));
    if (nrefs) *nrefs = 0;

    size_t len = 0;
    if (!rules_store_read_psbc(rule_id, s_psbc, sizeof(s_psbc), &len)) {
        st->validate_err = PSVM_ERR_TRUNCATED;
        strlcpy(st->reason, "bytecode unreadable", sizeof(st->reason));
        return;
    }
    psvm_err_t verr = psvm_validate(s_psbc, len, PSVM_DIALECT_RULES,
                                    RULES_CAP_MAX_ID, RULES_BUILTINS_IMPL, &st->prog);
    if (verr != PSVM_OK) {
        st->validate_err = verr;
        strlcpy(st->reason, "invalid bytecode", sizeof(st->reason));
        return;
    }
    st->loaded_ok = true;
    st->all_ready = true;

    for (uint16_t i = 0; i < st->prog.ref_count; i++) {
        char why[48];
        bool ok = rules_resolve(&st->prog, i, &s_resolved[i], why, sizeof(why));
        if (!ok && st->all_ready) {
            st->all_ready = false;
            strlcpy(st->reason, why, sizeof(st->reason));
        }
        if (refs_out && nrefs && *nrefs < refs_cap) {
            rules_test_ref_t *ro = &refs_out[*nrefs];
            psvm_ref_t pr = psvm_get_ref(&st->prog, i);
            uint16_t nlen = 0;
            const char *nm = psvm_get_str(&st->prog, pr.name_const, &nlen);
            char namebuf[40];
            if (nm) {
                if (nlen >= sizeof(namebuf)) nlen = sizeof(namebuf) - 1;
                memcpy(namebuf, nm, nlen);
                namebuf[nlen] = '\0';
            } else {
                namebuf[0] = '\0';
            }
            snprintf(ro->ref_desc, sizeof(ro->ref_desc), "%s(\"%s\").%s%s",
                     pr.kind == 0 ? "plant" : "device", namebuf, rules_cap_name(pr.capability),
                     pr.field == 1 ? ".age" : "");
            ro->value = s_resolved[i].value;
            ro->age_s = s_resolved[i].age_s;
            ro->ready = s_resolved[i].ready;
            (*nrefs)++;
        }
    }
}

/* Real firing sink: log -> event_log_append(0, ...), notify -> level 1 --
 * spec §4 "Actions" / brief step 3. builtin (0/1) IS the event level, spec
 * §2's builtins bitmap and §5's event_t.level share the same 0=log/1=notify
 * numbering by design. */
typedef struct { uint32_t rule_id; } real_sink_ctx_t;

static bool real_sink(void *ctx, uint8_t builtin, const char *msg)
{
    const real_sink_ctx_t *c = ctx;
    event_log_append(builtin, c->rule_id, msg);
    return true;
}

/* rules_test()'s capture sink: renders the action strings without executing
 * them (spec §6: "actions rendered but NOT executed, no state change"). */
typedef struct {
    rules_test_action_t *acts;
    size_t               *nacts;
    size_t                cap;
} capture_sink_ctx_t;

static bool capture_sink(void *ctx, uint8_t builtin, const char *msg)
{
    capture_sink_ctx_t *c = ctx;
    if (c->acts && c->nacts && *c->nacts < c->cap) {
        rules_test_action_t *a = &c->acts[*c->nacts];
        a->builtin = builtin;
        strlcpy(a->msg, msg, sizeof(a->msg));
        (*c->nacts)++;
    }
    return true;
}

/* Result of one real evaluation, computed entirely against a SNAPSHOT
 * rule_t (a by-value copy taken under g_rules_mutex before this runs -- see
 * evaluate_all()) rather than a live g_rules[] slot. out->fsm starts as the
 * snapshot's own fsm and is only advanced by rules_fsm_should_fire() when
 * evaluation reaches that point (i.e. exactly the cases where the pre-fix
 * code used to mutate r->fsm directly) -- evaluate_all()'s write-back phase
 * decides whether/where this outcome actually lands in g_rules[]. */
typedef struct {
    bool               ready;
    char               not_ready_reason[48];
    psvm_err_t         last_err;
    bool               fired;
    rules_fsm_state_t  fsm;
} eval_outcome_t;

/* One real evaluation of snapshot rule `snap` (brief step 3's exact path):
 * resolve -> not-ready short-circuits (status update only) ->
 * psvm_run(run_actions = false) for cond -> rules_fsm_should_fire -> if
 * firing, a SECOND psvm_run(run_actions = true) with the real sink. psvm_run
 * is stateless per call (always starts at pc 0), so re-running the
 * condition code a second time to reach the action code is the documented
 * way to do this (spec §3), not wasted work worth avoiding.
 *
 * Deliberately touches g_rules_mutex nowhere -- `snap` is a private copy
 * (review fix, critical: the pre-fix version held a live rule_t* across
 * this whole unlocked resolve+psvm_run stretch, so a concurrent
 * rules_delete()+rules_upsert() reusing that slot for a different rule
 * mid-evaluation could read a torn slot and misattribute a fire). Any
 * log/notify action DOES already happen for real inside this call (via
 * real_sink -> event_log_append(), keyed by snap->id, which stays valid and
 * correctly-attributed regardless of what happens to the slot afterward) --
 * only the g_rules[] status write-back is deferred to the caller, which can
 * still discard it if the id no longer resolves to a live slot. Caller must
 * hold s_eval_mutex. */
static void evaluate_real(const rule_t *snap, uint32_t now_uptime_s, eval_outcome_t *out)
{
    memset(out, 0, sizeof(*out));
    out->fsm = snap->fsm;

    resolve_state_t st;
    resolve_all(snap->id, &st, NULL, NULL, 0);

    if (!st.loaded_ok || !st.all_ready) {
        out->ready = false;
        strlcpy(out->not_ready_reason, st.reason, sizeof(out->not_ready_reason));
        out->last_err = st.loaded_ok ? PSVM_OK : st.validate_err;
        return;
    }

    psvm_result_t cres = psvm_run(&st.prog, s_resolved, NULL, NULL, NULL, false);
    if (cres.err != PSVM_OK) {
        out->ready = false;
        out->last_err = cres.err;
        strlcpy(out->not_ready_reason, "evaluation error", sizeof(out->not_ready_reason));
        return;
    }
    out->ready = true;
    out->last_err = PSVM_OK;

    bool fire = rules_fsm_should_fire(&out->fsm, (rules_mode_t)snap->mode, snap->cooldown_s,
                                      now_uptime_s, cres.cond);
    if (!fire) return;

    real_sink_ctx_t ctx = { .rule_id = snap->id };
    psvm_result_t ares = psvm_run(&st.prog, s_resolved, NULL, real_sink, &ctx, true);
    if (ares.err != PSVM_OK) {
        /* The fire already happened (fsm state above already advanced) --
         * an error partway through the action code (e.g. a step-budget
         * trip building a very long interpolated string) does not undo
         * that; it just means some/all of this fire's log/notify calls
         * never ran. Logged, not fatal to the engine. */
        ESP_LOGW(TAG, "rule %u (\"%s\"): action code error %d after firing",
                 (unsigned)snap->id, snap->name, (int)ares.err);
    }
    out->fired = true;
}

/* only_due=false: value-update wake -- every enabled rule gets re-evaluated
 * (M1 has no per-rule dependency tracking, spec §4: "cheap table scans, <=32
 * sensors" makes a full sweep affordable). only_due=true: periodic wake --
 * only rules whose own esp_timer actually ticked. Either way, a rule's due
 * flag is cleared once its evaluation is queued here, so a periodic rule
 * touched by a value-update sweep doesn't ALSO get a redundant periodic pass
 * moments later.
 *
 * Three phases per slot, none of which holds g_rules_mutex across the
 * resolve+psvm_run work (review fix, critical -- see evaluate_real()'s doc
 * comment): (1) snapshot the candidate slot by value under g_rules_mutex,
 * clearing `due` in the same hold; (2) evaluate the snapshot, unlocked,
 * under s_eval_mutex; (3) re-take g_rules_mutex and re-find the slot BY ID
 * (never by index i -- a concurrent rules_delete()/rules_upsert() may have
 * reused slot i for a different rule while (2) ran unlocked). A missing id
 * at write-back time discards the status write-back entirely (the rule was
 * deleted, or replaced by an upsert that reused the same slot under a NEW
 * id -- ids are never reused, so an id mismatch always means "not the same
 * rule anymore"); an already-fired action from (2) is logged but accepted,
 * since event_log_append() already ran for real and cannot be un-fired. */
static void evaluate_all(bool only_due)
{
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    for (int i = 0; i < RULES_MAX; i++) {
        xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
        rule_t *slot = &g_rules[i];
        bool candidate = slot->in_use && slot->enabled && (!only_due || slot->due);
        rule_t snap;
        if (candidate) {
            snap = *slot;
            slot->due = false;
        }
        xSemaphoreGive(g_rules_mutex);
        if (!candidate) continue;

        xSemaphoreTake(s_eval_mutex, portMAX_DELAY);
        eval_outcome_t out;
        evaluate_real(&snap, now_uptime_s, &out);
        xSemaphoreGive(s_eval_mutex);

        xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
        int idx = -1;
        for (int j = 0; j < RULES_MAX; j++) {
            if (g_rules[j].in_use && g_rules[j].id == snap.id) { idx = j; break; }
        }
        if (idx < 0) {
            xSemaphoreGive(g_rules_mutex);
            if (out.fired) {
                ESP_LOGW(TAG, "rule %u (\"%s\") fired but was deleted/replaced mid-evaluation; "
                              "discarding its status write-back", (unsigned)snap.id, snap.name);
            }
            continue;
        }
        rule_t *live = &g_rules[idx];
        live->last_eval_ts = now_uptime_s;
        live->ready = out.ready;
        strlcpy(live->not_ready_reason, out.not_ready_reason, sizeof(live->not_ready_reason));
        live->last_err = out.last_err;
        if (out.ready) {
            /* Only advance fsm/fire bookkeeping on the ready path -- same
             * as the pre-fix code, which never touched r->fsm at all on a
             * not-ready evaluation. This also protects against clobbering
             * a fresh rules_fsm_reset() from a same-id rules_upsert()/
             * rules_set_enabled() that raced this evaluation: on the
             * not-ready path, out.fsm is left equal to snap.fsm (whatever
             * it was at snapshot time) and is simply never written back. */
            live->fsm = out.fsm;
            if (out.fired) {
                live->last_fire_ts = now_uptime_s;
                live->fire_count++;
            }
        }
        xSemaphoreGive(g_rules_mutex);

        if (out.fired) {
            ESP_LOGI(TAG, "rule %u (\"%s\") fired", (unsigned)snap.id, snap.name);
        }
    }
}

/* esp_timer task callback (one per rule with every_s > 0): marks the rule
 * due and wakes the engine task. Kept minimal, per esp_timer's own
 * "callbacks should be short" contract. */
static void periodic_timer_cb(void *arg)
{
    rule_t *r = arg;
    r->due = true;
    if (s_events) xEventGroupSetBits(s_events, PERIODIC_BIT);
}

void rules_engine_sync_timer(rule_t *r)
{
    if (!r) return;
    if (r->timer) {
        esp_timer_stop(r->timer);   /* no-op if not running */
        esp_timer_delete(r->timer);
        r->timer = NULL;
    }
    r->due = false;
    if (r->every_s == 0) return;

    const esp_timer_create_args_t args = {
        .callback = periodic_timer_cb,
        .arg = r,
        .name = "rule_every",
    };
    esp_err_t err = esp_timer_create(&args, &r->timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rule %u: esp_timer_create failed (%s); 'every' trigger disabled",
                 (unsigned)r->id, esp_err_to_name(err));
        r->timer = NULL;
        return;
    }
    err = esp_timer_start_periodic(r->timer, (uint64_t)r->every_s * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rule %u: esp_timer_start_periodic failed (%s); 'every' trigger disabled",
                 (unsigned)r->id, esp_err_to_name(err));
        esp_timer_delete(r->timer);
        r->timer = NULL;
    }
}

static void engine_task(void *arg)
{
    (void)arg;
    /* See this file's header comment: ENGINE_TASK_STACK is the single
     * largest dynamic task allocation on the hub boot path, and until now
     * its size rested on an estimate (a wrong one, at that). This logs the
     * measured headroom after the first evaluation pass, and again whenever
     * a later pass sets a new low -- a fire path that reaches LittleFS and
     * the SSE/MQTT hooks is deeper than a no-op pass, so the first pass
     * alone is NOT representative here (unlike the sampler's). Cheap
     * (uxTaskGetStackHighWaterMark is a stack scan, once per pass) and
     * permanent: it is what lets a future round reclaim heap from this
     * stack with evidence, and it turns a would-be stack-overflow panic
     * into a visible warning first. */
    static UBaseType_t lowest_free = (UBaseType_t)-1;
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(s_events, VALUE_UPDATE_BIT | PERIODIC_BIT,
                                               pdTRUE, pdFALSE, portMAX_DELAY);
        if (bits & VALUE_UPDATE_BIT) {
            /* Debounce (brief step 3): collapse a burst of sensor updates
             * into one evaluation pass. Re-clear VALUE_UPDATE_BIT after the
             * delay too, draining anything that arrived during it, so that
             * burst doesn't immediately wake this loop again right after an
             * evaluation that already covered it. */
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            xEventGroupClearBits(s_events, VALUE_UPDATE_BIT);
            evaluate_all(false);
        } else if (bits & PERIODIC_BIT) {
            evaluate_all(true);
        }
        UBaseType_t free_bytes = uxTaskGetStackHighWaterMark(NULL);
        if (free_bytes < lowest_free) {
            lowest_free = free_bytes;
            ESP_LOGI(TAG, "engine task stack high-water mark: %u B unused of %u B",
                     (unsigned)free_bytes, (unsigned)ENGINE_TASK_STACK);
        }
    }
}

void rules_init(void)
{
    rules_store_load_all();

    s_events = xEventGroupCreate();
    s_eval_mutex = xSemaphoreCreateMutex();
    if (!s_events || !s_eval_mutex) {
        ESP_LOGE(TAG, "failed to create engine sync primitives; rules will not evaluate");
        return;
    }
    s_initialized = true;

    if (xTaskCreate(engine_task, "rules_engine", ENGINE_TASK_STACK, NULL, ENGINE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(rules_engine) failed; rules will not evaluate");
        s_initialized = false;
    }
}

void rules_notify_value_update(void)
{
    /* Safe before rules_init() runs (brief's mandatory correction): guarded
     * on s_initialized, set only after both s_events and the engine task
     * exist. */
    if (!s_initialized || !s_events) return;
    xEventGroupSetBits(s_events, VALUE_UPDATE_BIT);
}

int rules_test(uint32_t id, bool *ready, bool *cond, bool *would_fire,
               rules_test_ref_t *refs, size_t *nrefs,
               rules_test_action_t *acts, size_t *nacts)
{
    if (!ready || !cond || !would_fire) return ESP_ERR_INVALID_ARG;
    *ready = false;
    *cond = false;
    *would_fire = false;
    /* nrefs/nacts are in/out: caller sets them to the capacity of
     * refs[]/acts[] before the call; both are reset to the actual filled
     * count here (0 if this returns early). */
    size_t refs_cap = (refs && nrefs) ? *nrefs : 0;
    size_t acts_cap = (acts && nacts) ? *nacts : 0;
    if (nrefs) *nrefs = 0;
    if (nacts) *nacts = 0;

    if (!g_rules_mutex || !s_eval_mutex) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(g_rules_mutex, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < RULES_MAX; i++) {
        if (g_rules[i].in_use && g_rules[i].id == id) { idx = i; break; }
    }
    rule_t snapshot;
    if (idx >= 0) snapshot = g_rules[idx];
    xSemaphoreGive(g_rules_mutex);
    if (idx < 0) return ESP_ERR_NOT_FOUND;

    xSemaphoreTake(s_eval_mutex, portMAX_DELAY);
    resolve_state_t st;
    resolve_all(snapshot.id, &st, refs, nrefs, refs_cap);
    if (!st.loaded_ok || !st.all_ready) {
        xSemaphoreGive(s_eval_mutex);
        return ESP_OK;   /* *ready stays false; refs[] already shows why */
    }

    /* Test path never gates on mode/cooldown (spec §3: "the /test endpoint
     * passes run_actions=true when the condition holds"), always through the
     * capturing sink -- rendered strings only, never event_log/MQTT. */
    capture_sink_ctx_t cctx = { .acts = acts, .nacts = nacts, .cap = acts_cap };
    psvm_result_t res = psvm_run(&st.prog, s_resolved, NULL,
                                 (acts && nacts) ? capture_sink : NULL, &cctx, true);
    xSemaphoreGive(s_eval_mutex);

    if (res.err != PSVM_OK) return ESP_OK;   /* *ready stays false */

    *ready = true;
    *cond = res.cond;

    /* would_fire: same fsm decision the real engine would make, computed on
     * a throwaway COPY of the rule's fsm state so this dry run never mutates
     * the real armed/last-fire state (brief step 3: "no fsm mutation"). */
    rules_fsm_state_t tmp = snapshot.fsm;
    uint32_t now_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    *would_fire = rules_fsm_should_fire(&tmp, (rules_mode_t)snapshot.mode, snapshot.cooldown_s,
                                       now_uptime_s, res.cond);
    return ESP_OK;
}
