#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wrapper_index.h"

static const uint8_t MAC_A[6] = {0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03};
static const uint8_t MAC_B[6] = {0xAA, 0xBB, 0xCC, 0x99, 0x98, 0x97};   /* same prefix as MAC_A */
static const uint8_t MAC_C[6] = {0x11, 0x22, 0x33, 0x01, 0x02, 0x03};  /* different prefix */

int main(void)
{
    wrapper_index_t ix;
    wrapper_index_init(&ix);
    assert(ix.count == 0);

    /* add/lookup for all three kinds */
    assert(wrapper_index_add(&ix, WMATCH_SERVICE, 0x181A, 10) == 0);
    assert(wrapper_index_add(&ix, WMATCH_MANUFACTURER, 0x0499, 11) == 0);
    assert(wrapper_index_add(&ix, WMATCH_MAC_PREFIX, 0xAABBCC, 12) == 0);
    assert(ix.count == 3);

    assert(wrapper_index_lookup(&ix, 0x181A, 0xFFFFFFFF, MAC_C) == 10);
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0x0499, MAC_C) == 11);
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0xFFFFFFFF, MAC_A) == 12);
    /* MAC-prefix matching compares exactly 3 bytes: MAC_B shares MAC_A's
     * first 3 bytes but differs after, and still matches. */
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0xFFFFFFFF, MAC_B) == 12);

    /* a service-UUID advert must not match a manufacturer entry with the
     * same numeric key */
    assert(wrapper_index_add(&ix, WMATCH_SERVICE, 0x2A2A, 20) == 0);
    assert(wrapper_index_add(&ix, WMATCH_MANUFACTURER, 0x2A2A, 21) == 0);
    assert(wrapper_index_lookup(&ix, 0x2A2A, 0xFFFFFFFF, MAC_C) == 20);
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0x2A2A, MAC_C) == 21);

    /* duplicate (kind,key) rejected with -1 -- table unchanged */
    uint8_t count_before = ix.count;
    assert(wrapper_index_add(&ix, WMATCH_SERVICE, 0x2A2A, 22) == -1);
    assert(ix.count == count_before);

    /* lookup with all-absent inputs (no service/manufacturer data, and a
     * MAC prefix nothing registered) returns -1 */
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0xFFFFFFFF, MAC_C) == -1);

    /* remove then lookup misses and frees a slot */
    assert(wrapper_index_remove(&ix, 11) == true);
    assert(ix.count == count_before - 1);
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0x0499, MAC_C) == -1);
    /* removing an id that was never added fails without disturbing the table */
    assert(wrapper_index_remove(&ix, 999) == false);
    assert(ix.count == count_before - 1);
    /* the freed slot is usable again */
    assert(wrapper_index_add(&ix, WMATCH_MANUFACTURER, 0x0499, 30) == 0);
    assert(wrapper_index_lookup(&ix, 0xFFFFFFFF, 0x0499, MAC_C) == 30);

    /* full index rejected with -1 */
    wrapper_index_t full;
    wrapper_index_init(&full);
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        assert(wrapper_index_add(&full, WMATCH_SERVICE, (uint32_t)(0x1000 + i), (uint16_t)i) == 0);
    }
    assert(full.count == WRAPPERS_MAX);
    assert(wrapper_index_add(&full, WMATCH_SERVICE, 0x9999, 200) == -1);
    assert(full.count == WRAPPERS_MAX);
    /* every entry that made it in is still reachable */
    for (int i = 0; i < WRAPPERS_MAX; i++) {
        assert(wrapper_index_lookup(&full, (uint32_t)(0x1000 + i), 0xFFFFFFFF, MAC_C) == i);
    }

    /* removing from a full table frees exactly one slot */
    assert(wrapper_index_remove(&full, 5) == true);
    assert(full.count == WRAPPERS_MAX - 1);
    assert(wrapper_index_lookup(&full, 0x1005, 0xFFFFFFFF, MAC_C) == -1);
    assert(wrapper_index_add(&full, WMATCH_SERVICE, 0x9999, 200) == 0);
    assert(wrapper_index_lookup(&full, 0x9999, 0xFFFFFFFF, MAC_C) == 200);

    /* M3 review fix 3: WMATCH_MAC_PREFIX's key is display/human order
     * (wrapper_index.h's top comment), pinned here against a realistic MAC
     * so a future accidental reversal (e.g. a caller passing raw GAP order
     * again) is caught by this suite instead of only failing silently on
     * hardware. For display MAC D0:CF:13:E5:BC:CA, a wrapper registered as
     * `match mac_prefix 0xD0CF13` (the prefix a human reads/types, and what
     * GET /api/v1/unknown's `id` and wrapper_store.c's header parser both
     * use) must match a lookup() call passed that MAC in the SAME
     * mac[0]=0xD0 order -- and the reversed-order key (0xCABCE5, what a
     * caller mistakenly passing raw GAP order would produce) must NOT. */
    {
        static const uint8_t MAC_DISPLAY[6] = {0xD0, 0xCF, 0x13, 0xE5, 0xBC, 0xCA};
        wrapper_index_t ixp;
        wrapper_index_init(&ixp);
        assert(wrapper_index_add(&ixp, WMATCH_MAC_PREFIX, 0xD0CF13, 40) == 0);
        assert(wrapper_index_lookup(&ixp, 0xFFFFFFFF, 0xFFFFFFFF, MAC_DISPLAY) == 40);

        wrapper_index_t ixp_reversed;
        wrapper_index_init(&ixp_reversed);
        assert(wrapper_index_add(&ixp_reversed, WMATCH_MAC_PREFIX, 0xCABCE5, 41) == 0);
        assert(wrapper_index_lookup(&ixp_reversed, 0xFFFFFFFF, 0xFFFFFFFF, MAC_DISPLAY) == -1);
    }

    /* wrapper_index_kind_of() -- M3 Task 5 addition */
    assert(wrapper_index_kind_of(&ix, 10) == WMATCH_SERVICE);
    assert(wrapper_index_kind_of(&ix, 12) == WMATCH_MAC_PREFIX);
    assert(wrapper_index_kind_of(&ix, 30) == WMATCH_MANUFACTURER);
    assert(wrapper_index_kind_of(&ix, 999) == 0xFF);

    printf("test_wrapper_index: OK\n");
    return 0;
}
