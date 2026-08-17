#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gatt_fsm.h"

/* Trailing PSBC connect-plan section bytes (psvm.h's PSVM_FLAG_CONNECT_PLAN
 * doc comment / components/psvm/psvm.c's parser): read_count, write_count,
 * interval_s (u32 LE), reads[] (u16 LE each), writes[] (u16 LE uuid, u8
 * len, u8 data[len]). This is the exact 14-byte shape the M5a spec's own
 * `connect` block example compiles to (design spec section 2): one write
 * (0x2A00 = 01), two reads (0x2A6E temp, 0x2A6F hum). */
static const uint8_t PLAN_2READ_1WRITE[] = {
    2, 1,                       /* read_count=2, write_count=1 */
    0x58, 0x02, 0x00, 0x00,     /* interval_s = 600 */
    0x6E, 0x2A,                 /* read 0x2A6E */
    0x6F, 0x2A,                 /* read 0x2A6F */
    0x00, 0x2A, 1, 0x01,        /* write 0x2A00 = 01 */
};

static const uint8_t PLAN_1READ[] = {
    1, 0,                       /* read_count=1, write_count=0 */
    0x2C, 0x01, 0x00, 0x00,     /* interval_s = 300 */
    0x6E, 0x2A,                 /* read 0x2A6E */
};

#define EV(k) ((gatt_ev_t){ .kind = (k), .handle = 0, .data = NULL, .len = 0 })
#define EV_READ(u, d, l) \
    ((gatt_ev_t){ .kind = GE_READ_OK, .handle = (u), .data = (const uint8_t *)(d), .len = (l) })

/* Cold cache: the full sequence, in order. */
static void test_cold_path(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);

    assert(gatt_fsm_step(&f, &EV(GE_START)).kind      == GA_CONNECT);
    assert(f.state == GS_CONNECTING);
    assert(gatt_fsm_step(&f, &EV(GE_CONNECTED)).kind  == GA_DISCOVER);
    assert(f.state == GS_DISCOVERING);
    assert(gatt_fsm_step(&f, &EV(GE_DISCOVERED)).kind == GA_WRITE);
    assert(f.state == GS_WRITING);
    assert(gatt_fsm_step(&f, &EV(GE_WRITE_OK)).kind   == GA_READ);
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

/* Warm cache skips discovery entirely -- the whole point of the cache. */
static void test_warm_cache_skips_discovery(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ, true);
    assert(gatt_fsm_step(&f, &EV(GE_START)).kind     == GA_CONNECT);
    gatt_act_t a = gatt_fsm_step(&f, &EV(GE_CONNECTED));
    assert(a.kind == GA_READ);
    assert(a.uuid16 == 0x2A6E);
    assert(f.state == GS_READING);
}

/* A read failure must not decode, and must report so the caller can drop
 * the handle cache -- a stale handle is the likeliest cause. */
static void test_read_error_fails_without_decoding(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ, true);
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
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ, true);
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
 * Drives PLAN_2READ_1WRITE's cold path (2 reads, 1 write) up to but not
 * including `target`, so GS_DECODING is reachable too -- both reads must
 * actually land for that one, via the same EV_READ() uuids test_cold_path
 * uses. */
static void drive_to_state(gatt_fsm_t *f, gatt_state_t target)
{
    gatt_fsm_init(f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);
    gatt_fsm_step(f, &EV(GE_START));
    if (target == GS_CONNECTING) return;
    gatt_fsm_step(f, &EV(GE_CONNECTED));
    if (target == GS_DISCOVERING) return;
    gatt_fsm_step(f, &EV(GE_DISCOVERED));
    if (target == GS_WRITING) return;
    gatt_fsm_step(f, &EV(GE_WRITE_OK));
    if (target == GS_READING) return;
    gatt_fsm_step(f, &EV_READ(0x2A6E, "\x11\x22", 2));
    gatt_fsm_step(f, &EV_READ(0x2A6F, "\x33\x44", 2));
    /* target == GS_DECODING */
}

static const gatt_state_t OPEN_CONNECTION_STATES[] = {
    GS_CONNECTING, GS_DISCOVERING, GS_WRITING, GS_READING, GS_DECODING,
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
 * this module itself (see gatt_ev_t.handle's doc comment): Task 6's
 * adapter may filter using its own NimBLE callback context too, but it
 * must not be the only layer that does, because it is the one layer a
 * host test cannot reach. */
static void test_read_ok_wrong_uuid_ignored(void)
{
    gatt_fsm_t f;
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV(GE_DISCOVERED));
    gatt_fsm_step(&f, &EV(GE_WRITE_OK));
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
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);
    gatt_ev_t success_seq[] = {
        EV(GE_START), EV(GE_CONNECTED), EV(GE_DISCOVERED), EV(GE_WRITE_OK),
        EV_READ(0x2A6E, "\x11\x22", 2), EV_READ(0x2A6F, "\x33\x44", 2), EV(GE_DECODED),
    };
    run_and_count(&f, success_seq, sizeof success_seq / sizeof success_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0);
    assert(f.state == GS_DONE);

    /* read error (warm cache -- straight to GS_READING) */
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ, true);
    gatt_ev_t read_err_seq[] = { EV(GE_START), EV(GE_CONNECTED), EV(GE_ERROR) };
    run_and_count(&f, read_err_seq, sizeof read_err_seq / sizeof read_err_seq[0], &nd, &nr);
    assert(nd == 1 && nr == 0);
    assert(f.state == GS_FAILED);

    /* write error */
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);
    gatt_ev_t write_err_seq[] = { EV(GE_START), EV(GE_CONNECTED), EV(GE_DISCOVERED), EV(GE_ERROR) };
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
    gatt_fsm_init(&f, PLAN_1READ, sizeof PLAN_1READ, true);
    assert(gatt_fsm_step(&f, &EV(GE_READ_OK)).kind == GA_NONE);
    assert(f.state == GS_IDLE);

    /* A duplicate GE_WRITE_OK delivered after the single write already
     * completed (state has moved on to GS_READING) must not be mistaken
     * for a read completion -- it does not match GS_READING's expected
     * event kind, so it is ignored and read_idx is untouched. */
    gatt_fsm_init(&f, PLAN_2READ_1WRITE, sizeof PLAN_2READ_1WRITE, false);
    gatt_fsm_step(&f, &EV(GE_START));
    gatt_fsm_step(&f, &EV(GE_CONNECTED));
    gatt_fsm_step(&f, &EV(GE_DISCOVERED));
    gatt_fsm_step(&f, &EV(GE_WRITE_OK));
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

int main(void)
{
    test_cold_path();
    test_warm_cache_skips_discovery();
    test_read_error_fails_without_decoding();
    test_short_read_zero_pads();
    test_timeout_in_each_state();
    test_unsolicited_disconnect();
    test_unexpected_event_ignored();
    test_read_ok_wrong_uuid_ignored();
    test_exactly_one_disconnect_per_terminal_path();

    printf("test_gatt_fsm: OK\n");
    return 0;
}
