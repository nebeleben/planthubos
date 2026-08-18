#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gatt_fsm.h"

/* Trailing PSBC connect-plan section bytes (psvm.h's PSVM_FLAG_CONNECT_PLAN
 * doc comment / components/psvm/psvm.c's parser): read_count, write_count,
 * interval_s (u32 LE), reads[] (u16 LE each), writes[] (u16 LE uuid, u8
 * len, u8 data[len]). This is the exact 16-byte shape the M5a spec's own
 * `connect` block example compiles to (design spec section 2): one write
 * (0x2A00 = 01), two reads (0x2A6E temp, 0x2A6F hum). */
static const uint8_t PLAN_2READ_1WRITE[] = {
    2, 1,                       /* read_count=2, write_count=1 */
    0x58, 0x02, 0x00, 0x00,     /* interval_s = 600 */
    0x6E, 0x2A, 2,              /* read 0x2A6E, min_len 2 */
    0x6F, 0x2A, 2,              /* read 0x2A6F, min_len 2 */
    0x00, 0x2A, 1, 0x01,        /* write 0x2A00 = 01 */
};

static const uint8_t PLAN_1READ[] = {
    1, 0,                       /* read_count=1, write_count=0 */
    0x2C, 0x01, 0x00, 0x00,     /* interval_s = 300 */
    0x6E, 0x2A, 2,              /* read 0x2A6E, min_len 2 */
};

/* Two writes, so a duplicate GE_WRITE_OK for write 0 can be delivered
 * while write 1 is still outstanding (test_write_ok_wrong_uuid_ignored) --
 * PLAN_2READ_1WRITE only has one write, which can't exercise that case. */
static const uint8_t PLAN_2READ_2WRITE[] = {
    2, 2,                       /* read_count=2, write_count=2 */
    0x58, 0x02, 0x00, 0x00,     /* interval_s = 600 */
    0x6E, 0x2A, 2,              /* read 0x2A6E, min_len 2 */
    0x6F, 0x2A, 2,              /* read 0x2A6F, min_len 2 */
    0x00, 0x2A, 1, 0x01,        /* write 0x2A00 = 01 */
    0x01, 0x2A, 1, 0x02,        /* write 0x2A01 = 02 */
};

#define EV(k) ((gatt_ev_t){ .kind = (k), .handle = 0, .data = NULL, .len = 0 })
#define EV_READ(u, d, l) \
    ((gatt_ev_t){ .kind = GE_READ_OK, .handle = (u), .data = (const uint8_t *)(d), .len = (l) })
#define EV_WRITE(u) ((gatt_ev_t){ .kind = GE_WRITE_OK, .handle = (u), .data = NULL, .len = 0 })

/* The full sequence, in order. No discovery state (removed during the M5a
 * hardware gate -- gatt_fsm_init() no longer takes a have_handles argument
 * at all): GE_CONNECTED goes straight to writing/reading, because every
 * read and write is now addressed by uuid16 on every connection instead of
 * through a handle a discovery pass would have produced. */
static void test_full_sequence(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);

    assert(gatt_fsm_step(&f, &EV(GE_START)).kind      == GA_CONNECT);
    assert(f.state == GS_CONNECTING);
    assert(gatt_fsm_step(&f, &EV(GE_CONNECTED)).kind  == GA_WRITE);
    assert(f.state == GS_WRITING);
    assert(gatt_fsm_step(&f, &EV_WRITE(0x2A00)).kind == GA_READ);
    assert(f.state == GS_READING);
    assert(gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11\x22", 2)).kind == GA_READ);
    assert(f.state == GS_READING);
    gatt_act_t decode = gatt_fsm_step(&f, &EV_READ(0x2A6F, "\x33\x44", 2));
    assert(decode.kind == GA_DECODE);
    assert(f.state == GS_DECODING);
    assert(decode.len == 32);          /* 2 reads x GATT_FSM_SLOT */
    assert(memcmp(decode.data, "\x11\x22", 2) == 0);
    assert(memcmp(decode.data + 16, "\x33\x44", 2) == 0);

    /* Only the caller's GE_DECODED -- fed back once it has actually run
     * the decode -- tears the connection down; that is what makes the
     * success path emit GA_DISCONNECT the same as every failure path. */
    assert(gatt_fsm_step(&f, &EV(GE_DECODED)).kind == GA_DISCONNECT);
    assert(f.state == GS_DONE);

    assert(gatt_fsm_step(&f, &EV(GE_DISCONNECTED)).kind == GA_NONE);
    assert(f.state == GS_DONE);
}

/* A plan with no writes goes straight from GS_CONNECTING to GS_READING on
 * GE_CONNECTED -- the counterpart to test_full_sequence's write-then-read
 * path, and the shape every read-only connect wrapper actually uses. */
static void test_connect_goes_straight_to_reading_with_no_writes(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind     == GA_CONNECT);
    gatt_act_t a = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(a.kind == GA_READ);
    assert(a.uuid16 == 0x2A6E);
    assert(f.state == GS_READING);
}

/* A read failure must not decode, and must report so the caller can end
 * the attempt. */
static void test_read_error_fails_without_decoding(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_act_t a = gatt_fsm_step(&f, &EV(GE_ERROR));
    assert(a.kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED);
}

/* A short read still fills its slot; decode sees zero padding, never the
 * previous device's bytes. */
static void test_short_read_zero_pads(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ);
    /* Poison the slot the way a previous device's read could have left it,
     * to prove the zero-pad actively clears it rather than happening to
     * start clean. */
    memset(f.buf, 0xAA, sizeof f.buf);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));

    gatt_act_t decode = gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11\x22", 2));
    assert(decode.kind == GA_DECODE);
    assert(f.state == GS_DECODING);
    assert(decode.len == 16);
    assert(decode.data[0] == 0x11 && decode.data[1] == 0x22);
    for (int i = 2; i < 16; i++) assert(decode.data[i] == 0x00);
}

/* The deadline fires in every state and always ends in a disconnect.
 * Drives PLAN_2READ_1WRITE (2 reads, 1 write) up to but not including
 * `target`, so GS_DECODING is reachable too -- both reads must actually
 * land for that one, via the same EV_READ() uuids test_full_sequence
 * uses. */
static void drive_to_state(gatt_fsm_t *f, gatt_state_t target)
{
    gatt_fsm_init(f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);
    gatt_fsm_step(f, &EV(GE_START));
    if (target == GS_CONNECTING) return;
    gatt_fsm_step(f, &EV(GE_CONNECTED));
    if (target == GS_WRITING) return;
    gatt_fsm_step(f, &EV_WRITE(0x2A00));
    if (target == GS_READING) return;
    gatt_fsm_step(f, &EV_READ(0x2A6E, "\x11\x22", 2));
    gatt_fsm_step(f, &EV_READ(0x2A6F, "\x33\x44", 2));
    /* target == GS_DECODING */
}

/* A read shorter than the plan's min_len must FAIL the attempt, not
 * zero-pad its slot. Zero-padding would emit a plausible wrong value with
 * no failure recorded -- the silent-wrong-value shape spec section 4 exists
 * to prevent, reached by a short characteristic rather than handle drift. */
static void test_short_read_fails(void)
{
    gatt_fsm_t f;
    drive_to_state(&f, GS_READING);
    /* min_len is 2 for both reads in PLAN_2READ_1WRITE. */
    gatt_act_t a = gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11", 1));
    assert(a.kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED);
}

/* A zero-length response is the same defect at its limit: it must not count
 * as a successful read of a characteristic that returned nothing. */
static void test_empty_read_fails(void)
{
    gatt_fsm_t f;
    drive_to_state(&f, GS_READING);
    gatt_act_t a = gatt_fsm_step(&f, &EV_READ(0x2A6E, NULL, 0));
    assert(a.kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED);
}

/* Exactly min_len bytes is enough -- the boundary is inclusive. */
static void test_exact_min_len_read_ok(void)
{
    gatt_fsm_t f;
    drive_to_state(&f, GS_READING);
    gatt_act_t a = gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11\x22", 2));
    assert(a.kind == GA_READ);          /* proceeds to the second read */
    assert(f.state == GS_READING);
}

static const gatt_state_t OPEN_CONNECTION_STATES[] = {
    GS_CONNECTING, GS_WRITING, GS_READING, GS_DECODING,
};
#define N_OPEN_CONNECTION_STATES (sizeof OPEN_CONNECTION_STATES / sizeof OPEN_CONNECTION_STATES[0])

static void test_timeout_in_each_state(void)
{
    for (size_t i = 0; i < N_OPEN_CONNECTION_STATES; i++) {
        gatt_fsm_t f;
        drive_to_state(&f, OPEN_CONNECTION_STATES[i]);
        assert(f.state == OPEN_CONNECTION_STATES[i]);
        gatt_act_t a = gatt_fsm_step(&f, &EV(GE_TIMEOUT));
        assert(a.kind == GA_DISCONNECT);
        assert(f.state == GS_FAILED);
    }
}

/* An unsolicited disconnect in any state is safe and terminal. The link is
 * already down when this event arrives, so there is nothing left for the
 * caller to command -- only to report, which is why this is GA_REPORT_FAIL
 * rather than GA_DISCONNECT (contrast test_timeout_in_each_state, where
 * the link may still be open and the caller must actively close it). */
static void test_unsolicited_disconnect(void)
{
    for (size_t i = 0; i < N_OPEN_CONNECTION_STATES; i++) {
        gatt_fsm_t f;
        drive_to_state(&f, OPEN_CONNECTION_STATES[i]);
        assert(f.state == OPEN_CONNECTION_STATES[i]);
        gatt_act_t a = gatt_fsm_step(&f, &EV(GE_DISCONNECTED));
        assert(a.kind == GA_REPORT_FAIL);
        assert(f.state == GS_FAILED);
    }
}

/* A READ_OK's handle must match the read currently expected -- a
 * duplicate completion for the read that already finished, or one
 * carrying a uuid16 this plan never named at all, is ignored rather than
 * accepted for whichever read the FSM happens to be waiting on next. This
 * is the check the coordinator's fix-round-1 ruling required move into
 * this module itself (see gatt_ev_t.handle's doc comment): the adapter may
 * filter using its own NimBLE callback context too, but it must not be the
 * only layer that does, because it is the one layer a host test cannot
 * reach. */
static void test_read_ok_wrong_uuid_ignored(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2A00));
    assert(f.state == GS_READING);
    assert(f.read_idx == 0);

    /* Correct completion for read 0 (0x2A6E). */
    assert(gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11\x22", 2)).kind == GA_READ);
    assert(f.read_idx == 1);

    /* Duplicate completion for the read that already finished (0x2A6E
     * again, not the 0x2A6F now expected) -- ignored, does not advance. */
    gatt_act_t dup = gatt_fsm_step(&f, &EV_READ(0x2A6E, "\xFF\xFF", 2));
    assert(dup.kind == GA_NONE);
    assert(f.state == GS_READING);
    assert(f.read_idx == 1);

    /* A completion carrying a uuid16 that isn't in this plan at all is
     * equally ignored. */
    gatt_act_t bogus = gatt_fsm_step(&f, &EV_READ(0x9999, "\xEE\xEE", 2));
    assert(bogus.kind == GA_NONE);
    assert(f.state == GS_READING);
    assert(f.read_idx == 1);

    /* The correct next read still completes normally afterwards -- the
     * mismatches above corrupted neither read_idx nor the buffer. */
    gatt_act_t decode = gatt_fsm_step(&f, &EV_READ(0x2A6F, "\x33\x44", 2));
    assert(decode.kind == GA_DECODE);
    assert(f.state == GS_DECODING);
    assert(memcmp(decode.data, "\x11\x22", 2) == 0);       /* untouched by the 0x2A6E duplicate */
    assert(memcmp(decode.data + 16, "\x33\x44", 2) == 0);
}

/* A WRITE_OK's handle must match the write currently expected -- mirrors
 * test_read_ok_wrong_uuid_ignored exactly, for the write path the
 * coordinator's fix-round-2 ruling extended the same check to (round 1
 * only required it for reads; round 2 found the write path skips a real
 * write silently, the same wrong-value hazard one event kind over -- a
 * device left unwoken or in its default mode still answers the reads
 * that follow with plausible bytes). Needs PLAN_2READ_2WRITE so a
 * duplicate for write 0 can be delivered while write 1 is still
 * outstanding -- PLAN_2READ_1WRITE's single write can't exercise that. */
static void test_write_ok_wrong_uuid_ignored(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_2READ_2WRITE, sizeof PLAN_2READ_2WRITE);
    gatt_fsm_step(&f, &EV(GE_START));
    assert(gatt_fsm_step(&f, &EV(GE_CONNECTED)).kind == GA_WRITE);
    assert(f.state == GS_WRITING);
    assert(f.write_idx == 0);

    /* Correct completion for write 0 (0x2A00). */
    gatt_act_t w1 = gatt_fsm_step(&f, &EV_WRITE(0x2A00));
    assert(w1.kind == GA_WRITE);
    assert(w1.uuid16 == 0x2A01);
    assert(f.write_idx == 1);

    /* Duplicate completion for the write that already finished (0x2A00
     * again, not the 0x2A01 now expected, with write 1 still outstanding)
     * -- ignored, does not advance. */
    gatt_act_t dup = gatt_fsm_step(&f, &EV_WRITE(0x2A00));
    assert(dup.kind == GA_NONE);
    assert(f.state == GS_WRITING);
    assert(f.write_idx == 1);

    /* A completion carrying a uuid16 that isn't in this plan at all is
     * equally ignored. */
    gatt_act_t bogus = gatt_fsm_step(&f, &EV_WRITE(0x9999));
    assert(bogus.kind == GA_NONE);
    assert(f.state == GS_WRITING);
    assert(f.write_idx == 1);

    /* The correct next write still completes normally afterwards -- the
     * mismatches above did not corrupt write_idx, and the sequence
     * proceeds into reading exactly as it would have without them. A
     * guard written too strictly would break this assertion, which is
     * why it matters as much as the mismatch ones above: rejecting every
     * legitimate write completion would be worse than the bug it
     * replaces. */
    gatt_act_t r = gatt_fsm_step(&f, &EV_WRITE(0x2A01));
    assert(r.kind == GA_READ);
    assert(f.state == GS_READING);
    assert(r.uuid16 == 0x2A6E);
}

/* Every path from GS_CONNECTING to a terminal state must close the link
 * exactly once (coordinator's fix-round-1 ruling): the success path used
 * to be the exception, with zero GA_DISCONNECT actions and teardown left
 * to the caller's own discretion. It no longer is. The one deliberate
 * exception is an unsolicited disconnect, where the link is already down
 * and GA_REPORT_FAIL substitutes for GA_DISCONNECT (kept as-is per the
 * same ruling) -- so that path is checked separately, for exactly one
 * GA_REPORT_FAIL and zero GA_DISCONNECT, rather than folded into the
 * same assertion. */
static void run_and_count(gatt_fsm_t *f, const gatt_ev_t *seq, size_t n,
                           int *disconnects, int *reports)
{
    *disconnects = 0;
    *reports = 0;
    for (size_t i = 0; i < n; i++) {
        gatt_act_t a = gatt_fsm_step(f, &seq[i]);
        if (a.kind == GA_DISCONNECT)  (*disconnects)++;
        if (a.kind == GA_REPORT_FAIL) (*reports)++;
    }
}

static void test_exactly_one_disconnect_per_terminal_path(void)
{
    gatt_fsm_t f;
    int nd, nr;

    /* success */
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);
    gatt_ev_t success_seq[] = {
        EV(GE_START), EV(GE_CONNECTED), EV_WRITE(0x2A00),
        EV_READ(0x2A6E, "\x11\x22", 2), EV_READ(0x2A6F, "\x33\x44", 2), EV(GE_DECODED),
    };
    run_and_count(&f, success_seq, sizeof success_seq / sizeof success_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0);
    assert(f.state == GS_DONE);

    /* read error (no writes -- straight to GS_READING) */
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ);
    gatt_ev_t read_err_seq[] = { EV(GE_START), EV(GE_CONNECTED), EV(GE_ERROR) };
    run_and_count(&f, read_err_seq, sizeof read_err_seq / sizeof read_err_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0);
    assert(f.state == GS_FAILED);

    /* write error */
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);
    gatt_ev_t write_err_seq[] = { EV(GE_START), EV(GE_CONNECTED), EV(GE_ERROR) };
    run_and_count(&f, write_err_seq, sizeof write_err_seq / sizeof write_err_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0);
    assert(f.state == GS_FAILED);

    /* timeout in each open-connection state, GS_DECODING included */
    for (size_t i = 0; i < N_OPEN_CONNECTION_STATES; i++) {
        drive_to_state(&f, OPEN_CONNECTION_STATES[i]);
        gatt_act_t a = gatt_fsm_step(&f, &EV(GE_TIMEOUT));
        assert(a.kind == GA_DISCONNECT);
        assert(f.state == GS_FAILED);
    }

    /* unsolicited disconnect: the deliberate exception -- zero
     * GA_DISCONNECT, one GA_REPORT_FAIL, in every open-connection state. */
    for (size_t i = 0; i < N_OPEN_CONNECTION_STATES; i++) {
        drive_to_state(&f, OPEN_CONNECTION_STATES[i]);
        gatt_act_t a = gatt_fsm_step(&f, &EV(GE_DISCONNECTED));
        assert(a.kind == GA_REPORT_FAIL);
        assert(f.state == GS_FAILED);
    }
}

/* Events that cannot happen in the current state are ignored, not fatal --
 * a duplicate callback from the stack must not corrupt a sequence in
 * flight. */
static void test_unexpected_event_ignored(void)
{
    /* Nothing has started yet: a stray completion callback before GE_START
     * is inert. */
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ);
    assert(gatt_fsm_step(&f, &EV(GE_READ_OK)).kind == GA_NONE);
    assert(f.state == GS_IDLE);

    /* A duplicate GE_WRITE_OK delivered after the single write already
     * completed (state has moved on to GS_READING) must not be mistaken
     * for a read completion -- it does not match GS_READING's expected
     * event kind, so it is ignored and read_idx is untouched. */
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2A00));
    assert(f.state == GS_READING);
    assert(gatt_fsm_step(&f, &EV(GE_WRITE_OK)).kind == GA_NONE);
    assert(f.state == GS_READING);
    assert(f.read_idx == 0);

    /* Once terminal (GS_DONE), every further event is ignored, including
     * one of a kind that used to matter. */
    gatt_fsm_step(&f, &EV_READ(0x2A6E, "\x11\x22", 2));
    gatt_fsm_step(&f, &EV_READ(0x2A6F, "\x33\x44", 2));
    assert(f.state == GS_DECODING);
    gatt_fsm_step(&f, &EV(GE_DECODED));
    assert(f.state == GS_DONE);
    assert(gatt_fsm_step(&f, &EV(GE_READ_OK)).kind == GA_NONE);
    assert(f.state == GS_DONE);
}

/* ---------------- M5b Task 8: command mode ----------------
 *
 * The bytes below are a ONE-ENTRY action section: the u8 action_count
 * psvm.h's PSVM_FLAG_ACTION_TABLE layout begins with, followed by that one
 * entry, byte for byte as the blob carries it (see
 * gatt_fsm_init_command()'s own doc comment for why the count byte is
 * still there when only one entry is ever executed).
 *
 * This is `action irrigation.open(duration_s max 300) / write 2AF0 = 01
 * u16le(duration_s)` in the exact bytes the browser compiler emits for it
 * and psvm_validate() accepts: write_len is "constant prefix + parameter
 * width" (3), with zero PLACEHOLDER bytes reserved for the parameter, and
 * param_offset addresses a position inside that length. Fix round 1: the
 * fixture originally declared write_len 1 and relied on the parameter
 * extending the payload to 3 -- a shape psvm_validate() rejects, so no
 * wrapper could ever produce it, and the tests that used it were pinning
 * behaviour for bytes that cannot reach this code. The extension is now a
 * refusal (test_param_past_declared_write_len_refused below). */
static const uint8_t ACT_OPEN_WITH_CONFIRM[] = {
    1,                          /* action_count */
    2,                          /* ACT_IRRIGATION_OPEN */
    0x2C, 0x01,                 /* param_max 300 */
    0x03,                       /* flags: device-local | has confirm */
    0xF0, 0x2A,                 /* write uuid 0x2AF0 */
    3, 0x01, 0x00, 0x00,        /* write_len 3: opcode 01, then the u16 placeholder */
    1, 1,                       /* param_offset 1, encoding u16le */
    0xF1, 0x2A,                 /* confirm uuid 0x2AF1 */
    1,                          /* confirm_min_len */
    0, 0, 0,                    /* offset 0, encoding u8, op == */
    1, 0,                       /* confirm_value 1 */
};

static void test_command_sequence(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind == GA_CONNECT);
    gatt_act_t w = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(w.kind == GA_WRITE && w.uuid16 == 0x2AF0);
    /* The parameter is spliced at offset 1, little-endian: 8 -> 08 00 */
    assert(w.len == 3 && w.data[0] == 0x01 && w.data[1] == 0x08 && w.data[2] == 0x00);
    gatt_act_t r = gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    assert(r.kind == GA_READ && r.uuid16 == 0x2AF1);
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x01", 1)).kind == GA_DISCONNECT);
    assert(f.state == GS_DONE);
}

/* A confirm read that fails `require` is FAILED, which is louder than
 * unconfirmed -- the device answered and said no. */
static void test_confirm_require_unsatisfied_fails(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x00", 1)).kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED);
    assert(f.fail == GF_CONFIRM_FAILED);
}

/* A confirm read shorter than confirm_min_len is a short read, reusing
 * M5a's rule rather than inventing a second one. */
static void test_confirm_short_read_fails(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, NULL, 0)).kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED && f.fail == GF_SHORT_READ);
}

/* No confirm block: the write alone completes the command, and it is
 * recorded as unconfirmed rather than as a success. */
static void test_no_confirm_completes_unconfirmed(void) {
    static const uint8_t noconf[] = { 1, 1, 0,0, 0x00, 0xF0,0x2A, 1,0x00, 0xFF,0xFF };
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, noconf, sizeof noconf, 0);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(gatt_fsm_step(&f, &EV_WRITE(0x2AF0)).kind == GA_DISCONNECT);
    assert(f.state == GS_DONE);
    assert(f.confirmed == false);
}

/* The parameter overwrites the placeholder bytes and nothing else: the
 * payload stays exactly write_len long, and a parameter at the top of the
 * declared range lands whole. */
static void test_param_splice_over_placeholder_bytes(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 300);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_act_t w = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(w.kind == GA_WRITE && w.len == 3);
    assert(w.data[0] == 0x01 && w.data[1] == 0x2C && w.data[2] == 0x01);
}

/* A parameter that does not fit inside the entry's OWN declared write_len
 * is refused, never accommodated by growing the payload (fix round 1,
 * Critical 1). psvm_validate() rejects this shape, so accepting it here
 * would give one on-blob format two authorities -- and it would put bytes
 * on the air no author wrote: the second entry below declares one byte and
 * would have produced eight, five of them invented zeros. */
static void test_param_past_declared_write_len_refused(void) {
    /* The exact shape the task brief's original fixture used: write_len 1
     * with a u16le parameter at offset 1. */
    static const uint8_t short_len[] = {
        1, 2, 0x2C,0x01, 0x03, 0xF0,0x2A, 1,0x01, 1,1,
        0xF1,0x2A, 1, 0,0,0, 1,0,
    };
    /* And the version that made it a Critical rather than a curiosity. */
    static const uint8_t far_past[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 1,0x01, 6,2 };
    gatt_fsm_t f;

    gatt_fsm_init_command(&f, short_len, sizeof short_len, 8);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind == GA_NONE);

    gatt_fsm_init_command(&f, far_past, sizeof far_past, 8);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind == GA_NONE);

    /* The boundary is inclusive: a parameter ending exactly at write_len
     * is the normal, compiler-produced case and must still splice. */
    static const uint8_t exact[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 3,0x01,0x00,0x00, 1,1 };
    gatt_fsm_init_command(&f, exact, sizeof exact, 8);
    assert(f.state == GS_IDLE);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_act_t w = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(w.kind == GA_WRITE && w.len == 3);
    assert(w.data[0] == 0x01 && w.data[1] == 0x08 && w.data[2] == 0x00);
}

/* Both other encodings, and the "no parameter" sentinel pair: a u8
 * parameter is one byte, a u16be is the SAME value in the other byte
 * order (getting this backwards writes a 2048-second irrigation where 8
 * was asked for, which is precisely the class of silent wrong value this
 * project keeps eliminating). */
static void test_param_encodings(void) {
    static const uint8_t u8ent[] = {
        1, 3, 0xFF, 0x00, 0x00, 0xF0, 0x2A, 2, 0x0A, 0x00, 1, 0,
    };
    static const uint8_t bent[] = {
        1, 2, 0x2C, 0x01, 0x00, 0xF0, 0x2A, 3, 0x0A, 0x00, 0x00, 1, 2,
    };
    gatt_fsm_t f;

    gatt_fsm_init_command(&f, u8ent, sizeof u8ent, 0x42);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_act_t w = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(w.kind == GA_WRITE && w.len == 2 && w.data[0] == 0x0A && w.data[1] == 0x42);

    gatt_fsm_init_command(&f, bent, sizeof bent, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    w = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(w.kind == GA_WRITE && w.len == 3);
    assert(w.data[0] == 0x0A && w.data[1] == 0x00 && w.data[2] == 0x08);
}

/* A parameter above the entry's own declared bound is REFUSED, not
 * clamped and not spliced. actor_table_check() already bounds it at the
 * queue's door, so reaching here means something upstream is wrong -- and
 * the thing being asked for is a valve held open longer than its wrapper
 * says is safe. */
static void test_param_over_declared_max_refused(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 301);
    assert(f.state == GS_FAILED && f.fail == GF_PARAM_OVER_MAX);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind == GA_NONE);
    assert(f.state == GS_FAILED);

    /* The bound itself is inclusive. */
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 300);
    assert(f.state == GS_IDLE);
}

/* Every truncation of a valid entry must refuse the command outright --
 * never connect, never write a half-parsed payload -- and must never read
 * past the buffer it was given (run this under a sanitizer to see the
 * second half of that claim). */
static void test_truncated_entry_refused(void) {
    gatt_fsm_t f;
    for (size_t cut = 1; cut < sizeof ACT_OPEN_WITH_CONFIRM; cut++) {
        /* An exact-size heap copy, not a shorter length over the full
         * static array: reading one byte past a static array is invisible
         * to every tool, while reading one byte past a malloc'd block of
         * exactly `cut` bytes is what a sanitizer run can actually catch. */
        uint8_t *cut_copy = malloc(cut);
        assert(cut_copy != NULL);
        memcpy(cut_copy, ACT_OPEN_WITH_CONFIRM, cut);
        gatt_fsm_init_command(&f, cut_copy, (uint16_t)cut, 8);
        assert(f.state == GS_FAILED);
        assert(f.fail == GF_BAD_ACTION);
        assert(gatt_fsm_step(&f, &EV(GE_START)).kind == GA_NONE);
        free(cut_copy);
    }
    gatt_fsm_init_command(&f, NULL, 0, 0);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);
}

/* Hostile entries psvm_validate() would have refused, reaching this
 * parser anyway (its contract, inherited from gatt_fsm_init(), is to
 * survive a plan it did not validate itself). */
static void test_hostile_entry_refused(void) {
    gatt_fsm_t f;
    /* param_offset 250 + a u16 runs past both write_len and any buffer */
    static const uint8_t far_off[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 1,0x01, 250, 1 };
    gatt_fsm_init_command(&f, far_off, sizeof far_off, 8);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);

    /* write_len 0, and write_len above GATT_FSM_WRITE_MAX */
    static const uint8_t zero_len[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 0, 0xFF,0xFF };
    gatt_fsm_init_command(&f, zero_len, sizeof zero_len, 0);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);
    static const uint8_t huge_len[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 9,
                                        1,2,3,4,5,6,7,8,9, 0xFF,0xFF };
    gatt_fsm_init_command(&f, huge_len, sizeof huge_len, 0);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);

    /* param_offset/param_encoding must be a MATCHED pair */
    static const uint8_t half_pair[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 2,0x01,0x00, 0x00,0xFF };
    gatt_fsm_init_command(&f, half_pair, sizeof half_pair, 8);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);

    /* an encoding outside {0,1,2} */
    static const uint8_t bad_enc[] = { 1, 2, 0x2C,0x01, 0x00, 0xF0,0x2A, 2,0x01,0x00, 0x00,0x07 };
    gatt_fsm_init_command(&f, bad_enc, sizeof bad_enc, 8);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);

    /* action_count 0: there is no entry to execute */
    static const uint8_t no_entries[] = { 0 };
    gatt_fsm_init_command(&f, no_entries, sizeof no_entries, 0);
    assert(f.state == GS_FAILED && f.fail == GF_BAD_ACTION);
}

/* confirm_min_len says "1", but the require addresses offset 3 -- so a
 * peer answering with one byte satisfies the declared minimum and still
 * leaves the compared byte unread. psvm_validate() does NOT check
 * confirm_offset against confirm_min_len, so this reaches us: comparing
 * against the zero-padded slot would confirm (or fail) an action on a
 * byte the device never sent. */
static void test_confirm_offset_past_the_answer_is_a_short_read(void) {
    static const uint8_t deep[] = {
        1, 2, 0x2C,0x01, 0x02, 0xF0,0x2A, 1,0x01, 0xFF,0xFF,
        0xF1, 0x2A, 1, 3, 0, 0, 0, 0,     /* confirm offset 3, u8, == 0 */
    };
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, deep, sizeof deep, 0);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x00", 1)).kind == GA_DISCONNECT);
    assert(f.state == GS_FAILED && f.fail == GF_SHORT_READ);
    assert(f.confirmed == false);
    assert(f.cmd_state_valid == false);
}

/* `!=` is the other confirm op. */
static void test_confirm_op_ne(void) {
    static const uint8_t ne[] = {
        1, 1, 0,0, 0x02, 0xF0,0x2A, 1,0x00, 0xFF,0xFF,
        0xF1, 0x2A, 1, 0, 0, 1, 0, 0,     /* require u8(st,0) != 0 */
    };
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ne, sizeof ne, 0);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x01", 1)).kind == GA_DISCONNECT);
    assert(f.state == GS_DONE && f.confirmed == true);

    gatt_fsm_init_command(&f, ne, sizeof ne, 0);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x00", 1));
    assert(f.state == GS_FAILED && f.fail == GF_CONFIRM_FAILED);
}

/* The confirm read's decoded value is what the engine stores into
 * switch.state, so it is carried out of the state machine WHETHER OR NOT
 * the require was satisfied: a device that answers "still closed" after an
 * open is telling the truth about its state, and that truth is the more
 * important half of the report. */
static void test_confirm_value_is_reported_either_way(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x01", 1));
    assert(f.confirmed == true && f.cmd_state_valid == true && f.cmd_state == 1);

    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
    gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x00", 1));
    assert(f.confirmed == false && f.cmd_state_valid == true && f.cmd_state == 0);
}

/* Command mode inherits M5a's identity checks unchanged: a completion for
 * a characteristic other than the one currently awaited is ignored, not
 * mistaken for the next step's. For a command that matters more than for a
 * read -- a skipped write is a valve that never moved, reported as
 * confirmed. */
static void test_command_wrong_uuid_completions_ignored(void) {
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(gatt_fsm_step(&f, &EV_WRITE(0x2AF1)).kind == GA_NONE);
    assert(f.state == GS_WRITING);
    assert(gatt_fsm_step(&f, &EV_WRITE(0x2AF0)).kind == GA_READ);
    /* a duplicate of the write just completed, now in GS_READING */
    assert(gatt_fsm_step(&f, &EV_WRITE(0x2AF0)).kind == GA_NONE);
    assert(f.state == GS_READING);
    /* a read completion for the write characteristic */
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF0, "\x01", 1)).kind == GA_NONE);
    assert(f.state == GS_READING);
    assert(gatt_fsm_step(&f, &EV_READ(0x2AF1, "\x01", 1)).kind == GA_DISCONNECT);
    assert(f.state == GS_DONE && f.confirmed == true);
}

/* The same "exactly one GA_DISCONNECT per terminal path" invariant
 * gatt_fsm.h states, driven over the command sequence rather than the read
 * one -- a command that ends without tearing the link down holds the radio
 * and the hub goes deaf, exactly as M5a's own version of this test
 * guards. */
static void test_command_exactly_one_disconnect_per_terminal_path(void)
{
    gatt_fsm_t f;
    int nd, nr;

    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_ev_t ok_seq[] = {
        EV(GE_START), EV(GE_CONNECTED), EV_WRITE(0x2AF0), EV_READ(0x2AF1, "\x01", 1),
    };
    run_and_count(&f, ok_seq, sizeof ok_seq / sizeof ok_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0 && f.state == GS_DONE);

    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_ev_t nak_seq[] = {
        EV(GE_START), EV(GE_CONNECTED), EV_WRITE(0x2AF0), EV_READ(0x2AF1, "\x00", 1),
    };
    run_and_count(&f, nak_seq, sizeof nak_seq / sizeof nak_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0 && f.state == GS_FAILED);

    static const uint8_t noconf[] = { 1, 1, 0,0, 0x00, 0xF0,0x2A, 1,0x00, 0xFF,0xFF };
    gatt_fsm_init_command(&f, noconf, sizeof noconf, 0);
    gatt_ev_t unconf_seq[] = { EV(GE_START), EV(GE_CONNECTED), EV_WRITE(0x2AF0) };
    run_and_count(&f, unconf_seq, sizeof unconf_seq / sizeof unconf_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0 && f.state == GS_DONE);

    /* GE_TIMEOUT and GE_ERROR in each state that has a connection open,
     * then the unsolicited-disconnect exception (one GA_REPORT_FAIL, no
     * GA_DISCONNECT), driven over the command path's own states. */
    const gatt_ev_kind_t enders[] = { GE_TIMEOUT, GE_ERROR, GE_DISCONNECTED };
    for (size_t e = 0; e < sizeof enders / sizeof enders[0]; e++) {
        for (int depth = 0; depth < 3; depth++) {
            gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
            gatt_fsm_step(&f, &EV(GE_START));
            if (depth >= 1) gatt_fsm_step(&f, &EV(GE_CONNECTED));
            if (depth >= 2) gatt_fsm_step(&f, &EV_WRITE(0x2AF0));
            gatt_act_t a = gatt_fsm_step(&f, &EV(enders[e]));
            assert(a.kind == (enders[e] == GE_DISCONNECTED ? GA_REPORT_FAIL : GA_DISCONNECT));
            assert(f.state == GS_FAILED);
            assert(f.confirmed == false);
        }
    }
}

/* A command never runs a wrapper decode: GA_DECODE and GS_DECODING belong
 * to the read path alone. The engine hands GA_DECODE to the wrapper VM on
 * another task, and a command has no decode block, no read buffer worth
 * decoding and nothing to emit. */
static void test_command_never_decodes(void)
{
    gatt_fsm_t f;
    gatt_fsm_init_command(&f, ACT_OPEN_WITH_CONFIRM, sizeof ACT_OPEN_WITH_CONFIRM, 8);
    gatt_ev_t seq[] = {
        EV(GE_START), EV(GE_CONNECTED), EV_WRITE(0x2AF0), EV_READ(0x2AF1, "\x01", 1),
    };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        assert(gatt_fsm_step(&f, &seq[i]).kind != GA_DECODE);
        assert(f.state != GS_DECODING);
    }
    /* and a stray GE_DECODED cannot revive a finished command */
    assert(gatt_fsm_step(&f, &EV(GE_DECODED)).kind == GA_NONE);
    assert(f.state == GS_DONE);
}

int main(void)
{
    test_full_sequence();
    test_connect_goes_straight_to_reading_with_no_writes();
    test_read_error_fails_without_decoding();
    test_short_read_zero_pads();
    test_timeout_in_each_state();
    test_unsolicited_disconnect();
    test_unexpected_event_ignored();
    test_read_ok_wrong_uuid_ignored();
    test_write_ok_wrong_uuid_ignored();
    test_exactly_one_disconnect_per_terminal_path();
    test_short_read_fails();
    test_empty_read_fails();
    test_exact_min_len_read_ok();

    test_command_sequence();
    test_confirm_require_unsatisfied_fails();
    test_confirm_short_read_fails();
    test_no_confirm_completes_unconfirmed();
    test_param_splice_over_placeholder_bytes();
    test_param_past_declared_write_len_refused();
    test_param_encodings();
    test_param_over_declared_max_refused();
    test_truncated_entry_refused();
    test_hostile_entry_refused();
    test_confirm_offset_past_the_answer_is_a_short_read();
    test_confirm_op_ne();
    test_confirm_value_is_reported_either_way();
    test_command_wrong_uuid_completions_ignored();
    test_command_exactly_one_disconnect_per_terminal_path();
    test_command_never_decodes();

    printf("test_gatt_fsm: OK\n");
    return 0;
}
