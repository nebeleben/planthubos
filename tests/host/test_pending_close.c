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
 * Whole-branch review follow-up: records are keyed on the DEVICE's stable
 * identity (device_id_t bytes), never on its registry index -- the registry
 * is RAM-only and assigns indices in discovery order, so index N after a
 * reboot is whichever device advertised Nth. KEY(n) fabricates distinct,
 * realistically shaped identities (kind 1 = BLE, then a MAC); the tests
 * below use them exactly where they used to pass a bare index, so the
 * pre-existing coverage is preserved rather than rewritten.
 * --------------------------------------------------------------------- */
static uint8_t g_keys[128][PENDING_CLOSE_KEY_LEN];
static const uint8_t *KEY(unsigned n) {
    assert(n < 128);   /* byte 6 is 0xA0 + n, injective over this range */
    g_keys[n][0] = 1;                      /* DEV_KIND_BLE */
    g_keys[n][1] = 0xD0; g_keys[n][2] = 0xCF; g_keys[n][3] = 0x13;
    g_keys[n][4] = 0xE5; g_keys[n][5] = 0xB9;
    g_keys[n][6] = (uint8_t)(0xA0 + n);    /* the only differing byte */
    g_keys[n][7] = 0; g_keys[n][8] = 0;
    return g_keys[n];
}
static const uint8_t ZERO_KEY[PENDING_CLOSE_KEY_LEN] = { 0 };

/* `.dev_idx = N` in a record initialiser becomes `.key` set from KEY(N).
 * A small helper keeps the tests readable. */
static pending_close_t REC(unsigned n, uint8_t close_action,
                            uint32_t deadline_s, uint8_t retries) {
    pending_close_t r;
    memset(&r, 0, sizeof r);
    memcpy(r.key, KEY(n), PENDING_CLOSE_KEY_LEN);
    r.close_action = close_action;
    r.deadline_s = deadline_s;
    r.retries = retries;
    return r;
}
static bool REC_IS(const pending_close_t *r, unsigned n) {
    return memcmp(r->key, KEY(n), PENDING_CLOSE_KEY_LEN) == 0;
}

/* ---------------------------------------------------------------------
 * Brief's tests, verbatim.
 * --------------------------------------------------------------------- */

/* A round trip must preserve every field -- this file is what stands between
 * a reboot and an actuator left open. */
static void test_round_trip(void) {
    pending_close_t in[2] = {
        REC(1, ACT_SWITCH_OFF, 4242, 0),
        REC(3, ACT_SWITCH_OFF, 99, 2),
    };
    uint8_t buf[64];
    size_t n = pending_close_serialize(in, 2, buf, sizeof buf);
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 2);
    assert(REC_IS(&out[0], 1) && out[0].deadline_s == 4242);
    assert(out[1].retries == 2);
}

/* A truncated or corrupt file must yield NOTHING rather than a plausible
 * wrong close -- closing a device we were never told about is its own bug. */
static void test_truncated_file_yields_nothing(void) {
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 10, 0);
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
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 999999, 0);
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
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 10, 0);
    uint8_t buf[4]; /* header alone; no room for one 15-byte record */
    assert(pending_close_serialize(&in, 1, buf, sizeof buf) == 0);
}

static void test_deserialize_wrong_fmt_yields_nothing(void) {
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 10, 0);
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    buf[0] = 9; /* an fmt this reader has never written */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

/* Whole-branch review follow-up: a FORMAT 1 file -- the shape this firmware
 * itself wrote until the re-keying -- must be discarded, not reinterpreted.
 * Its 7-byte record began with an int8_t registry index; read as the head of
 * a device key that is a wrong close waiting to happen, and the cost of
 * discarding is one stale obligation on a single upgrade boot.
 *
 * Built here as literal bytes rather than through any current helper, so it
 * stays a fixture of the OLD format even as the current one moves on. */
static void test_format_1_file_is_discarded_not_reinterpreted(void) {
    /* { fmt=1, count=1, crc } + { dev_idx=1, close_action=1, deadline LE32, retries=0 } */
    uint8_t old_file[4 + 7] = {
        1, 1, 0x00, 0x00,
        0x01, ACT_SWITCH_OFF, 0x0A, 0x00, 0x00, 0x00, 0x00,
    };
    /* Give it a crc that is genuinely correct for its own format, so the
     * rejection is proved to come from the FORMAT byte and not from a
     * checksum that happened to fail. */
    uint16_t crc = 0xFFFFu;
    for (size_t i = 1; i < sizeof old_file; i++) {
        if (i == 2 || i == 3) continue;
        crc ^= (uint16_t)((uint16_t)old_file[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u)
                                              : (uint16_t)(crc << 1));
    }
    old_file[2] = (uint8_t)(crc & 0xFFu);
    old_file[3] = (uint8_t)(crc >> 8);

    pending_close_t out[4];
    assert(pending_close_deserialize(old_file, sizeof old_file, out, 4) == 0);

    /* And nothing of it reaches the table by any other route. */
    pending_close_init();
    pending_close_boot_load(out, 0);
    assert(pending_close_active_count() == 0);
}

/* A record whose identity is the all-zero sentinel could never be resolved
 * back to a device, so it is a corrupt row, not an obligation. */
static void test_deserialize_keyless_record_yields_nothing(void) {
    pending_close_t in;
    memset(&in, 0, sizeof in);
    in.close_action = ACT_SWITCH_OFF;
    in.deadline_s = 10;
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    assert(n > 0);
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

/* An identity is only ever matched WHOLE: two devices differing in a single
 * byte must never share a record. */
static void test_two_similar_keys_are_distinct_records(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_arm(KEY(2), ACT_SWITCH_OFF, 200);
    assert(pending_close_active_count() == 2);

    pending_close_clear(KEY(1));
    assert(pending_close_active_count() == 1);
    pending_close_t out;
    assert(pending_close_due(1000, &out) == 1);
    assert(REC_IS(&out, 2));

    /* An identity never armed clears nothing, and a NULL/all-zero one is a
     * safe no-op rather than a wildcard. */
    pending_close_clear(KEY(5));
    pending_close_clear(ZERO_KEY);
    pending_close_clear(NULL);
    assert(pending_close_active_count() == 1);
}

/* The reboot the whole re-keying exists for: a record is saved, the hub
 * restarts, and the device comes back at a DIFFERENT registry index. The
 * record must still be the same obligation -- and while the device has not
 * been seen at all, it must defer rather than resolve to anything. */
static void test_reboot_device_reappears_at_a_different_index(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);

    pending_close_t snap[PENDING_CLOSE_MAX];
    size_t n = pending_close_persist_snapshot(snap, PENDING_CLOSE_MAX);
    assert(n == 1);
    uint8_t buf[64];
    size_t len = pending_close_serialize(snap, n, buf, sizeof buf);
    assert(len > 0);

    /* ---- reboot ---- */
    pending_close_t restored[PENDING_CLOSE_MAX];
    assert(pending_close_deserialize(buf, len, restored, PENDING_CLOSE_MAX) == 1);
    assert(REC_IS(&restored[0], 1));   /* the IDENTITY survived, not an index */

    pending_close_init();
    pending_close_boot_load(restored, 1);

    pending_close_t out;
    assert(pending_close_due(0, &out) == 1);

    /* The device is not in the registry yet -- nothing resolves it, so the
     * caller passes device_known = false and the record DEFERS. It must not
     * consume a retry, and above all it must not be attempted against
     * whatever else exists. */
    assert(pending_close_step(KEY(1), 0, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
    assert(out.retries == 0);
    assert(pending_close_active_count() == 1);

    /* It advertises later and is declared -- at index 0 this boot rather
     * than whatever it was before, which the record never recorded and so
     * cannot get wrong. The caller resolves it, and the obligation
     * proceeds. */
    uint32_t now = out.deadline_s;
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, true, &out) == PENDING_CLOSE_STEP_REQUEST);
    assert(out.retries == 1);
    assert(REC_IS(&out, 1));

    /* The close is confirmed and the obligation ends -- cleared by
     * identity, which is what pending_close_note_result() passes. */
    pending_close_clear(KEY(1));
    assert(pending_close_active_count() == 0);
}

/* A device that never comes back must still break the silence exactly once,
 * keyed the new way -- the DEFERRED_TIMEOUT path is what stops an
 * unresolvable identity from being quietly forgotten. */
static void test_unresolvable_identity_still_reaches_the_deferred_timeout(void) {
    pending_close_init();
    pending_close_t restored[1] = { REC(4, ACT_SWITCH_OFF, 0, 0) };
    pending_close_boot_load(restored, 1);

    uint32_t now = 0;
    pending_close_t out;
    uint32_t threshold = PENDING_CLOSE_DEFERRED_ALERT_S / PENDING_CLOSE_UNKNOWN_RECHECK_S;
    for (uint32_t i = 0; i + 1 < threshold; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(4), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        now = out.deadline_s;
    }
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(4), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED_TIMEOUT);
    assert(out.retries == 0);
    assert(pending_close_active_count() == 1);
}

static void test_deserialize_bad_crc_yields_nothing(void) {
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 10, 0);
    uint8_t buf[64];
    size_t n = pending_close_serialize(&in, 1, buf, sizeof buf);
    buf[n - 1] ^= 0xFFu; /* flip a bit inside the last record's byte */
    pending_close_t out[4];
    assert(pending_close_deserialize(buf, n, out, 4) == 0);
}

static void test_deserialize_cap_too_small_yields_nothing(void) {
    pending_close_t in[2] = {
        REC(1, ACT_SWITCH_OFF, 10, 0),
        REC(2, ACT_SWITCH_OFF, 20, 0),
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
    pending_close_t in = REC(1, ACT_SWITCH_OFF, 10, 0);
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
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(50, &out) == 0);   /* not yet */
    assert(pending_close_due(100, &out) == 1);  /* inclusive boundary */
    assert(REC_IS(&out, 1) && out.close_action == ACT_SWITCH_OFF && out.retries == 0);
    assert(pending_close_due(200, &out) == 1);  /* still due once past deadline */
}

static void test_clear_removes(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_clear(KEY(1));
    pending_close_t out;
    assert(pending_close_due(1000, &out) == 0);
    assert(pending_close_active_count() == 0);
}

static void test_clear_unknown_device_is_a_safe_no_op(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_clear(KEY(2)); /* never armed */
    assert(pending_close_active_count() == 1);
}

static void test_two_devices_independent(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_arm(KEY(2), ACT_SWITCH_OFF, 200);
    assert(pending_close_active_count() == 2);

    pending_close_clear(KEY(1));
    assert(pending_close_active_count() == 1);

    pending_close_t out;
    /* Only device 2 can still be due; device 1's obligation is gone. */
    assert(pending_close_due(1000, &out) == 1);
    assert(REC_IS(&out, 2));
}

static void test_due_picks_earliest_when_multiple_due(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 200);
    pending_close_arm(KEY(2), ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(1000, &out) == 1);
    assert(REC_IS(&out, 2)); /* the earlier deadline, not arrival order */
}

/* A device that opens again while its close is already pending must have
 * the obligation REPLACED, not duplicated -- and the fresh open resets any
 * retry count a previous, unrelated attempt had accumulated. */
static void test_rearm_replaces_and_resets_retries(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_step(KEY(1), 100, true, &out) == PENDING_CLOSE_STEP_REQUEST);
    assert(pending_close_due(100, &out) == 0); /* pushed into the future by step() */

    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 500); /* device opened again */
    assert(pending_close_active_count() == 1); /* still one row, not two */
    assert(pending_close_due(500, &out) == 1);
    assert(out.deadline_s == 500 && out.retries == 0);
}

/* ---------------------------------------------------------------------
 * pending_close_step(): fix round 1 findings 2 and 5.
 * --------------------------------------------------------------------- */

static void test_step_known_device_backs_off(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(100, &out) == 1);

    assert(pending_close_step(KEY(1), 100, true, &out) == PENDING_CLOSE_STEP_REQUEST);
    assert(out.retries == 1);
    assert(pending_close_due(100, &out) == 0); /* rescheduled forward */
    assert(pending_close_due(1000, &out) == 1);
    uint32_t first_deadline = out.deadline_s;
    assert(first_deadline > 100);

    assert(pending_close_step(KEY(1), first_deadline, true, &out) == PENDING_CLOSE_STEP_REQUEST);
    assert(out.retries == 2);
    assert(pending_close_due(first_deadline, &out) == 0);
    assert(pending_close_due(100000, &out) == 1);
    assert(out.deadline_s > first_deadline); /* backoff, not a fixed step */
}

/* Fix round 1, finding 2's first ruling: an attempt against a device the
 * hub hasn't rediscovered yet must not spend the retry budget. */
static void test_step_unknown_device_does_not_consume_retry(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    pending_close_t out;
    assert(pending_close_due(100, &out) == 1);

    assert(pending_close_step(KEY(1), 100, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
    assert(out.retries == 0); /* NOT spent */
    assert(pending_close_due(100, &out) == 0); /* rescheduled a short delay ahead */
    assert(pending_close_due(100 + PENDING_CLOSE_UNKNOWN_RECHECK_S, &out) == 1);
    assert(out.retries == 0);

    /* A device that stays unknown forever can be deferred indefinitely
     * without ever spending a retry or reaching exhaustion -- the recorded
     * consequence of the ruling (pending_close_step()'s own doc comment). */
    uint32_t now = 100 + PENDING_CLOSE_UNKNOWN_RECHECK_S;
    for (int i = 0; i < 4 * PENDING_CLOSE_MAX_RETRIES; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        assert(out.retries == 0);
        now = out.deadline_s;
    }
}

/* Fix round 1, finding 5: exhaustion must be declared on a check that
 * follows the final real attempt's own backoff period -- never in the same
 * pass that issues that attempt, whose outcome is not yet known. */
static void test_step_exhaustion_follows_final_backoff_not_synchronous(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 0);
    uint32_t now = 0;
    pending_close_t out;

    for (int i = 0; i < PENDING_CLOSE_MAX_RETRIES; i++) {
        assert(pending_close_due(now, &out) == 1);
        pending_close_step_t st = pending_close_step(KEY(1), now, true, &out);
        /* Every one of the MAX_RETRIES real attempts is a REQUEST, never
         * EXHAUSTED -- in particular the LAST one: the alert must not fire
         * in the same call that just launched the final attempt. */
        assert(st == PENDING_CLOSE_STEP_REQUEST);
        assert(out.retries == (uint8_t)(i + 1));
        now = out.deadline_s; /* jump to exactly when it is due again */
    }

    /* Only NOW -- a full backoff period after the final attempt, with the
     * record still present (so that attempt did not succeed) -- is
     * exhaustion declared. */
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, true, &out) == PENDING_CLOSE_STEP_EXHAUSTED);
    assert(out.retries == PENDING_CLOSE_MAX_RETRIES);

    /* Given up for this boot: never due again... */
    assert(pending_close_due(now, &out) == 0);
    assert(pending_close_due(0xFFFFFFF0u, &out) == 0);
    /* ...but fix round 1, finding 2: NOT cleared -- the obligation the file
     * already recorded survives for the next boot to retry from scratch. */
    assert(pending_close_active_count() == 1);
}

/* Fix round 3, finding 1: an exhausted (or merely partway-through) record
 * must reach disk with a FULL retry budget -- pending_close_persist_snapshot()
 * is the exact array pending_close_save() writes, so this proves the real
 * path, not a simulation of it. Round-trips through serialize/deserialize
 * too, so the whole chain -- live table -> persist snapshot -> wire bytes
 * -> a fresh boot's table -- is covered by one test. */
static void test_persist_snapshot_zeroes_retries_of_exhausted_record(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 0);
    uint32_t now = 0;
    pending_close_t out;
    for (int i = 0; i < PENDING_CLOSE_MAX_RETRIES; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, true, &out) == PENDING_CLOSE_STEP_REQUEST);
        now = out.deadline_s;
    }
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, true, &out) == PENDING_CLOSE_STEP_EXHAUSTED);
    assert(out.retries == PENDING_CLOSE_MAX_RETRIES); /* the LIVE row really is exhausted */

    pending_close_t snap[PENDING_CLOSE_MAX];
    assert(pending_close_persist_snapshot(snap, PENDING_CLOSE_MAX) == 1);
    assert(REC_IS(&snap[0], 1) && snap[0].retries == 0); /* what disk gets: full budget */

    /* The full round trip: serialize the snapshot (not the live row),
     * deserialize it back, boot-load it, and confirm the RESTORED record
     * is due immediately with a full budget -- not pre-exhausted. */
    uint8_t buf[64];
    size_t len = pending_close_serialize(snap, 1, buf, sizeof buf);
    pending_close_t restored[4];
    assert(pending_close_deserialize(buf, len, restored, 4) == 1);
    assert(restored[0].retries == 0);

    pending_close_init(); /* fresh boot */
    pending_close_boot_load(restored, 1);
    assert(pending_close_due(0, &out) == 1);
    assert(REC_IS(&out, 1) && out.retries == 0);
    /* And it can genuinely retry again -- not re-declared EXHAUSTED on the
     * very first check of the new boot. */
    assert(pending_close_step(KEY(1), 0, true, &out) == PENDING_CLOSE_STEP_REQUEST);
}

static void test_step_no_record_returns_none(void) {
    pending_close_init();
    pending_close_t out;
    assert(pending_close_step(KEY(42), 0, true, &out) == PENDING_CLOSE_STEP_NONE);
}

/* Fix round 2: fix round 1's own combination of rulings -- never spend the
 * retry budget on an unreachable device, never alert until the budget is
 * spent -- reproduced exactly the silence the exhaustion alert exists to
 * prevent. A device that is NEVER rediscovered must still break that
 * silence once, without ever spending the retry budget, clearing, or
 * exhausting. */
static void test_step_deferred_timeout_alerts_once_keeps_obligation_never_exhausts(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 100);
    uint32_t now = 100;
    pending_close_t out;
    uint32_t threshold = PENDING_CLOSE_DEFERRED_ALERT_S / PENDING_CLOSE_UNKNOWN_RECHECK_S;

    /* Every check before the threshold is ordinary DEFERRED. */
    for (uint32_t i = 0; i + 1 < threshold; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        assert(out.retries == 0);
        now = out.deadline_s;
    }

    /* The threshold-th consecutive DEFERRED fires the alert exactly once. */
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED_TIMEOUT);
    assert(out.retries == 0); /* still never spent */
    now = out.deadline_s;

    /* Every check after that, for as long as it stays unreachable, is
     * ordinary DEFERRED again -- once per streak, never a repeating alarm
     * (an 8-entry alert ring would churn under one). */
    for (int i = 0; i < 50; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        assert(out.retries == 0);
        now = out.deadline_s;
    }

    /* Still one obligation, still due, still never exhausted or cleared. */
    assert(pending_close_active_count() == 1);
    assert(pending_close_due(now, &out) == 1);
    assert(REC_IS(&out, 1) && out.retries == 0);
}

/* The streak (and its one-shot latch) must be a property of the CURRENT
 * unreachable run, not of the device's whole lifetime: once a real attempt
 * happens, a later unreachable run must be able to alert again. */
static void test_step_deferred_timeout_resets_when_device_becomes_known(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 0);
    uint32_t now = 0;
    pending_close_t out;
    uint32_t threshold = PENDING_CLOSE_DEFERRED_ALERT_S / PENDING_CLOSE_UNKNOWN_RECHECK_S;

    for (uint32_t i = 0; i + 1 < threshold; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        now = out.deadline_s;
    }

    /* One check short of the alert, the device is finally reached -- a
     * real attempt, which resets the streak (pending_close_step()'s own
     * doc comment on PENDING_CLOSE_STEP_REQUEST). */
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, true, &out) == PENDING_CLOSE_STEP_REQUEST);
    now = out.deadline_s;

    /* If it goes unreachable again afterwards, the FULL threshold must be
     * crossed again before it alerts -- proving the reset was real, not
     * merely "the same countdown, delayed by one". */
    for (uint32_t i = 0; i + 1 < threshold; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        now = out.deadline_s;
    }
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED_TIMEOUT);
}

/* A device that reopens (a fresh arm) gets a fresh streak too -- the same
 * "fresh obligation, fresh everything" rule test_rearm_replaces_and_resets_retries
 * already pins for `retries`. */
static void test_step_deferred_timeout_resets_on_rearm(void) {
    pending_close_init();
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, 0);
    uint32_t now = 0;
    pending_close_t out;
    uint32_t threshold = PENDING_CLOSE_DEFERRED_ALERT_S / PENDING_CLOSE_UNKNOWN_RECHECK_S;

    for (uint32_t i = 0; i + 1 < threshold; i++) {
        assert(pending_close_due(now, &out) == 1);
        assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
        now = out.deadline_s;
    }
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED_TIMEOUT);
    now = out.deadline_s;

    /* Device opens again: a brand new obligation. */
    pending_close_arm(KEY(1), ACT_SWITCH_OFF, now);
    assert(pending_close_due(now, &out) == 1);
    assert(pending_close_step(KEY(1), now, false, &out) == PENDING_CLOSE_STEP_DEFERRED);
}

/* ---------------------------------------------------------------------
 * pending_close_needed(): fix round 1, finding 6 -- the arm decision moved
 * out of ble_collector.c's GATT completion path into pure, testable code.
 * --------------------------------------------------------------------- */

static void test_needed_true_for_timed_action_without_device_local(void) {
    assert(pending_close_needed(ACT_IRRIGATION_OPEN, 0x00));
}

static void test_needed_false_when_device_local_flag_set(void) {
    assert(!pending_close_needed(ACT_IRRIGATION_OPEN, ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF));
}

static void test_needed_false_for_parameterless_action(void) {
    assert(!pending_close_needed(ACT_SWITCH_ON, 0x00));
    assert(!pending_close_needed(ACT_SWITCH_OFF, 0x00));
}

static void test_needed_false_for_unknown_action_id(void) {
    assert(!pending_close_needed(0xFEu, 0x00));
}

/* ---------------------------------------------------------------------
 * pending_close_arm_on_dispatch(): whole-branch review, Critical 1 +
 * Important 2 (ruling FINAL-arm). The obligation is armed BEFORE the
 * command reaches the radio, so a write that lands and then fails its
 * confirm -- or a link that drops after the write -- still leaves one.
 * --------------------------------------------------------------------- */

static void test_arm_on_dispatch_true_for_hub_owned_open(void) {
    /* The case the whole finding is about: a timed open the device will
     * NOT close itself, from a rule and from a manual press alike. */
    assert(pending_close_arm_on_dispatch(ACTOR_SRC_RULE, ACT_IRRIGATION_OPEN, 0x00));
    assert(pending_close_arm_on_dispatch(ACTOR_SRC_MANUAL, ACT_IRRIGATION_OPEN, 0x00));
    assert(pending_close_arm_on_dispatch(ACTOR_SRC_RULE, ACT_PUMP_RUN, 0x00));
}

static void test_arm_on_dispatch_false_for_safety_source(void) {
    /* A close must never arm its own close, whatever the action looks
     * like -- otherwise a retried safety close would recreate the very
     * obligation it exists to discharge. */
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_SAFETY, ACT_SWITCH_OFF, 0x00));
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_SAFETY, ACT_IRRIGATION_OPEN, 0x00));
}

static void test_arm_on_dispatch_false_when_device_closes_itself(void) {
    /* Spec section 4.3's preferred path: the device owns the close, so the
     * hub must not schedule one (pending_close_arm()'s own contract). */
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_RULE, ACT_IRRIGATION_OPEN,
                                          ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF));
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_MANUAL, ACT_PUMP_RUN,
                                          ACTOR_FLAG_DEVICE_LOCAL_TIMED_OFF));
}

static void test_arm_on_dispatch_false_for_parameterless_or_unknown(void) {
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_MANUAL, ACT_SWITCH_ON, 0x00));
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_MANUAL, ACT_SWITCH_OFF, 0x00));
    assert(!pending_close_arm_on_dispatch(ACTOR_SRC_RULE, 0xFEu, 0x00));
}

static void test_boot_load_ignores_stored_deadline(void) {
    pending_close_init();
    pending_close_t recs[1] = {
        REC(1, ACT_SWITCH_OFF, 999999, 3),
    };
    pending_close_boot_load(recs, 1);
    pending_close_t out;
    /* Due at uptime 0, not 999999 -- exactly what pending_close_is_boot_due()
     * pins in isolation; this proves boot_load() actually uses it. */
    assert(pending_close_due(0, &out) == 1);
    assert(REC_IS(&out, 1) && out.retries == 3);
}

static void test_boot_load_skips_invalid_records(void) {
    pending_close_init();
    pending_close_t recs[1];
    memset(recs, 0, sizeof recs);   /* the all-zero "no identity" sentinel */
    pending_close_boot_load(recs, 1);
    assert(pending_close_active_count() == 0);
}

/* KEY(0) is a perfectly real identity -- the sentinel is the ALL-ZERO key,
 * not "the first device". Nothing in the boot-due path may confuse the two
 * (the old dev_idx form had the same hazard with index 0 vs -1). */
static void test_is_boot_due_true_for_first_key(void) {
    pending_close_t r = REC(0, ACT_SWITCH_OFF, 12345, 0);
    assert(pending_close_is_boot_due(&r));
}

static void test_is_boot_due_false_for_free_slot(void) {
    pending_close_t r;
    memset(&r, 0, sizeof r);
    assert(!pending_close_is_boot_due(&r));
}

/* The table caps at PENDING_CLOSE_MAX (== ACTOR_MAX_DEVICES): a fifth
 * distinct device is dropped, never overflowing the RAM table or
 * corrupting an existing row. */
static void test_table_full_drops_extra_without_corruption(void) {
    pending_close_init();
    for (unsigned i = 0; i < PENDING_CLOSE_MAX; i++)
        pending_close_arm(KEY(i), ACT_SWITCH_OFF, (uint32_t)(100 + i));
    assert(pending_close_active_count() == PENDING_CLOSE_MAX);

    pending_close_arm(KEY(7), ACT_SWITCH_OFF, 5); /* one too many */
    assert(pending_close_active_count() == PENDING_CLOSE_MAX);

    pending_close_t out;
    assert(pending_close_due(1000, &out) == 1);
    assert(!REC_IS(&out, 7)); /* the dropped one never took a row */
}

int main(void) {
    test_round_trip();
    test_truncated_file_yields_nothing();
    test_all_due_after_boot();
    test_serialize_zero_records();
    test_serialize_buffer_too_small_writes_nothing();
    test_deserialize_wrong_fmt_yields_nothing();
    test_format_1_file_is_discarded_not_reinterpreted();
    test_deserialize_keyless_record_yields_nothing();
    test_two_similar_keys_are_distinct_records();
    test_reboot_device_reappears_at_a_different_index();
    test_unresolvable_identity_still_reaches_the_deferred_timeout();
    test_deserialize_bad_crc_yields_nothing();
    test_deserialize_cap_too_small_yields_nothing();
    test_deserialize_trailing_garbage_yields_nothing();
    test_arm_then_due();
    test_clear_removes();
    test_clear_unknown_device_is_a_safe_no_op();
    test_two_devices_independent();
    test_due_picks_earliest_when_multiple_due();
    test_rearm_replaces_and_resets_retries();
    test_step_known_device_backs_off();
    test_step_unknown_device_does_not_consume_retry();
    test_step_exhaustion_follows_final_backoff_not_synchronous();
    test_persist_snapshot_zeroes_retries_of_exhausted_record();
    test_step_no_record_returns_none();
    test_step_deferred_timeout_alerts_once_keeps_obligation_never_exhausts();
    test_step_deferred_timeout_resets_when_device_becomes_known();
    test_step_deferred_timeout_resets_on_rearm();
    test_needed_true_for_timed_action_without_device_local();
    test_needed_false_when_device_local_flag_set();
    test_needed_false_for_parameterless_action();
    test_needed_false_for_unknown_action_id();
    test_arm_on_dispatch_true_for_hub_owned_open();
    test_arm_on_dispatch_false_for_safety_source();
    test_arm_on_dispatch_false_when_device_closes_itself();
    test_arm_on_dispatch_false_for_parameterless_or_unknown();
    test_boot_load_ignores_stored_deadline();
    test_boot_load_skips_invalid_records();
    test_is_boot_due_true_for_first_key();
    test_is_boot_due_false_for_free_slot();
    test_table_full_drops_extra_without_corruption();
    printf("test_pending_close: OK\n");
    return 0;
}
