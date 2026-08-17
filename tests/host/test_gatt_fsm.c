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
    assert(f.state == GS_DONE);
    assert(decode.len == 32);          /* 2 reads x GATT_FSM_SLOT */
    assert(memcmp(decode.data, "\x11\x22", 2) == 0);
    assert(memcmp(decode.data + 16, "\x33\x44", 2) == 0);

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
    assert(decode.len == 16);
    assert(decode.data[0] == 0x11 && decode.data[1] == 0x22);
    for (int i = 2; i < 16; i++) assert(decode.data[i] == 0x00);
}

/* The deadline fires in every state and always ends in a disconnect. */
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
    /* target == GS_READING */
}

static void test_timeout_in_each_state(void)
{
    const gatt_state_t states[] = { GS_CONNECTING, GS_DISCOVERING, GS_WRITING, GS_READING };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        gatt_fsm_t f;
        drive_to_state(&f, states[i]);
        assert(f.state == states[i]);
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
    const gatt_state_t states[] = { GS_CONNECTING, GS_DISCOVERING, GS_WRITING, GS_READING };
    for (size_t i = 0; i < sizeof states / sizeof states[0]; i++) {
        gatt_fsm_t f;
        drive_to_state(&f, states[i]);
        assert(f.state == states[i]);
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

    printf("test_gatt_fsm: OK\n");
    return 0;
}
