/* zb_cmd.c -- the Zigbee command engine (M6b spec section 3, Task 8).
 *
 * DEV_KIND_ZIGBEE's actor_dispatch_fn_t (actor.h, M6b Task 7): resolves a
 * dispatched actor_cmd_t to its stored device and sends the ZCL On/Off
 * command to its short address and endpoint. Mirrors the completion
 * contract the GATT engine already satisfies (ble_collector.c's
 * on_gatt_cmd_done()/gatt_engine_set_cmd_done_hook()): a command that was
 * SENT but never CONFIRMED by the device counts as NOT confirmed -- M5b's
 * rule for GATT (spec section 4.4), and pending_close's replay machinery
 * depends on it being honest wherever it applies. Only ACT_SWITCH_ON and
 * ACT_SWITCH_OFF have ACTION_PARAM_NONE (action.h) -- neither ever passes
 * pending_close_needed()'s ACTION_PARAM_DURATION_S test -- so this engine
 * never arms a pending-close obligation and never calls into
 * pending_close.c at all.
 *
 * Unlike the GATT engine, dispatch here is NOT pinned to one in-flight
 * command: ble_collector.c's decoder loop calls actor_service() whenever
 * the BLE/battery radio is free, with no knowledge of Zigbee at all, so a
 * queue holding several Zigbee commands can have them dispatched a tick
 * apart while an earlier one is still waiting on its Default Response. A
 * single in-flight slot (the GATT shape) would silently lose the earlier
 * command's confirmation in that case -- exactly the disappearance this
 * file's safety rule forbids -- so each outstanding command is tracked by
 * its own ZCL transaction sequence number (tsn) instead. See s_inflight.
 *
 * Cross-task. actor_service() -- and so on_zb_dispatch() below -- runs on
 * whichever task calls it (today, ble_collector.c's adv_decoder_task),
 * which makes it an "outside" caller of the Zigbee stack exactly like
 * zigbee_permit_join()/zigbee_device_remove() (zigbee.c): every SDK call
 * in on_zb_dispatch() holds esp_zb_lock_acquire()/esp_zb_lock_release()
 * for that reason. The Default Response and the timeout alarm, in
 * contrast, both run ON the Zigbee stack task (zb_core_action_handler and
 * an esp_zb_scheduler_alarm() callback are stack-task-only, per zigbee.c's
 * own comments on zb_core_action_handler()/s_iv), so they call the SDK
 * directly with no lock, the same as zigbee.c's own stack-task callbacks
 * do. s_inflight itself, though, is genuinely written from both sides and
 * is guarded by its own spinlock, s_inflight_mux -- not zigbee.c's
 * s_store_mutex, which protects a different table entirely.
 */
#include "zigbee.h"

#if CONFIG_PLANTHUB_ZB_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee_core.h"
#include "esp_log.h"
#include "actor.h"
#include "action.h"
#include "alert.h"
#include "capability.h"

static const char *TAG = "zb_cmd";

/* Same reasoning, same value, as zigbee.c's ZB_LOCK_WAIT_MS: generous over
 * a lock the stack task only ever holds for one scheduler iteration,
 * bounded so a wedged stack cannot hang actor_service()'s caller forever.
 * Kept as its own private constant rather than shared via a header -- both
 * are small, independently-obvious values, not a shared policy that must
 * never drift between the two files. */
#define ZB_CMD_LOCK_WAIT_MS 200

/* Bounded to ACTOR_QUEUE_MAX (actor.h): the actor queue can never hold
 * more than that many commands at once, so this can never need to track
 * more outstanding Default Responses than that either. */
#define ZB_CMD_MAX_INFLIGHT 4

/* Generous over a healthy mesh's round trip for a one-hop ZCL command; a
 * device that never answers (powered off, out of range, gone deaf) must
 * still surface as a failure rather than hang its slot forever. */
#define ZB_CMD_TIMEOUT_MS 5000

typedef struct {
    bool        used;
    uint8_t     tsn;
    actor_cmd_t cmd;
} zb_cmd_inflight_t;

/* Written by on_zb_dispatch() (whichever task calls actor_service() --
 * see this file's top comment) and by zb_cmd_on_default_resp()/
 * zb_cmd_timeout_cb() (the Zigbee stack task) -- genuinely cross-task,
 * unlike zigbee.c's own s_iv (stack-task-only by construction). A
 * portMUX_TYPE spinlock, not a SemaphoreHandle_t: every access here is a
 * short, bounded, allocation-free scan or copy and never anything that
 * could block -- the exact case zigbee.c's own s_mux doc comment argues
 * for a spinlock over a real mutex. */
static portMUX_TYPE s_inflight_mux = portMUX_INITIALIZER_UNLOCKED;
static zb_cmd_inflight_t s_inflight[ZB_CMD_MAX_INFLIGHT];

/* Reports how a dispatched command ended -- the completion contract
 * on_gatt_cmd_done() (ble_collector.c) already satisfies for GATT, applied
 * here. A FAILED command alerts, with the same code on_gatt_cmd_done()
 * uses for "dispatched, but never reached the actuator"
 * (ALERT_CODE_COMMAND_FAILED, alert.h) -- covers an unimplemented action
 * id (requirement 1), a device this engine cannot resolve or command
 * (requirement 2), and a send that was never confirmed, all alike: never
 * a silent drop. alert_post() is documented safe from any task (alert.h),
 * and the Zigbee stack task is already allowed to do far heavier things
 * directly than this (zigbee.c's zb_iv_handle_store() writes to flash from
 * that very task), so -- unlike ble_collector.c's deferred s_pc_defer_*
 * latch -- this needs no cross-task hand-off at all; every caller below,
 * on either task, may call this directly.
 *
 * There is no separate "ok but unconfirmed" outcome here the way GATT has
 * one (a write that lands but whose read-back does not match): a ZCL
 * Default Response's status *is* the only confirmation source this engine
 * has, so `ok` below already means "confirmed" -- there is nothing weaker
 * to distinguish it from. */
static void zb_cmd_report(int8_t dev_idx, uint8_t action_id, uint16_t param,
                           bool ok, const char *reason)
{
    if (!ok) {
        ESP_LOGW(TAG, "command failed: dev=%d action=%u param=%u (%s)", (int)dev_idx,
                 (unsigned)action_id, (unsigned)param, reason ? reason : "unknown");
        alert_post(EVENT_LEVEL_ALERT, ALERT_CODE_COMMAND_FAILED, dev_idx, action_id, param);
        return;
    }
    ESP_LOGI(TAG, "command confirmed: dev=%d action=%u param=%u", (int)dev_idx,
             (unsigned)action_id, (unsigned)param);
}

/* esp_zb_scheduler_alarm() callback (esp_zb_callback_t: void(uint8_t)) --
 * runs on the Zigbee stack task. `slot_idx` is the s_inflight index handed
 * to esp_zb_scheduler_alarm() at dispatch, not a generation counter: at
 * most one alarm is ever outstanding per slot (one dispatch schedules
 * exactly one alarm; a confirmed response cancels it -- see
 * zb_cmd_on_default_resp()), so there is nothing here for a generation to
 * disambiguate. */
static void zb_cmd_timeout_cb(uint8_t slot_idx)
{
    bool was_used = false;
    actor_cmd_t cmd = { 0 };

    portENTER_CRITICAL(&s_inflight_mux);
    if (slot_idx < ZB_CMD_MAX_INFLIGHT && s_inflight[slot_idx].used) {
        was_used = true;
        cmd = s_inflight[slot_idx].cmd;
        s_inflight[slot_idx].used = false;
    }
    portEXIT_CRITICAL(&s_inflight_mux);

    /* Not used: the Default Response already arrived and cleared this slot
     * before the timeout fired. esp_zb_scheduler_alarm_cancel() in
     * zb_cmd_on_default_resp() usually pre-empts this alarm entirely, but
     * the cancellation racing the alarm's own firing is not guaranteed
     * instant -- a benign, expected no-op here, not a bug. */
    if (!was_used) return;

    zb_cmd_report(cmd.dev_idx, cmd.action_id, cmd.param, false,
                  "no Default Response within the timeout");
}

void zb_cmd_on_default_resp(uint8_t tsn, uint16_t cluster, uint8_t resp_to_cmd,
                             uint8_t status_code)
{
    (void)resp_to_cmd; /* tsn alone already identifies which outstanding
                         * command this answers -- see s_inflight's comment
                         * above; nothing else this engine sends could
                         * share it. */
    bool        found = false;
    int         slot = -1;
    actor_cmd_t cmd = { 0 };

    portENTER_CRITICAL(&s_inflight_mux);
    for (int i = 0; i < ZB_CMD_MAX_INFLIGHT; i++) {
        if (s_inflight[i].used && s_inflight[i].tsn == tsn) {
            found = true;
            slot = i;
            cmd = s_inflight[i].cmd;
            s_inflight[i].used = false;
            break;
        }
    }
    portEXIT_CRITICAL(&s_inflight_mux);

    if (!found) {
        /* A response for a command that already timed out (and so had its
         * slot reclaimed), a duplicate, or something this engine never
         * sent at all. Dropped, not alerted: there is no outstanding
         * obligation left here to report against. */
        ESP_LOGD(TAG, "default response (tsn=%u cluster=0x%04x status=0x%02x) matched no "
                      "outstanding Zigbee command", (unsigned)tsn, (unsigned)cluster,
                 (unsigned)status_code);
        return;
    }

    /* Runs on the Zigbee stack task (zigbee.c's zb_core_action_handler
     * calls this directly from its own ESP_ZB_CORE_CMD_DEFAULT_RESP_CB_ID
     * case) -- no esp_zb_lock needed for this SDK call, the same as every
     * other stack-task callback in zigbee.c. */
    esp_zb_scheduler_alarm_cancel(zb_cmd_timeout_cb, (uint8_t)slot);

    bool ok = (status_code == ESP_ZB_ZCL_STATUS_SUCCESS);
    zb_cmd_report(cmd.dev_idx, cmd.action_id, cmd.param, ok,
                  ok ? NULL : "device returned a non-success Default Response status");
}

/* actor_service()'s DEV_KIND_ZIGBEE dispatch hook (actor_dispatch_fn_t,
 * M6b Task 7). Every early return goes through zb_cmd_report(false, ...):
 * this command has already passed every guard, been popped from the actor
 * queue and charged against its hourly budget (actor_service_step()), so
 * there is no second chance for it and no other place its disappearance
 * would surface -- the same reasoning ble_collector.c's on_actor_dispatch()
 * gives for its own early returns. */
static void on_zb_dispatch(const actor_cmd_t *cmd)
{
    uint8_t on_off_cmd_id;
    switch (cmd->action_id) {
    case ACT_SWITCH_ON:  on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_ON_ID;  break;
    case ACT_SWITCH_OFF: on_off_cmd_id = ESP_ZB_ZCL_CMD_ON_OFF_OFF_ID; break;
    default:
        /* Requirement 1: an action id this engine does not implement
         * (ACT_IRRIGATION_OPEN, ACT_PUMP_RUN -- action.h) must post an
         * alert and never be silently dropped. */
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "the Zigbee command engine does not implement this action");
        return;
    }

    /* actor_device_key() gives back the dispatched device's stable
     * identity -- device_id_t's raw bytes, key[0] the kind and key[1..8]
     * the 8-byte address (capability.h; actor.c's own comment on
     * ACTOR_DEVICE_KEY_LEN). actor_service() only ever calls THIS hook for
     * a command whose kind already resolved to DEV_KIND_ZIGBEE, so the
     * kind check below is belt-and-suspenders, not a real branch this
     * engine expects to take -- but requirement 2 asks for a device this
     * engine cannot resolve to fail visibly rather than assumed away. */
    uint8_t key[ACTOR_DEVICE_KEY_LEN];
    if (!actor_device_key(cmd->dev_idx, key) || key[0] != DEV_KIND_ZIGBEE) {
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "no stable Zigbee identity for this device");
        return;
    }
    const uint8_t *eui64 = key + 1;

    /* Requirement 2: a device absent from the store, or with no usable
     * short_addr, cannot be commanded. */
    uint16_t short_addr;
    uint8_t  endpoint;
    if (!zigbee_store_lookup(eui64, &short_addr, &endpoint)) {
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "device not present in the Zigbee store");
        return;
    }
    if (short_addr == 0xFFFF) {
        /* 0xFFFF is the ZDO/ZCL spec's own "invalid address" sentinel (see
         * esp_zigbee_zdo_command.h's own doc comments on every ZDO
         * response that carries one) -- never a real joined device's short
         * address. */
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "device has no usable short address");
        return;
    }

    int slot = -1;
    portENTER_CRITICAL(&s_inflight_mux);
    for (int i = 0; i < ZB_CMD_MAX_INFLIGHT; i++) {
        if (!s_inflight[i].used) {
            s_inflight[i].used = true;
            s_inflight[i].cmd  = *cmd;
            slot = i;
            break;
        }
    }
    portEXIT_CRITICAL(&s_inflight_mux);
    if (slot < 0) {
        ESP_LOGW(TAG, "%u Zigbee command(s) already outstanding; dev=%d action=%u dropped",
                 (unsigned)ZB_CMD_MAX_INFLIGHT, (int)cmd->dev_idx, (unsigned)cmd->action_id);
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "too many Zigbee commands already outstanding");
        return;
    }

    /* An "outside" caller of the stack (this file's own top comment) --
     * hold the lock for both SDK calls below, the same discipline
     * zigbee_permit_join()/zigbee_device_remove() (zigbee.c) already use
     * for the identical reason. */
    if (!esp_zb_lock_acquire(pdMS_TO_TICKS(ZB_CMD_LOCK_WAIT_MS))) {
        portENTER_CRITICAL(&s_inflight_mux);
        s_inflight[slot].used = false;
        portEXIT_CRITICAL(&s_inflight_mux);
        zb_cmd_report(cmd->dev_idx, cmd->action_id, cmd->param, false,
                      "could not acquire the Zigbee stack lock");
        return;
    }

    esp_zb_zcl_on_off_cmd_t req = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = short_addr,
            .dst_endpoint = endpoint,
            .src_endpoint = zigbee_coordinator_endpoint(),
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .on_off_cmd_id = on_off_cmd_id,
    };
    uint8_t tsn = esp_zb_zcl_on_off_cmd_req(&req);

    portENTER_CRITICAL(&s_inflight_mux);
    s_inflight[slot].tsn = tsn;
    portEXIT_CRITICAL(&s_inflight_mux);

    /* Scheduled while still holding the stack lock, same as the send
     * itself -- see this file's top comment on why every SDK call in this
     * function needs it. */
    esp_zb_scheduler_alarm(zb_cmd_timeout_cb, (uint8_t)slot, ZB_CMD_TIMEOUT_MS);
    esp_zb_lock_release();
}

void zb_cmd_start(void)
{
    actor_set_dispatch_hook(DEV_KIND_ZIGBEE, on_zb_dispatch);
}

#else /* !CONFIG_PLANTHUB_ZB_ENABLED */

void zb_cmd_start(void)
{
}

void zb_cmd_on_default_resp(uint8_t tsn, uint16_t cluster, uint8_t resp_to_cmd,
                             uint8_t status_code)
{
    (void)tsn;
    (void)cluster;
    (void)resp_to_cmd;
    (void)status_code;
}

#endif /* CONFIG_PLANTHUB_ZB_ENABLED */
