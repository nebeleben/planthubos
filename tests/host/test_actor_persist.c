/* tests/host/test_actor_persist.c -- guard persistence across a reboot
 * (M5b whole-branch review, ruling FINAL-persist).
 *
 * Covers the two pure halves together, because they are one mechanism:
 * actor_persist.c's bytes-on-disk (serialise/deserialise/find) and
 * actor_table.c's decisions (which rows a merge keeps, and what a restored
 * uptime timestamp is taken to mean). The LittleFS wrapper itself is
 * ESP_PLATFORM-gated and not exercised here, exactly like
 * test_pending_close.c's own split. */
#include "actor_persist.h"
#include "actor_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static actor_table_t T;

/* Two distinct stable identities, shaped like real device_id_t values
 * (kind 1 = BLE, then a 6-byte MAC zero-padded to 8). */
static const uint8_t KEY_A[ACTOR_DEVICE_KEY_LEN] =
    { 1, 0xD0, 0xCF, 0x13, 0xE5, 0xB9, 0xDA, 0, 0 };
static const uint8_t KEY_B[ACTOR_DEVICE_KEY_LEN] =
    { 1, 0xBC, 0xC8, 0x11, 0x22, 0x33, 0x44, 0, 0 };

static void setup_one_device(void)
{
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_IRRIGATION_OPEN, 300, 0x00));
    assert(actor_table_add(&T, 3, ACT_SWITCH_OFF, 0, 0x00));
    actor_table_set_key(&T, 3, KEY_A);
}

/* ---------------------------------------------------------------------
 * Wire format
 * --------------------------------------------------------------------- */

static void test_round_trip(void)
{
    actor_guard_row_t in[2];
    memset(in, 0, sizeof in);
    memcpy(in[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    in[0].action_id = ACT_IRRIGATION_OPEN;
    in[0].lockout = true;
    in[0].cooldown_s = 21600;
    in[0].max_per_hour = 4;
    in[0].window_count = 3;
    in[0].window_start_s = 123456;
    in[0].last_fire_s = 123999;
    memcpy(in[1].key, KEY_B, ACTOR_DEVICE_KEY_LEN);
    in[1].action_id = ACT_SWITCH_OFF;
    in[1].lockout = false;
    in[1].cooldown_s = 0;
    in[1].max_per_hour = 0;
    in[1].window_count = 0;

    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(in, 2, buf, sizeof buf);
    assert(len == ACTOR_PERSIST_HEADER_LEN + 2 * ACTOR_PERSIST_RECORD_LEN);
    assert(buf[0] == ACTOR_PERSIST_FMT);
    assert(buf[1] == 2);

    actor_guard_row_t out[ACTOR_GUARD_ROWS_MAX];
    assert(actor_persist_deserialize(buf, len, out, ACTOR_GUARD_ROWS_MAX) == 2);
    assert(memcmp(&out[0], &in[0], sizeof in[0]) == 0);
    assert(memcmp(&out[1], &in[1], sizeof in[1]) == 0);
}

static void test_empty_table_round_trips(void)
{
    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(NULL, 0, buf, sizeof buf);
    assert(len == ACTOR_PERSIST_HEADER_LEN);
    actor_guard_row_t out[ACTOR_GUARD_ROWS_MAX];
    assert(actor_persist_deserialize(buf, len, out, ACTOR_GUARD_ROWS_MAX) == 0);
}

static void test_full_table_fits_the_buffer(void)
{
    actor_guard_row_t in[ACTOR_GUARD_ROWS_MAX];
    memset(in, 0, sizeof in);
    for (size_t i = 0; i < ACTOR_GUARD_ROWS_MAX; i++) {
        memcpy(in[i].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
        in[i].key[ACTOR_DEVICE_KEY_LEN - 1] = (uint8_t)i;   /* distinct devices */
        in[i].action_id = ACT_SWITCH_ON;
    }
    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(in, ACTOR_GUARD_ROWS_MAX, buf, sizeof buf);
    assert(len == sizeof buf);
    actor_guard_row_t out[ACTOR_GUARD_ROWS_MAX];
    assert(actor_persist_deserialize(buf, len, out, ACTOR_GUARD_ROWS_MAX)
           == ACTOR_GUARD_ROWS_MAX);
}

static void test_serialize_refuses_rather_than_truncates(void)
{
    actor_guard_row_t in[1];
    memset(in, 0, sizeof in);
    memcpy(in[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    uint8_t small[ACTOR_PERSIST_HEADER_LEN + ACTOR_PERSIST_RECORD_LEN - 1];
    assert(actor_persist_serialize(in, 1, small, sizeof small) == 0);
    /* And nothing was written into it: byte 0 would be the format byte. */
    assert(actor_persist_serialize(in, ACTOR_GUARD_ROWS_MAX + 1, small, sizeof small) == 0);
}

/* A corrupt or truncated file must yield NOTHING rather than a partial
 * table -- half a guard table is a guard table an operator did not
 * configure, and the half that is missing might be the lockout. */
static void test_corruption_yields_nothing(void)
{
    actor_guard_row_t in[2];
    memset(in, 0, sizeof in);
    memcpy(in[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    in[0].action_id = ACT_IRRIGATION_OPEN;
    in[0].lockout = true;
    memcpy(in[1].key, KEY_B, ACTOR_DEVICE_KEY_LEN);
    in[1].action_id = ACT_SWITCH_OFF;

    uint8_t good[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(in, 2, good, sizeof good);
    actor_guard_row_t out[ACTOR_GUARD_ROWS_MAX];
    assert(actor_persist_deserialize(good, len, out, ACTOR_GUARD_ROWS_MAX) == 2);

    /* Every truncation. */
    for (size_t cut = 1; cut <= len; cut++) {
        assert(actor_persist_deserialize(good, len - cut, out, ACTOR_GUARD_ROWS_MAX) == 0);
    }
    /* Trailing garbage -- an exact-length match, not "at least". */
    uint8_t longer[ACTOR_PERSIST_BUF_MAX + 1];
    memcpy(longer, good, len);
    longer[len] = 0xAA;
    assert(actor_persist_deserialize(longer, len + 1, out, ACTOR_GUARD_ROWS_MAX) == 0);

    /* Every single-byte flip in the whole image. */
    for (size_t i = 0; i < len; i++) {
        uint8_t bad[ACTOR_PERSIST_BUF_MAX];
        memcpy(bad, good, len);
        bad[i] ^= 0xFFu;
        assert(actor_persist_deserialize(bad, len, out, ACTOR_GUARD_ROWS_MAX) == 0);
    }

    /* An unknown action id, with the crc recomputed so only the semantic
     * check can catch it. */
    {
        actor_guard_row_t bad_in[1];
        memset(bad_in, 0, sizeof bad_in);
        memcpy(bad_in[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
        bad_in[0].action_id = 0xFE;
        uint8_t b[ACTOR_PERSIST_BUF_MAX];
        size_t l = actor_persist_serialize(bad_in, 1, b, sizeof b);
        assert(l > 0);
        assert(actor_persist_deserialize(b, l, out, ACTOR_GUARD_ROWS_MAX) == 0);
    }
    /* The all-zero "no key" sentinel: a row nothing could ever be matched
     * back to. Same treatment. */
    {
        actor_guard_row_t bad_in[1];
        memset(bad_in, 0, sizeof bad_in);
        bad_in[0].action_id = ACT_SWITCH_OFF;
        uint8_t b[ACTOR_PERSIST_BUF_MAX];
        size_t l = actor_persist_serialize(bad_in, 1, b, sizeof b);
        assert(l > 0);
        assert(actor_persist_deserialize(b, l, out, ACTOR_GUARD_ROWS_MAX) == 0);
    }
    /* A count larger than the caller's capacity. */
    assert(actor_persist_deserialize(good, len, out, 1) == 0);
}

static void test_find_walks_every_row_of_one_device(void)
{
    actor_guard_row_t rows[3];
    memset(rows, 0, sizeof rows);
    memcpy(rows[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN); rows[0].action_id = ACT_IRRIGATION_OPEN;
    memcpy(rows[1].key, KEY_B, ACTOR_DEVICE_KEY_LEN); rows[1].action_id = ACT_SWITCH_ON;
    memcpy(rows[2].key, KEY_A, ACTOR_DEVICE_KEY_LEN); rows[2].action_id = ACT_SWITCH_OFF;

    assert(actor_persist_find(rows, 3, KEY_A, 0) == 0);
    assert(actor_persist_find(rows, 3, KEY_A, 1) == 2);
    assert(actor_persist_find(rows, 3, KEY_A, 3) == -1);
    assert(actor_persist_find(rows, 3, KEY_B, 0) == 1);
    assert(actor_persist_find(rows, 0, KEY_A, 0) == -1);
}

/* ---------------------------------------------------------------------
 * Merge: which rows survive
 * --------------------------------------------------------------------- */

static void test_merge_captures_config_and_state(void)
{
    setup_one_device();
    assert(actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 600, 4));
    actor_table_set_lockout(&T, 3, true);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1000);
    actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1200);

    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    size_t n = actor_table_guard_merge(&T, rows, 0, ACTOR_GUARD_ROWS_MAX);
    assert(n == 2);   /* both declared actions */

    int at = actor_persist_find(rows, n, KEY_A, 0);
    assert(at >= 0);
    const actor_guard_row_t *open = (rows[at].action_id == ACT_IRRIGATION_OPEN)
                                        ? &rows[at] : &rows[at + 1];
    assert(open->action_id == ACT_IRRIGATION_OPEN);
    assert(open->cooldown_s == 600);
    assert(open->max_per_hour == 4);
    assert(open->window_count == 2);
    assert(open->window_start_s == 1000);
    assert(open->last_fire_s == 1200);
    assert(open->lockout);
}

/* A device with no key set is not persistable -- there would be nothing to
 * match it back to after a reboot. */
static void test_merge_skips_a_device_with_no_key(void)
{
    actor_table_init(&T);
    assert(actor_table_add(&T, 3, ACT_SWITCH_ON, 0, 0x00));
    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    assert(actor_table_guard_merge(&T, rows, 0, ACTOR_GUARD_ROWS_MAX) == 0);
}

/* THE load-bearing rule: a device that has not advertised since this boot
 * is not declared, and its operator's lockout must not be erased from
 * flash just because some OTHER device fired. */
static void test_merge_keeps_rows_of_a_device_not_seen_this_boot(void)
{
    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    memcpy(rows[0].key, KEY_B, ACTOR_DEVICE_KEY_LEN);
    rows[0].action_id = ACT_IRRIGATION_OPEN;
    rows[0].lockout = true;
    rows[0].max_per_hour = 4;
    rows[0].window_count = 4;

    setup_one_device();   /* declares KEY_A only */
    size_t n = actor_table_guard_merge(&T, rows, 1, ACTOR_GUARD_ROWS_MAX);
    assert(n == 3);   /* KEY_B's untouched row, plus KEY_A's two */

    int at = actor_persist_find(rows, n, KEY_B, 0);
    assert(at >= 0);
    assert(rows[at].lockout);
    assert(rows[at].max_per_hour == 4);
    assert(rows[at].window_count == 4);
}

/* The other side of it: a device that IS declared, for an action it no
 * longer declares, loses that row -- the wrapper's action block changed. */
static void test_merge_drops_a_stale_action_of_a_declared_device(void)
{
    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    memcpy(rows[0].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    rows[0].action_id = ACT_PUMP_RUN;         /* not declared below */
    memcpy(rows[1].key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    rows[1].action_id = ACT_IRRIGATION_OPEN;  /* declared below */
    rows[1].max_per_hour = 4;

    setup_one_device();
    size_t n = actor_table_guard_merge(&T, rows, 2, ACTOR_GUARD_ROWS_MAX);
    assert(n == 2);
    for (size_t i = 0; i < n; i++) assert(rows[i].action_id != ACT_PUMP_RUN);
}

static void test_merge_updates_in_place_rather_than_appending(void)
{
    setup_one_device();
    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    size_t n = actor_table_guard_merge(&T, rows, 0, ACTOR_GUARD_ROWS_MAX);
    assert(n == 2);

    assert(actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 900, 2));
    n = actor_table_guard_merge(&T, rows, n, ACTOR_GUARD_ROWS_MAX);
    assert(n == 2);   /* still two rows, not four */
    for (size_t i = 0; i < n; i++) {
        if (rows[i].action_id == ACT_IRRIGATION_OPEN) assert(rows[i].cooldown_s == 900);
    }
}

/* A live pair always gets a slot: cap is exactly the number of pairs that
 * can be declared at once, so when the image is full of rows belonging to
 * devices that are NOT declared, one of them is evicted. */
static void test_merge_evicts_a_stale_row_when_full(void)
{
    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    for (size_t i = 0; i < ACTOR_GUARD_ROWS_MAX; i++) {
        memcpy(rows[i].key, KEY_B, ACTOR_DEVICE_KEY_LEN);
        rows[i].key[ACTOR_DEVICE_KEY_LEN - 1] = (uint8_t)(i + 1);   /* all different, none live */
        rows[i].action_id = ACT_SWITCH_ON;
    }

    setup_one_device();
    size_t n = actor_table_guard_merge(&T, rows, ACTOR_GUARD_ROWS_MAX, ACTOR_GUARD_ROWS_MAX);
    assert(n == ACTOR_GUARD_ROWS_MAX);

    /* The live device's two actions are both in there now. */
    unsigned live_rows = 0;
    for (size_t i = 0; i < n; i++) {
        if (memcmp(rows[i].key, KEY_A, ACTOR_DEVICE_KEY_LEN) == 0) live_rows++;
    }
    assert(live_rows == 2);
}

/* ---------------------------------------------------------------------
 * Apply: what a restored row means
 * --------------------------------------------------------------------- */

static void test_apply_restores_config_and_does_not_refund_the_budget(void)
{
    /* A fresh boot: the device is declared with clean guards. */
    setup_one_device();

    actor_guard_row_t row;
    memset(&row, 0, sizeof row);
    memcpy(row.key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    row.action_id = ACT_IRRIGATION_OPEN;
    row.lockout = true;
    row.cooldown_s = 60;
    row.max_per_hour = 4;
    row.window_count = 4;        /* the whole hourly budget was spent */
    row.window_start_s = 900000; /* pre-reboot uptime: meaningless now */
    row.last_fire_s = 903000;

    /* Before the restore, the reboot has refunded everything -- which is
     * exactly the bug. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 50) == ACTOR_OK);

    assert(actor_table_guard_apply(&T, 3, &row, /*now_s*/ 50));

    /* The stop button is back on, so a rule is refused... */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 50)
           == ACTOR_REFUSED_LOCKOUT);
    /* ...and a manual press, which lockout permits, hits the restored
     * cooldown rather than sailing through. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 50)
           == ACTOR_REFUSED_COOLDOWN);
    /* Past the cooldown, the spent hourly budget is still spent. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 200)
           == ACTOR_REFUSED_RATE);
    /* And it stays spent for a full window measured from the RESTORE, not
     * from the pre-reboot uptime the file happened to record. */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 50 + 3599)
           == ACTOR_REFUSED_RATE);
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 50 + 3600)
           == ACTOR_OK);

    /* The safety close is exempt from every one of those, as always. */
    assert(actor_table_check(&T, 3, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 50) == ACTOR_OK);
}

/* window_count == 0 is "never fired". A restored row saying so must leave
 * the slot byte-identical to a freshly declared one, not plant a
 * last_fire_s that the cooldown could later read. */
static void test_apply_of_a_never_fired_row_leaves_clocks_at_zero(void)
{
    setup_one_device();
    actor_guard_row_t row;
    memset(&row, 0, sizeof row);
    memcpy(row.key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    row.action_id = ACT_IRRIGATION_OPEN;
    row.cooldown_s = 600;
    row.max_per_hour = 4;
    row.window_count = 0;
    row.window_start_s = 900000;
    row.last_fire_s = 903000;

    assert(actor_table_guard_apply(&T, 3, &row, /*now_s*/ 50));
    /* Cooldown configured but never fired -> permitted (the same rule
     * test_cooldown_before_first_fire_permits() pins for a fresh pair). */
    assert(actor_table_check(&T, 3, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 50) == ACTOR_OK);

    actor_pair_state_t ps;
    assert(actor_table_pair_state(&T, 3, ACT_IRRIGATION_OPEN, 50, &ps));
    assert(!ps.has_fired);
    assert(ps.last_fire_s == 0);
    assert(ps.activations_this_hour == 0);
}

static void test_apply_rejects_an_undeclared_pair_or_device(void)
{
    setup_one_device();
    actor_guard_row_t row;
    memset(&row, 0, sizeof row);
    memcpy(row.key, KEY_A, ACTOR_DEVICE_KEY_LEN);
    row.action_id = ACT_PUMP_RUN;   /* declared by nobody here */
    assert(!actor_table_guard_apply(&T, 3, &row, 50));
    row.action_id = ACT_IRRIGATION_OPEN;
    assert(!actor_table_guard_apply(&T, 9, &row, 50));   /* no such device */
    assert(!actor_table_guard_apply(&T, -1, &row, 50));
    assert(!actor_table_guard_apply(&T, 3, NULL, 50));
}

/* End to end, the sequence the hub actually performs: configure guards,
 * spend some budget, serialise, "reboot" (a fresh table), re-declare, and
 * restore. */
static void test_reboot_round_trip(void)
{
    setup_one_device();
    assert(actor_table_set_guards(&T, 3, ACT_IRRIGATION_OPEN, 0, 4));
    actor_table_set_lockout(&T, 3, true);
    for (int i = 0; i < 4; i++) actor_table_record(&T, 3, ACT_IRRIGATION_OPEN, 1000 + i * 10);

    actor_guard_row_t rows[ACTOR_GUARD_ROWS_MAX];
    memset(rows, 0, sizeof rows);
    size_t n = actor_table_guard_merge(&T, rows, 0, ACTOR_GUARD_ROWS_MAX);
    uint8_t buf[ACTOR_PERSIST_BUF_MAX];
    size_t len = actor_persist_serialize(rows, n, buf, sizeof buf);
    assert(len > 0);

    /* ---- reboot ---- */
    actor_guard_row_t loaded[ACTOR_GUARD_ROWS_MAX];
    size_t ln = actor_persist_deserialize(buf, len, loaded, ACTOR_GUARD_ROWS_MAX);
    assert(ln == n);

    /* The device now comes up at a DIFFERENT registry index -- which is
     * the whole reason rows are keyed on the device and not on dev_idx. */
    actor_table_init(&T);
    assert(actor_table_add(&T, 0, ACT_IRRIGATION_OPEN, 300, 0x00));
    assert(actor_table_add(&T, 0, ACT_SWITCH_OFF, 0, 0x00));
    actor_table_set_key(&T, 0, KEY_A);

    unsigned applied = 0;
    int at = actor_persist_find(loaded, ln, KEY_A, 0);
    while (at >= 0) {
        if (actor_table_guard_apply(&T, 0, &loaded[at], /*now_s*/ 5)) applied++;
        at = actor_persist_find(loaded, ln, KEY_A, (size_t)at + 1);
    }
    assert(applied == 2);

    bool lock = false;
    assert(actor_table_lockout(&T, 0, &lock) && lock);
    assert(actor_table_check(&T, 0, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_RULE, 5)
           == ACTOR_REFUSED_LOCKOUT);
    assert(actor_table_check(&T, 0, ACT_IRRIGATION_OPEN, 10, ACTOR_SRC_MANUAL, 5)
           == ACTOR_REFUSED_RATE);
    assert(actor_table_check(&T, 0, ACT_SWITCH_OFF, 0, ACTOR_SRC_SAFETY, 5) == ACTOR_OK);
}

int main(void)
{
    test_round_trip();
    test_empty_table_round_trips();
    test_full_table_fits_the_buffer();
    test_serialize_refuses_rather_than_truncates();
    test_corruption_yields_nothing();
    test_find_walks_every_row_of_one_device();
    test_merge_captures_config_and_state();
    test_merge_skips_a_device_with_no_key();
    test_merge_keeps_rows_of_a_device_not_seen_this_boot();
    test_merge_drops_a_stale_action_of_a_declared_device();
    test_merge_updates_in_place_rather_than_appending();
    test_merge_evicts_a_stale_row_when_full();
    test_apply_restores_config_and_does_not_refund_the_budget();
    test_apply_of_a_never_fired_row_leaves_clocks_at_zero();
    test_apply_rejects_an_undeclared_pair_or_device();
    test_reboot_round_trip();
    printf("test_actor_persist: OK (sizeof(actor_guard_row_t)=%u, image=%u B, file<=%u B)\n",
           (unsigned)sizeof(actor_guard_row_t),
           (unsigned)(sizeof(actor_guard_row_t) * ACTOR_GUARD_ROWS_MAX),
           (unsigned)ACTOR_PERSIST_BUF_MAX);
    return 0;
}
