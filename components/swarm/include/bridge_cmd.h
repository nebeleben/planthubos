#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "swarm_frame.h"
#include "bridge_table.h"   /* BRIDGE_MAX_NODES -- one router slot per bridge
                              * node, same sizing as the bridge table itself
                              * (both are keyed by bridge node mac, and a hub
                              * never has more bridge nodes than
                              * BRIDGE_MAX_NODES). Pure C, no ESP-IDF -- safe
                              * to pull in here for the same reason it is
                              * host-testable on its own. */

/* Hub-side command router (M7 Task 8): tracks, per bridge node, the ONE
 * outstanding SWARM_MSG_COMMAND this hub may have in flight to it at a
 * time -- PERMIT_JOIN, DEVICE_REMOVE, DEVICE_RENAME, ACTUATE or RESYNC --
 * from submission through retry, ack correlation and TTL expiry. Pure C,
 * no ESP-IDF, no locking of its own: swarm.c's bridge_task is this
 * router's only caller (see swarm.c's own comment on that), so every
 * function here runs single-threaded from that task's point of view and
 * needs no mutex -- exactly the same split as bridge_table.c/.h.
 *
 * One slot per node, not per command: a hub that wants to send a SECOND
 * command to a node already has one outstanding must wait for it to
 * finish (bridge_cmd_submit() refuses) -- this keeps the wire protocol,
 * and this router, simple: at most one seq per node is ever "live", so an
 * ack can never be ambiguous about which of several in-flight commands it
 * answers. Task 9's HTTP handlers see that refusal as ESP_ERR_INVALID_STATE
 * (swarm.c's swarm_bridge_permit/remove/rename wrap this router).
 *
 * Node/seq assignment is sticky: submitting to a mac this router has never
 * seen claims the next free slot AND that slot's own per-node seq counter
 * (next_seq[]), which then belongs to that mac for the life of this boot --
 * a later command to the SAME mac, even after its previous command
 * finished and the slot went idle, reuses the same slot index and keeps
 * counting up from where that node's seq counter left off (never reusing
 * seq 0, and never restarting from 1 for a node this router already knows,
 * so a stale ack for an old seq can never be mistaken for the new
 * command's). A slot is "known but idle" when its bound mac is non-zero
 * and slot[i].active is false; "never used" when its bound mac is still
 * all-zero (bridge_cmd_init()'s zeroed state) -- a real ESP-NOW station
 * MAC is never all-zero, so that is an unambiguous sentinel, the same
 * "zeroed struct is the empty state" convention swarm_store.c and
 * bridge_table.c both already rely on. */
#define BRIDGE_CMD_RETRY_S 2

typedef struct {
    uint8_t          mac[6];
    uint16_t         seq;
    uint8_t          op;             /* SWARM_CMD_* */
    swarm_dev_addr_t dev;
    uint16_t         arg;
    char             name[SWARM_DEV_NAME_MAX];
    uint8_t          name_len;
    uint32_t         sent_s;         /* last time bridge_cmd_next_send() sent this, 0 before the first send */
    uint32_t         deadline_s;     /* absolute; bridge_cmd_expire()'s cutoff */
    uint8_t          sends;          /* 0 (never sent), 1 (sent once), 2 (retried once -- no further retry) */
    bool             accepted;       /* SWARM_ACK_ACCEPTED seen -- next_send() stops retrying once true */
    bool             active;         /* a command is genuinely outstanding in this slot */
    int              actor_dev_idx;  /* -1 for a non-ACTUATE op (PERMIT_JOIN/REMOVE/RENAME/RESYNC
                                       * have no actor to report back to); the dispatched device's
                                       * index for ACTUATE, so DONE/FAILED/expiry can report through
                                       * the same actor/alert path a local dispatch would */
    uint8_t          actor_action;
    uint16_t         actor_param;
} bridge_cmd_t;

typedef struct {
    bridge_cmd_t slot[BRIDGE_MAX_NODES];
    uint16_t     next_seq[BRIDGE_MAX_NODES];   /* per-node, sticky -- see this header's top comment */
    uint8_t      macs[BRIDGE_MAX_NODES][6];    /* which mac owns slot[i]/next_seq[i]; all-zero = unbound */
} bridge_router_t;

void bridge_cmd_init(bridge_router_t *r);

/* Queues a command for mac. False (nothing changed) when mac already has
 * one in flight (slot[i].active), or when this router has never seen mac
 * before and every slot is already bound to a different node
 * (BRIDGE_MAX_NODES exceeded). seq is assigned from mac's own sticky
 * counter, skipping 0 (0 is never a valid seq -- matches swarm.c's
 * existing s_resync_seq convention). deadline_s = now_s + ttl_s.
 * actor_dev_idx/actor_action/actor_param are opaque to this router --
 * carried through untouched so swarm.c can report an ACTUATE's outcome to
 * the actor layer on DONE/FAILED/expiry; pass actor_dev_idx = -1 for any
 * op that has no actor to report to. dev/name/name_len are copied
 * verbatim (name truncated to SWARM_DEV_NAME_MAX if longer -- callers are
 * expected to already respect that limit; this is a defensive clamp, not
 * a normal path). */
bool bridge_cmd_submit(bridge_router_t *r, const uint8_t mac[6], uint8_t op, const swarm_dev_addr_t *dev,
                       uint16_t arg, const char *name, uint8_t name_len, uint32_t ttl_s, uint32_t now_s,
                       int actor_dev_idx, uint8_t actor_action, uint16_t actor_param);

/* Returns the next active slot due to (re)send, or NULL when nothing is
 * due right now. A freshly-submitted command (sends == 0) is always due
 * immediately; an unaccepted command (sends == 1, !accepted) becomes due
 * again once BRIDGE_CMD_RETRY_S has passed since it was last sent. Never
 * retries a second time (sends == 2 is a dead end here -- only
 * bridge_cmd_on_ack()/bridge_cmd_expire() can move it on from there).
 * Marks the returned slot sent (bumps sends, sets sent_s = now_s) before
 * returning it -- callers must actually encode and send it; there is no
 * "peek without marking sent" mode. The returned pointer aliases *r and is
 * only valid until the next bridge_cmd_* call on this router. */
const bridge_cmd_t *bridge_cmd_next_send(bridge_router_t *r, uint32_t now_s);

/* Correlates an inbound SWARM_MSG_COMMAND_ACK from mac against whatever
 * this router currently has outstanding for it. A mismatched mac (unknown
 * to this router), an idle slot, or a seq that doesn't match the
 * outstanding command's own seq (stale/duplicate/foreign) is silently
 * ignored -- returns false, *done_out untouched. SWARM_ACK_ACCEPTED marks
 * the slot accepted (stops bridge_cmd_next_send() from retrying it) and
 * returns false -- the command is still in flight. SWARM_ACK_DONE/
 * SWARM_ACK_FAILED copies the finished slot into *done_out (with
 * ->active forced false, since the command is now over), clears the slot
 * so the node's next submit can reuse it, and returns true. */
bool bridge_cmd_on_ack(bridge_router_t *r, const uint8_t mac[6], const swarm_command_ack_t *ack,
                       uint32_t now_s, bridge_cmd_t *done_out);

/* Finds the first active slot whose deadline has passed without a DONE/
 * FAILED ack (now_s > deadline_s -- the deadline itself is still not
 * expired, same inclusive convention actor.h's own TTL check uses),
 * copies it into *expired_out (->active forced false) and clears it.
 * False (nothing changed, *expired_out untouched) when no slot has
 * expired. Only ever reports ONE expiry per call -- same "drain by
 * repeated calls" shape as the rest of this router's single-item
 * accessors -- so a caller running this on a periodic tick must call it
 * in a loop if it wants every expiry this pass, not just the first. */
bool bridge_cmd_expire(bridge_router_t *r, uint32_t now_s, bridge_cmd_t *expired_out);

/* M7: a POLL-triggered opportunistic-send peek (swarm.c's hub_rx_cb/
 * bridge_task, BRIDGE_ITEM_FLUSH) -- returns the active, not-yet-accepted
 * slot for mac (the same "still needs sending" condition
 * bridge_cmd_next_send() itself checks via !accepted), or NULL when mac is
 * unknown to this router, has no active command, or its outstanding
 * command has already been ACCEPTED (only DONE/FAILED clear a slot, but an
 * ACCEPTED one is already progressing on the bridge's side and a resend
 * would just be redundant). Unlike bridge_cmd_next_send(), this never
 * mutates the slot (no sends/sent_s bump) and ignores BRIDGE_CMD_RETRY_S
 * entirely -- the caller decides whether to actually act on the peek, and
 * must call bridge_cmd_mark_sent() itself if it does. Same pointer-aliasing
 * caveat as bridge_cmd_next_send(): valid only until the next bridge_cmd_*
 * call on this router. */
const bridge_cmd_t *bridge_cmd_peek_pending(bridge_router_t *r, const uint8_t mac[6]);

/* Records that the caller just sent (or attempted to send) mac's active
 * command outside the normal bridge_cmd_next_send() cadence -- updates
 * sent_s = now_s and bumps sends, the same two fields bridge_cmd_next_send()
 * itself updates on a normal (re)send, so bridge_cmd_next_send()'s own
 * due_first_send/due_retry checks see this as already sent and don't
 * immediately resend it again. No-op when mac has no active slot. */
void bridge_cmd_mark_sent(bridge_router_t *r, const uint8_t mac[6], uint32_t now_s);
