#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "bridge_cmd.h"

int main(void)
{
    bridge_router_t r; bridge_cmd_init(&r);
    uint8_t m[6] = {1,2,3,4,5,6};
    swarm_dev_addr_t d = { .kind = 2, .addr = {1} };
    assert(bridge_cmd_submit(&r, m, SWARM_CMD_ACTUATE, &d, 0x0100, NULL, 0, 30, 1000, 3, 0, 1));
    assert(!bridge_cmd_submit(&r, m, SWARM_CMD_PERMIT_JOIN, NULL, 0, NULL, 0, 30, 1000, -1, 0, 0));  /* one in flight */
    const bridge_cmd_t *s = bridge_cmd_next_send(&r, 1000);
    assert(s && s->seq == 1 && s->op == SWARM_CMD_ACTUATE && s->sends == 1);
    assert(bridge_cmd_next_send(&r, 1001) == NULL);              /* not yet due for retry */
    s = bridge_cmd_next_send(&r, 1003); assert(s && s->sends == 2);   /* retry after 2 s */
    assert(bridge_cmd_next_send(&r, 1010) == NULL);              /* only one retry */
    swarm_command_ack_t acc = { .seq = 1, .op = SWARM_CMD_ACTUATE, .status = SWARM_ACK_ACCEPTED };
    bridge_cmd_t done;
    assert(!bridge_cmd_on_ack(&r, m, &acc, 1004, &done));         /* accepted: still in flight */
    swarm_command_ack_t dup = acc; assert(!bridge_cmd_on_ack(&r, m, &dup, 1005, &done));
    swarm_command_ack_t fin = { .seq = 1, .op = SWARM_CMD_ACTUATE, .status = SWARM_ACK_DONE, .detail = 0 };
    assert(bridge_cmd_on_ack(&r, m, &fin, 1006, &done) && done.actor_dev_idx == 3 && done.active == false);
    assert(bridge_cmd_submit(&r, m, SWARM_CMD_PERMIT_JOIN, NULL, 0, NULL, 0, 30, 2000, -1, 0, 0));   /* slot free again */
    s = bridge_cmd_next_send(&r, 2000); assert(s && s->seq == 2);
    swarm_command_ack_t wrong = { .seq = 9, .op = SWARM_CMD_PERMIT_JOIN, .status = SWARM_ACK_DONE };
    assert(!bridge_cmd_on_ack(&r, m, &wrong, 2001, &done));      /* unknown seq ignored */
    bridge_cmd_t ex; assert(!bridge_cmd_expire(&r, 2029, &ex));
    assert(bridge_cmd_expire(&r, 2031, &ex) && ex.op == SWARM_CMD_PERMIT_JOIN);   /* ttl 30 passed */
    assert(bridge_cmd_next_send(&r, 2032) == NULL);
    uint8_t other[6] = {9,9,9,9,9,9};
    assert(bridge_cmd_submit(&r, other, SWARM_CMD_RESYNC, NULL, 0, NULL, 0, 10, 3000, -1, 0, 0));   /* per-node slots */

    /* M7: bridge_cmd_peek_pending()/bridge_cmd_mark_sent() (POLL-triggered
     * opportunistic send). `other` has a fresh, never-sent RESYNC (sends==0,
     * not accepted) -- peek must return it. */
    const bridge_cmd_t *pk = bridge_cmd_peek_pending(&r, other);
    assert(pk && pk->op == SWARM_CMD_RESYNC && pk->sends == 0);
    bridge_cmd_mark_sent(&r, other, 3001);
    /* mark_sent bumped sends to 1 and sent_s to 3001 -- next_send() must not
     * offer it again immediately (not due for retry until BRIDGE_CMD_RETRY_S
     * later), the whole point of calling mark_sent after an out-of-band send. */
    assert(bridge_cmd_next_send(&r, 3001) == NULL);
    assert(bridge_cmd_next_send(&r, 3002) == NULL);          /* still < retry interval */
    const bridge_cmd_t *s2 = bridge_cmd_next_send(&r, 3003); assert(s2 && s2->sends == 2);   /* retry due */

    /* Unknown mac: no active slot. */
    uint8_t unknown[6] = {8,8,8,8,8,8};
    assert(bridge_cmd_peek_pending(&r, unknown) == NULL);
    bridge_cmd_mark_sent(&r, unknown, 3010);   /* no-op, must not crash */

    /* Accepted slot: peek must return NULL (already progressing, no resend). */
    uint8_t acc_mac[6] = {7,7,7,7,7,7};
    assert(bridge_cmd_submit(&r, acc_mac, SWARM_CMD_ACTUATE, &d, 0, NULL, 0, 30, 4000, -1, 0, 0));
    assert(bridge_cmd_peek_pending(&r, acc_mac) != NULL);     /* not yet accepted: pending */
    swarm_command_ack_t acc2 = { .seq = 1, .op = SWARM_CMD_ACTUATE, .status = SWARM_ACK_ACCEPTED };
    bridge_cmd_t done2;
    assert(!bridge_cmd_on_ack(&r, acc_mac, &acc2, 4001, &done2));   /* still in flight, now accepted */
    assert(bridge_cmd_peek_pending(&r, acc_mac) == NULL);     /* accepted: nothing to flush-send */

    printf("test_bridge_cmd: OK\n");
    return 0;
}
