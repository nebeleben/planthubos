#include <assert.h>
#include <stdio.h>
#include "gatt_sched.h"

/* Backoff doubles and then stops doubling -- an unreachable device must
 * not drift to an interval so long it never retries. */
static void test_backoff_doubles_and_caps(void)
{
    gatt_sched_reset();
    for (int i = 0; i < 10; i++) gatt_sched_fail(0, 1000);
    /* declared 600 s; effective interval capped at 8x = 4800 s */
    assert(gatt_sched_due(0, 600, 1000 + 4799) == false);
    assert(gatt_sched_due(0, 600, 1000 + 4800) == true);
}

/* One success clears the backoff completely. */
static void test_success_resets_backoff(void)
{
    gatt_sched_reset();
    gatt_sched_fail(0, 1000); gatt_sched_fail(0, 1000);
    gatt_sched_ok(0, 2000);
    assert(gatt_sched_fail_count(0) == 0);
    assert(gatt_sched_due(0, 600, 2000 + 600) == true);
}

/* The cache is per device: dropping one must not disturb another. */
static void test_cache_drop_is_per_device(void)
{
    gatt_cache_reset();
    gatt_cache_store(0, 0x2A6E, 0x0025);
    gatt_cache_store(1, 0x2A6E, 0x0031);
    gatt_cache_drop(0);
    assert(gatt_cache_lookup(0, 0x2A6E) == 0);
    assert(gatt_cache_lookup(1, 0x2A6E) == 0x0031);
}

/* A fifth entry for one device must not silently evict a needed handle --
 * a plan declares at most 4 reads, so a 5th store is a bug upstream. */
static void test_cache_overflow_is_refused(void)
{
    gatt_cache_reset();
    gatt_cache_store(2, 0x1111, 0x0010);
    gatt_cache_store(2, 0x2222, 0x0020);
    gatt_cache_store(2, 0x3333, 0x0030);
    gatt_cache_store(2, 0x4444, 0x0040);
    gatt_cache_store(2, 0x5555, 0x0050);   /* 5th distinct uuid16: refused */

    assert(gatt_cache_lookup(2, 0x1111) == 0x0010);
    assert(gatt_cache_lookup(2, 0x2222) == 0x0020);
    assert(gatt_cache_lookup(2, 0x3333) == 0x0030);
    assert(gatt_cache_lookup(2, 0x4444) == 0x0040);
    assert(gatt_cache_lookup(2, 0x5555) == 0);

    /* A re-discovery of an ALREADY-cached uuid16 is not a new entry --
     * it must still be free to update in place even while full. */
    gatt_cache_store(2, 0x1111, 0x0011);
    assert(gatt_cache_lookup(2, 0x1111) == 0x0011);
}

/* Never-read device is due immediately; a device read a second ago is not. */
static void test_due_boundaries(void)
{
    gatt_sched_reset();
    assert(gatt_sched_due(3, 600, 12345) == true);   /* never contacted */

    gatt_sched_ok(3, 1000);
    assert(gatt_sched_due(3, 600, 1001) == false);    /* read a second ago */
    assert(gatt_sched_due(3, 600, 1000 + 599) == false);
    assert(gatt_sched_due(3, 600, 1000 + 600) == true);
}

/* The cache and scheduler are independently keyed by dev_idx and don't
 * interfere with each other's tables. */
static void test_last_ok_getter(void)
{
    gatt_sched_reset();
    assert(gatt_sched_last_ok(4) == 0);
    gatt_sched_ok(4, 500);
    assert(gatt_sched_last_ok(4) == 500);
    gatt_sched_fail(4, 900);
    assert(gatt_sched_last_ok(4) == 900);
    assert(gatt_sched_fail_count(4) == 1);
}

int main(void)
{
    test_backoff_doubles_and_caps();
    test_success_resets_backoff();
    test_cache_drop_is_per_device();
    test_cache_overflow_is_refused();
    test_due_boundaries();
    test_last_ok_getter();

    printf("test_gatt_sched: OK\n");
    return 0;
}
