#pragma once
/* Pure connection state machine for M5a's GATT read path (design spec
 * docs/superpowers/specs/2026-08-17-planthub-v2-m5a-gatt-read-design.md
 * section 3). No ESP-IDF, no NimBLE, no timers, no I/O: every sequence
 * here -- including every failure path -- is provable on the host, because
 * the radio adapter that drives this state machine (a later task) talks to
 * a radio and cannot be host-tested at all.
 *
 * gatt_fsm_step() never allocates, never blocks and never calls out; it
 * only inspects one event and returns one action. The caller (the NimBLE
 * adapter) is this module's entire I/O boundary: it performs the returned
 * action against the radio and feeds the resulting callback back in as the
 * next event. */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * IDLE -> CONNECTING -> [WRITING] -> READING -> DECODING -> DONE
 *    \        |             |           |           |
 *     \       +-------------+-----------+-----------+--> FAILED
 *      \                (GE_TIMEOUT/GE_ERROR: GA_DISCONNECT)
 *       `------------------- (unsolicited GE_DISCONNECTED: GA_REPORT_FAIL) ---'
 *
 * No discovery state (removed during the M5a hardware gate -- see
 * gatt_fsm_init()'s own doc comment for why: every read and write is now
 * addressed by UUID, resolved by the SERVER on each connection, so there is
 * nothing left for a discovery pass to produce that the read/write path
 * still needs). WRITING is skipped when the plan has no writes. DECODING is
 * the one state between the last read landing and the connection actually
 * being torn down -- see GE_DECODED's own doc note below for why it exists.
 * DONE and FAILED are both terminal: gatt_fsm_step() ignores every event
 * received in either one. */
typedef enum { GS_IDLE, GS_CONNECTING, GS_WRITING,
               GS_READING, GS_DECODING, GS_DONE, GS_FAILED } gatt_state_t;

/* GE_DECODED: fed back once the caller has run the decode GA_DECODE asked
 * for. It exists so the success path can emit GA_DISCONNECT itself
 * instead of leaving teardown to a rule the caller must remember
 * unaided -- see gatt_fsm_step()'s doc comment for the invariant this
 * buys. */
typedef enum { GE_START, GE_CONNECTED, GE_DISCONNECTED,
               GE_WRITE_OK, GE_READ_OK, GE_DECODED, GE_ERROR, GE_TIMEOUT } gatt_ev_kind_t;

/* handle: which characteristic this completion is for (uuid16). Both
 * GE_READ_OK and GE_WRITE_OK are checked against it: each compares
 * against the uuid16 the currently-awaited read or write expects (from
 * the plan) and ignores the event on a mismatch, exactly as it ignores
 * any other event that makes no sense right now. This is deliberately
 * checked here rather than left to the adapter -- this pure module is
 * the only part of M5a's GATT read path a host test can reach, so a
 * same-state duplicate (a second completion of the SAME kind arriving
 * for the read or write that already finished, while still in
 * GS_READING/GS_WRITING waiting on the next one) has to be caught here or
 * it isn't provably caught at all: unlike a duplicate of the wrong EVENT
 * KIND, which the (state, event-kind) dispatch below already rejects on
 * its own, two READ_OKs (or two WRITE_OKs) in a row are the same kind in
 * the same state, and without this check the second one would be
 * accepted as the next step's completion. For a read that means bytes
 * written into the wrong slot -- a decode that succeeds and is silently
 * wrong. For a write it means the real write is skipped or never
 * confirmed while the sequence proceeds as though it landed -- the same
 * hazard one event kind over: a device left unwoken or in its default
 * mode still answers the reads that follow with plausible bytes. Task 6's
 * adapter may still filter using its own NimBLE callback context too
 * (defence in depth is fine); it must not be the only layer that does. */
typedef struct { gatt_ev_kind_t kind; uint16_t handle; const uint8_t *data; uint8_t len; } gatt_ev_t;

typedef enum { GA_NONE, GA_CONNECT, GA_WRITE, GA_READ,
               GA_DISCONNECT, GA_DECODE, GA_REPORT_FAIL } gatt_act_kind_t;

/* uuid16: which characteristic to write/read (GA_WRITE/GA_READ) -- the
 * caller resolves it to an actual GATT handle itself, on the SERVER's own
 * say-so, addressing it by UUID on every connection (spec section 4, as
 * amended during the M5a hardware gate: handles are never written into a
 * wrapper NOR cached across a connection, and this module is the same
 * discipline applied to the connection manager's own state -- it never
 * sees or stores a handle at all, only the uuid16 the plan named). data/len:
 * the write payload for GA_WRITE, or the assembled read buffer for
 * GA_DECODE (see gatt_fsm_t.buf's doc comment); NULL/0 for every other
 * action kind. */
typedef struct { gatt_act_kind_t kind; uint16_t uuid16; const uint8_t *data; uint8_t len; } gatt_act_t;

/* Mirrors psvm.h's PSVM_PLAN_MAX_READS / PSVM_PLAN_MAX_WRITES /
 * PSVM_PLAN_WRITE_MAX / PSVM_PLAN_SLOT (components/psvm/include/psvm.h).
 * Duplicated rather than included: this component has no REQUIRES on psvm
 * (Task 6's adapter is the first thing in this component that needs one,
 * per this task's brief). If either copy of these four numbers ever
 * changes, the other must change with it by hand -- nothing in the build
 * enforces that automatically here, unlike the browser compiler's
 * plan-limits.js, which imports psvm.h's numbers by document reference and
 * mirrors its _Static_assert. */
#define GATT_FSM_MAX_READS  4
#define GATT_FSM_MAX_WRITES 2
#define GATT_FSM_WRITE_MAX  8
#define GATT_FSM_SLOT       16

typedef struct {
    gatt_state_t state;

    uint8_t read_count;
    uint8_t write_count;
    uint8_t read_idx;
    uint8_t write_idx;

    uint16_t read_uuid[GATT_FSM_MAX_READS];
    /* Fewest bytes each read must return for its slot to be decodable:
     * max(offset + accessor width) over the decode block's accessors for
     * that buffer, computed by the compiler and carried in the plan. A
     * shorter read fails the attempt rather than zero-padding its slot into
     * a plausible wrong value. 1..GATT_FSM_SLOT. */
    uint8_t  read_min_len[GATT_FSM_MAX_READS];

    struct {
        uint16_t uuid16;
        uint8_t  len;
        const uint8_t *data;   /* points into the plan buffer passed to gatt_fsm_init() */
    } write[GATT_FSM_MAX_WRITES];

    /* Concatenated read buffer, GATT_FSM_MAX_READS x GATT_FSM_SLOT bytes.
     * Zeroed whole at gatt_fsm_init() and re-zeroed per slot immediately
     * before each read lands, so a short read zero-pads its slot and a
     * failed or short read can never leave a previous device's bytes
     * sitting in a slot it didn't write. GA_DECODE's action.data points
     * here; action.len is read_count * GATT_FSM_SLOT, the meaningful
     * prefix (any unused trailing slots, when read_count < the max, stay
     * zeroed but are not included in len). */
    uint8_t buf[GATT_FSM_MAX_READS * GATT_FSM_SLOT];
} gatt_fsm_t;

/* Parses the trailing PSBC connect-plan section (psvm.h's
 * PSVM_FLAG_CONNECT_PLAN doc comment has the on-blob layout this mirrors:
 * u8 read_count, u8 write_count, u32 interval_s LE, then read_count x
 * {u16 uuid16 LE, u8 min_len}, then write_count x {u16 uuid16 LE, u8 len, u8
 * data[len]}) directly from (plan, plan_len). This module does NOT assume
 * psvm_validate() already ran: every offset is bounds-checked against
 * plan_len as it is derived, never trusted from the counts alone, and
 * interval_s is parsed (to keep offsets aligned with the on-blob layout)
 * but otherwise unused -- the interval gate is Task 5's scheduler, not
 * this state machine. A plan too short or malformed to parse in full
 * yields whatever prefix of reads/writes DID fit inside plan_len and never
 * reads past it; read_count/write_count are clamped to
 * GATT_FSM_MAX_READS/GATT_FSM_MAX_WRITES regardless.
 *
 * No discovery step and no handle cache (removed during the M5a hardware
 * gate): the bench found that a service inserted ahead of the real one on
 * the peripheral shifted every ATT handle, so a cached handle from an
 * earlier discovery pointed at a DIFFERENT attribute -- a declaration,
 * which is always readable -- and the read "succeeded" with the wrong
 * bytes decoded as plausible garbage. §4's invalidation rule (drop the
 * cache on a failed read) could not catch this, because the read never
 * failed. So every read and write is now addressed by uuid16 alone, on
 * every connection, and the caller resolves it to a handle itself via a
 * server-side UUID lookup (NimBLE's read-by-UUID procedure) that cannot be
 * misled by handle drift -- see gatt_ev_t.handle's doc comment. fsm is
 * fully re-initialized (including f->state = GS_IDLE and a zeroed read
 * buffer); a previous connection's leftovers can never leak into a new
 * one this way. */
void gatt_fsm_init(gatt_fsm_t *fsm, const uint8_t *plan, size_t plan_len);

/* Advances the state machine by exactly one event, returning exactly one
 * action.
 *
 * An event that does not make sense in the current state -- a duplicate or
 * out-of-order callback from the radio stack -- is ignored: GA_NONE, no
 * state change, nothing about an in-flight sequence is touched. This
 * includes a GE_READ_OK whose handle names a characteristic other than
 * the one GS_READING is currently waiting on (see gatt_ev_t.handle's own
 * doc comment).
 *
 * The last read landing does not go straight to GS_DONE: it returns
 * GA_DECODE and moves to GS_DECODING, and only the caller's GE_DECODED
 * (fed back once it has actually run the decode) produces GA_DISCONNECT
 * and reaches GS_DONE. This is what makes the following invariant hold:
 *
 *     Every path from GS_CONNECTING to a terminal state emits exactly
 *     one GA_DISCONNECT -- EXCEPT an unsolicited GE_DISCONNECTED path,
 *     which emits exactly one GA_REPORT_FAIL and no GA_DISCONNECT at all
 *     (see below for why). test_gatt_fsm.c's
 *     test_exactly_one_disconnect_per_terminal_path pins this by
 *     execution, not by inspection.
 *
 * Without GS_DECODING, the success path would be the one case with ZERO
 * GA_DISCONNECT actions, leaving the adapter with two different rules for
 * ending a connection -- and the one it would have to remember unaided is
 * the common path. If it ever forgot, the link would stay open and the
 * hub would stay deaf to advertisements, which is exactly what the
 * design spec's 5-second deadline (section 3) exists to bound.
 *
 * GE_TIMEOUT and GE_ERROR, in any state that has a connection open
 * (GS_CONNECTING/GS_WRITING/GS_READING/GS_DECODING), all end the attempt
 * at GS_FAILED via GA_DISCONNECT: the link may still be up, so the caller
 * must actively command it closed, and a read error in particular must
 * never fall through to GA_DECODE (this module always transitions to
 * GS_FAILED instead of advancing read_idx on GE_ERROR, so half-read data
 * is never decoded as if it were complete).
 *
 * An unsolicited GE_DISCONNECTED (the peer or the radio dropped the link
 * for a reason nothing in this module caused) also ends at GS_FAILED, but
 * via GA_REPORT_FAIL instead of GA_DISCONNECT -- deliberately, and this is
 * the one place the "exactly one GA_DISCONNECT" invariant above does not
 * apply: the link is already down, so there is nothing left to command,
 * only to report -- so the caller can log the failure. Asking for a second
 * disconnect of a link that is already gone would not make the invariant
 * more uniform, only redundant.
 *
 * GS_DONE and GS_FAILED are terminal: every event received in either
 * state returns GA_NONE with no state change, including a GE_DISCONNECTED
 * that arrives afterward confirming the GA_DISCONNECT this module already
 * asked for. */
gatt_act_t gatt_fsm_step(gatt_fsm_t *fsm, const gatt_ev_t *ev);
