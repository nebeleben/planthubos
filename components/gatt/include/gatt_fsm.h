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

typedef enum { GS_IDLE, GS_CONNECTING, GS_DISCOVERING, GS_WRITING,
               GS_READING, GS_DONE, GS_FAILED } gatt_state_t;

typedef enum { GE_START, GE_CONNECTED, GE_DISCONNECTED, GE_DISCOVERED,
               GE_WRITE_OK, GE_READ_OK, GE_ERROR, GE_TIMEOUT } gatt_ev_kind_t;

/* handle: opaque passthrough from the caller (e.g. which characteristic a
 * GE_READ_OK/GE_WRITE_OK completed for). This module never reads it: it
 * has exactly one radio operation outstanding at a time and only ever
 * treats the NEXT event of the kind it is waiting for as that operation's
 * completion. Telling a genuine completion apart from a stale/duplicate
 * callback for a DIFFERENT outstanding request is the adapter's job --
 * NimBLE's own callback context (Task 6) already ties a completion to the
 * specific request that produced it, before this module ever sees an
 * event, so a duplicate the adapter forwards anyway only ever lands here
 * as an event kind this module's current state does not expect, which
 * gatt_fsm_step() ignores (see its own doc comment). */
typedef struct { gatt_ev_kind_t kind; uint16_t handle; const uint8_t *data; uint8_t len; } gatt_ev_t;

typedef enum { GA_NONE, GA_CONNECT, GA_DISCOVER, GA_WRITE, GA_READ,
               GA_DISCONNECT, GA_DECODE, GA_REPORT_FAIL } gatt_act_kind_t;

/* uuid16: which characteristic to write/read (GA_WRITE/GA_READ) -- the
 * caller resolves it to an actual GATT handle itself (from discovery or
 * its own warm cache), since this module never sees or stores handles
 * (spec section 4: handles are never written into a wrapper, and this
 * module is the same discipline applied to the connection manager's own
 * state). data/len: the write payload for GA_WRITE, or the assembled read
 * buffer for GA_DECODE (see gatt_fsm_t.buf's doc comment); NULL/0 for
 * every other action kind. */
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

    bool    have_handles;
    uint8_t read_count;
    uint8_t write_count;
    uint8_t read_idx;
    uint8_t write_idx;

    uint16_t read_uuid[GATT_FSM_MAX_READS];

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
 * {u16 uuid16 LE}, then write_count x {u16 uuid16 LE, u8 len, u8
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
 * have_handles: true when the caller's warm handle cache already has
 * every characteristic this plan needs for this device, which skips
 * GS_DISCOVERING entirely (spec section 4). fsm is fully re-initialized
 * (including f->state = GS_IDLE and a zeroed read buffer); a previous
 * connection's leftovers can never leak into a new one this way. */
void gatt_fsm_init(gatt_fsm_t *fsm, const uint8_t *plan, size_t plan_len, bool have_handles);

/* Advances the state machine by exactly one event, returning exactly one
 * action.
 *
 * An event that does not make sense in the current state -- a duplicate or
 * out-of-order callback from the radio stack -- is ignored: GA_NONE, no
 * state change, nothing about an in-flight sequence is touched.
 *
 * GE_TIMEOUT and GE_ERROR, in any state that has a connection open
 * (GS_CONNECTING/GS_DISCOVERING/GS_WRITING/GS_READING), both end the
 * attempt at GS_FAILED via GA_DISCONNECT: the link may still be up, so the
 * caller must actively command it closed, and a read error in particular
 * must never fall through to GA_DECODE (this module always transitions to
 * GS_FAILED instead of advancing read_idx on GE_ERROR, so half-read data
 * is never decoded as if it were complete).
 *
 * An unsolicited GE_DISCONNECTED (the peer or the radio dropped the link
 * for a reason nothing in this module caused) also ends at GS_FAILED, but
 * via GA_REPORT_FAIL instead of GA_DISCONNECT: the link is already down,
 * so there is nothing left to command, only to report -- so the caller can
 * log the failure and drop this device's handle cache (spec section 4: a
 * failed attempt is precisely the right trigger to rediscover next time).
 *
 * GS_DONE and GS_FAILED are terminal: every event received in either
 * state returns GA_NONE with no state change, including a GE_DISCONNECTED
 * that arrives afterward confirming a disconnect this module already
 * asked for (or, on the success path, one the caller performs on its own
 * once GA_DECODE comes back -- see the module's own top comment). */
gatt_act_t gatt_fsm_step(gatt_fsm_t *fsm, const gatt_ev_t *ev);
