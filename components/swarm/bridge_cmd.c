/* bridge_cmd.c -- see bridge_cmd.h for the why. Pure C, no ESP-IDF;
 * swarm.c's bridge_task is this router's only caller and owns every
 * FreeRTOS/mutex/espnow_link_send() concern around it.
 */
#include "bridge_cmd.h"
#include <string.h>

static bool mac_is_zero(const uint8_t mac[6])
{
    static const uint8_t zero[6] = { 0 };
    return memcmp(mac, zero, 6) == 0;
}

void bridge_cmd_init(bridge_router_t *r)
{
    memset(r, 0, sizeof(*r));
}

/* Finds mac's slot index (bound via a prior submit). create=false: pure
 * lookup, -1 if mac has never been bound. create=true: also claims the
 * first still-unbound index for mac, -1 only when every slot already
 * belongs to a different mac (BRIDGE_MAX_NODES exceeded) -- see
 * bridge_cmd.h's top comment on why an all-zero macs[i] is the "never
 * used" sentinel. */
static int slot_for_mac(bridge_router_t *r, const uint8_t mac[6], bool create)
{
    int free_idx = -1;
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        if (!mac_is_zero(r->macs[i])) {
            if (memcmp(r->macs[i], mac, 6) == 0) return i;
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (!create || free_idx < 0) return -1;
    memcpy(r->macs[free_idx], mac, 6);
    return free_idx;
}

bool bridge_cmd_submit(bridge_router_t *r, const uint8_t mac[6], uint8_t op, const swarm_dev_addr_t *dev,
                       uint16_t arg, const char *name, uint8_t name_len, uint32_t ttl_s, uint32_t now_s,
                       int actor_dev_idx, uint8_t actor_action, uint16_t actor_param)
{
    int i = slot_for_mac(r, mac, true);
    if (i < 0) return false;                 /* every slot belongs to a different node */
    if (r->slot[i].active) return false;      /* one already in flight for this node */

    r->next_seq[i]++;
    if (r->next_seq[i] == 0) r->next_seq[i] = 1;   /* skip 0 on the (practically unreachable) wrap */

    bridge_cmd_t *s = &r->slot[i];
    memset(s, 0, sizeof(*s));
    memcpy(s->mac, mac, 6);
    s->seq = r->next_seq[i];
    s->op = op;
    if (dev) s->dev = *dev;
    s->arg = arg;
    if (name && name_len > 0) {
        uint8_t n = name_len > SWARM_DEV_NAME_MAX ? SWARM_DEV_NAME_MAX : name_len;
        memcpy(s->name, name, n);
        s->name_len = n;
    }
    s->deadline_s = now_s + ttl_s;
    s->sends = 0;
    s->accepted = false;
    s->active = true;
    s->actor_dev_idx = actor_dev_idx;
    s->actor_action = actor_action;
    s->actor_param = actor_param;
    return true;
}

const bridge_cmd_t *bridge_cmd_next_send(bridge_router_t *r, uint32_t now_s)
{
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        bridge_cmd_t *s = &r->slot[i];
        if (!s->active) continue;
        bool due_first_send = (s->sends == 0);
        bool due_retry = (s->sends == 1 && !s->accepted && (now_s - s->sent_s) >= BRIDGE_CMD_RETRY_S);
        if (!due_first_send && !due_retry) continue;
        s->sends++;
        s->sent_s = now_s;
        return s;
    }
    return NULL;
}

bool bridge_cmd_on_ack(bridge_router_t *r, const uint8_t mac[6], const swarm_command_ack_t *ack,
                       uint32_t now_s, bridge_cmd_t *done_out)
{
    (void)now_s;   /* no ack-side timing decision needs "now" today -- kept for symmetry
                     * with this router's other accessors and in case a future caller
                     * wants to log/attribute latency from it. */
    int i = slot_for_mac(r, mac, false);
    if (i < 0) return false;
    bridge_cmd_t *s = &r->slot[i];
    if (!s->active || s->seq != ack->seq) return false;   /* idle slot, or stale/foreign/unknown seq */

    if (ack->status == SWARM_ACK_ACCEPTED) {
        s->accepted = true;
        return false;   /* still in flight */
    }
    if (ack->status == SWARM_ACK_DONE || ack->status == SWARM_ACK_FAILED) {
        if (done_out) {
            *done_out = *s;
            done_out->active = false;
        }
        memset(s, 0, sizeof(*s));
        return true;
    }
    return false;   /* an unrecognised status: leave the command outstanding rather than guess */
}

const bridge_cmd_t *bridge_cmd_peek_pending(bridge_router_t *r, const uint8_t mac[6])
{
    int i = slot_for_mac(r, mac, false);
    if (i < 0) return NULL;
    bridge_cmd_t *s = &r->slot[i];
    if (!s->active || s->accepted) return NULL;
    return s;
}

void bridge_cmd_mark_sent(bridge_router_t *r, const uint8_t mac[6], uint32_t now_s)
{
    int i = slot_for_mac(r, mac, false);
    if (i < 0) return;
    bridge_cmd_t *s = &r->slot[i];
    if (!s->active) return;
    s->sent_s = now_s;
    s->sends++;
}

bool bridge_cmd_expire(bridge_router_t *r, uint32_t now_s, bridge_cmd_t *expired_out)
{
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        bridge_cmd_t *s = &r->slot[i];
        if (s->active && now_s > s->deadline_s) {
            if (expired_out) {
                *expired_out = *s;
                expired_out->active = false;
            }
            memset(s, 0, sizeof(*s));
            return true;
        }
    }
    return false;
}
