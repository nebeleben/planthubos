/* tests/host/test_pending_close.c -- M5b Task 9: pending close persistence
 * and boot replay. Pure C99 only (no ESP-IDF): pending_close.c gates every
 * LittleFS/esp_timer/actor_request/alert_post call behind #ifdef ESP_PLATFORM,
 * exactly like actor.c already does, so this links and runs with plain cc.
 *
 * The brief's three tests (test_round_trip, test_truncated_file_yields_nothing,
 * test_all_due_after_boot) are reproduced verbatim first, then extended with
 * the RAM-table half of the module (arm/clear/due/retry) the brief's
 * "think past the tests" instruction asks for. */
#include "actor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Brief's tests, verbatim.
 * --------------------------------------------------------------------- */

/* A round trip must preserve every field -- this file is what stands between
 * a reboot and an actuator left open. */
static void test_round_trip(void) {
    pending_close_t in[2] = {
        { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 4242, .retries = 0 },
        { .dev_idx = 3, .close_action = ACT_SWITCH_OFF, .deadline_s = 99,   .retries = 2 },
    };
    uint8_t buf[64];
    size_t n = pending_close_serialize(in, 2, buf, sizeof buf);
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 2);
    assert(out[0].dev_idx == 1 && out[0].deadline_s == 4242);
    assert(out[1].retries == 2);
}

/* A truncated or corrupt file must yield NOTHING rather than a plausible
 * wrong close -- closing a device we were never told about is its own bug. */
static void test_truncated_file_yields_nothing(void) {
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 };
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    pending_close_t out[4];
    for (size_t cut = 1; cut < n; cut++)
        assert(pending_close_deserialize(buf, n - cut, out, 4) == 0);
}

/* On boot every record is due immediately, whatever its deadline said --
 * we cannot know how long we were off. */
static void test_all_due_after_boot(void) {
    pending_close_t out[4];
    uint8_t buf[64];
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 999999 };
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    assert(pending_close_deserialize(buf, n, out, 4) == 1);
    assert(pending_close_is_boot_due(&out[0]));
}

/* ---------------------------------------------------------------------
 * Additional serialisation coverage -- corruption forms the brief's sweep
 * doesn't reach (bad fmt byte, bad crc, oversubscribed cap, empty table).
 * --------------------------------------------------------------------- */

static void test_serialize_zero_records(void) {
    uint8_t buf[8];
    size_t n = pending_close_serialize(NULL, 0, buf, sizeof buf);
    assert(n == 4); /* header only: fmt, count=0, 2-byte crc */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

static void test_serialize_buffer_too_small_writes_nothing(void) {
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 };
    uint8_t buf[4]; /* header alone; no room for one 7-byte record */
    assert(pending_close_serialize(&in, 1, buf, sizeof buf) == 0);
}

static void test_deserialize_wrong_fmt_yields_nothing(void) {
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 };
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    buf[0] = 2; /* an fmt this reader has never written */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

static void test_deserialize_bad_crc_yields_nothing(void) {
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 };
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    buf[n - 1] ^= 0xFFu; /* flip a bit inside the last record's byte */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

static void test_deserialize_cap_too_small_yields_nothing(void) {
    pending_close_t in[2] = {
        { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 },
        { .dev_idx = 2, .close_action = ACT_SWITCH_OFF, .deadline_s = 20 },
    };
    uint8_t buf[64];
    size_t n = pending_close_serialize(in, 2, buf, sizeof buf);
    pending_close_t out[1];
    /* Rejected outright rather than silently truncated to 1 -- a caller
     * that asked for a specific capacity gets nothing, not a partial and
     * possibly misleading list. */
    assert(pending_close_deserialize(buf, n, out, 1) == 0);
}

static void test_deserialize_trailing_garbage_yields_nothing(void) {
    pending_close_t in = { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 10 };
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    buf[n] = 0xAA; /* one byte past the declared record -- not truncation */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n + 1, out, 4) == 0);
}

/* ---------------------------------------------------------------------
 * The RAM table: arm/clear/due/retry. Every test resets state via
 * pending_close_init() first -- the table is module-static (matching
 * actor.c's own s_table/s_queue), so tests must not depend on ordering.
 * --------------------------------------------------------------------- */

static void test_arm_then_due(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(50, &out) == 0);   /* not yet */
    assert(pending_close_due(100, &out) == 1);  /* inclusive boundary */
    assert(out.dev_idx == 1 && out.close_action == ACT_SWITCH_OFF && out.retries == 0);
    assert(pending_close_due(200, &out) == 1);  /* still due once past deadline */
}

static void test_clear_removes(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    pending_close_clear(1);
    pending_close_t out;
    assert(pending_close_due(1000, &out) == 0);
    assert(pending_close_active_count() == 0);
}

static void test_clear_unknown_device_is_a_safe_no_op(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    pending_close_clear(2); /* never armed */
    assert(pending_close_active_count() == 1);
}

static void test_two_devices_independent(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    pending_close_arm(2, ACT_SWITCH_OFF, 200);
    assert(pending_close_active_count() == 2);

    pending_close_clear(1);
    assert(pending_close_active_count() == 1);

    pending_close_t out;
    /* Only device 2 can still be due; device 1's obligation is gone. */
    assert(pending_close_due(1000, &out) == 1);
    assert(out.dev_idx == 2);
}

static void test_due_picks_earliest_when_multiple_due(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 200);
    pending_close_arm(2, ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(1000, &out) == 1);
    assert(out.dev_idx == 2); /* the earlier deadline, not arrival order */
}

/* A device that opens again while its close is already pending must have
 * the obligation REPLACED, not duplicated -- and the fresh open resets any
 * retry count a previous, unrelated attempt had accumulated. */
static void test_rearm_replaces_and_resets_retries(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    assert(pending_close_retry(1, 100));
    assert(pending_close_retry(1, 100));
    pending_close_t out;
    assert(pending_close_due(100, &out) == 0); /* pushed into the future by retry() */

    pending_close_arm(1, ACT_SWITCH_OFF, 500); /* device opened again */
    assert(pending_close_active_count() == 1); /* still one row, not two */
    assert(pending_close_due(500, &out) == 1);
    assert(out.deadline_s == 500 && out.retries == 0);
}

static void test_retry_backoff_increases_deadline(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(100, &out) == 1);

    assert(pending_close_retry(1, 100));
    assert(pending_close_due(100, &out) == 0); /* rescheduled forward */
    assert(pending_close_due(1000, &out) == 1);
    uint32_t first_deadline = out.deadline_s;
    assert(first_deadline > 100);

    assert(pending_close_retry(1, first_deadline));
    assert(pending_close_due(first_deadline, &out) == 0);
    assert(pending_close_due(100000, &out) == 1);
    assert(out.deadline_s > first_deadline); /* backoff, not a fixed step */
}

static void test_retry_is_bounded_and_exhausts(void) {
    pending_close_init();
    pending_close_arm(1, ACT_SWITCH_OFF, 0);
    uint32_t now = 0;
    int ok_count = 0;
    for (int i = 0; i < 100; i++) {
        if (pending_close_retry(1, now)) {
            ok_count++;
            now += 100000; /* always advance past the new backoff deadline */
        } else {
            break;
        }
    }
    assert(ok_count == PENDING_CLOSE_MAX_RETRIES); /* bounded, not infinite */
    /* One more call must keep refusing -- exhaustion is a stable state, not
     * a one-shot false. */
    assert(!pending_close_retry(1, now));
    /* The record itself is untouched by the refusal (still one row; a
     * caller decides whether/when to clear it after alerting). */
    assert(pending_close_active_count() == 1);
}

static void test_retry_unknown_device_returns_false(void) {
    pending_close_init();
    assert(!pending_close_retry(42, 0));
}

static void test_boot_load_ignores_stored_deadline(void) {
    pending_close_init();
    pending_close_t recs[1] = {
        { .dev_idx = 1, .close_action = ACT_SWITCH_OFF, .deadline_s = 999999, .retries = 3 },
    };
    pending_close_boot_load(recs, 1);
    pending_close_t out;
    /* Due at uptime 0, not 999999 -- exactly what pending_close_is_boot_due()
     * pins in isolation; this proves boot_load() actually uses it. */
    assert(pending_close_due(0, &out) == 1);
    assert(out.dev_idx == 1 && out.retries == 3);
}

static void test_boot_load_skips_invalid_records(void) {
    pending_close_init();
    pending_close_t recs[1] = { { .dev_idx = -1, .close_action = 0, .deadline_s = 0, .retries = 0 } };
    pending_close_boot_load(recs, 1);
    assert(pending_close_active_count() == 0);
}

/* A device index 0 (a real, valid index -- the free-slot sentinel is -1)
 * must not be mistaken for "no record" anywhere in the boot-due path. */
static void test_is_boot_due_true_for_device_zero(void) {
    pending_close_t r = { .dev_idx = 0, .close_action = ACT_SWITCH_OFF, .deadline_s = 12345, .retries = 0 };
    assert(pending_close_is_boot_due(&r));
}

static void test_is_boot_due_false_for_free_slot(void) {
    pending_close_t r = { .dev_idx = -1, .close_action = 0, .deadline_s = 0, .retries = 0 };
    assert(!pending_close_is_boot_due(&r));
}

/* The table caps at PENDING_CLOSE_MAX (== ACTOR_MAX_DEVICES): a fifth
 * distinct device is dropped, never overflowing the RAM table or
 * corrupting an existing row. */
static void test_table_full_drops_extra_without_corruption(void) {
    pending_close_init();
    for (int i = 0; i < PENDING_CLOSE_MAX; i++) pending_close_arm(i, ACT_SWITCH_OFF, (uint32_t)(100 + i));
    assert(pending_close_active_count() == PENDING_CLOSE_MAX);

    pending_close_arm(100, ACT_SWITCH_OFF, 5); /* one too many */
    assert(pending_close_active_count() == PENDING_CLOSE_MAX);

    pending_close_t out;
    assert(pending_close_due(1000, &out) == 1);
    assert(out.dev_idx != 100); /* the dropped one never took a row */
}

int main(void) {
    test_round_trip();
    test_truncated_file_yields_nothing();
    test_all_due_after_boot();
    test_serialize_zero_records();
    test_serialize_buffer_too_small_writes_nothing();
    test_deserialize_wrong_fmt_yields_nothing();
    test_deserialize_bad_crc_yields_nothing();
    test_deserialize_cap_too_small_yields_nothing();
    test_deserialize_trailing_garbage_yields_nothing();
    test_arm_then_due();
    test_clear_removes();
    test_clear_unknown_device_is_a_safe_no_op();
    test_two_devices_independent();
    test_due_picks_earliest_when_multiple_due();
    test_rearm_replaces_and_resets_retries();
    test_retry_backoff_increases_deadline();
    test_retry_is_bounded_and_exhausts();
    test_retry_unknown_device_returns_false();
    test_boot_load_ignores_stored_deadline();
    test_boot_load_skips_invalid_records();
    test_is_boot_due_true_for_device_zero();
    test_is_boot_due_false_for_free_slot();
    test_table_full_drops_extra_without_corruption();
    printf("test_pending_close: OK\n");
    return 0;
}
