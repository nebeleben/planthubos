#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "adv_queue.h"

/* Builds a distinguishable item: mac/rssi/addr_type/len/payload/uptime_s
 * all derived from n so a round trip can be checked byte-for-byte. */
static void make_item(adv_item_t *it, int n)
{
    memset(it, 0, sizeof(*it));
    for (int i = 0; i < 6; i++) it->mac[i] = (uint8_t)(n * 6 + i);
    it->rssi = (int8_t)(-40 - n);
    it->addr_type = (uint8_t)(n & 0x1);
    it->len = (uint8_t)((n % ADV_PAYLOAD_MAX) + 1);
    for (int i = 0; i < it->len; i++) it->payload[i] = (uint8_t)(n + i);
    it->uptime_s = (uint32_t)(1000 + n);
}

int main(void)
{
    /* init empty: count 0, dropped 0, pop fails */
    adv_ring_t r;
    adv_ring_init(&r);
    assert(adv_ring_count(&r) == 0);
    assert(adv_ring_dropped(&r) == 0);
    adv_item_t out;
    assert(adv_ring_pop(&r, &out) == false);

    /* push/pop FIFO order preserved, payload bytes survive round trip */
    adv_item_t a, b, c;
    make_item(&a, 1);
    make_item(&b, 2);
    make_item(&c, 3);
    assert(adv_ring_push(&r, &a) == true);
    assert(adv_ring_push(&r, &b) == true);
    assert(adv_ring_count(&r) == 2);
    assert(adv_ring_push(&r, &c) == true);
    assert(adv_ring_count(&r) == 3);

    assert(adv_ring_pop(&r, &out) == true);
    assert(memcmp(&out, &a, sizeof(out)) == 0);
    assert(adv_ring_pop(&r, &out) == true);
    assert(memcmp(&out, &b, sizeof(out)) == 0);
    assert(adv_ring_count(&r) == 1);
    assert(adv_ring_pop(&r, &out) == true);
    assert(memcmp(&out, &c, sizeof(out)) == 0);
    assert(adv_ring_count(&r) == 0);

    /* pop from empty (again, after having drained it) returns false */
    assert(adv_ring_pop(&r, &out) == false);

    /* drive head/tail through wrap: push+pop one at a time several times
     * past ADV_QUEUE_LEN so head/tail indices wrap around the array,
     * checking FIFO order and count survive it. */
    adv_ring_init(&r);
    for (int round = 0; round < ADV_QUEUE_LEN * 2 + 3; round++) {
        adv_item_t in;
        make_item(&in, round);
        assert(adv_ring_push(&r, &in) == true);
        assert(adv_ring_count(&r) == 1);
        assert(adv_ring_pop(&r, &out) == true);
        assert(memcmp(&out, &in, sizeof(out)) == 0);
        assert(adv_ring_count(&r) == 0);
    }
    assert(adv_ring_dropped(&r) == 0);

    /* fill to ADV_QUEUE_LEN, then one more push returns false and bumps
     * dropped, without corrupting the existing items -- verified by
     * popping every original item back out in order afterward. */
    adv_ring_init(&r);
    adv_item_t items[ADV_QUEUE_LEN];
    for (int i = 0; i < ADV_QUEUE_LEN; i++) {
        make_item(&items[i], i + 100);
        assert(adv_ring_push(&r, &items[i]) == true);
    }
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN);

    adv_item_t overflow;
    make_item(&overflow, 999);
    assert(adv_ring_push(&r, &overflow) == false);
    assert(adv_ring_dropped(&r) == 1);
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN);   /* unchanged by the drop */

    /* another overflow push bumps dropped again */
    assert(adv_ring_push(&r, &overflow) == false);
    assert(adv_ring_dropped(&r) == 2);

    for (int i = 0; i < ADV_QUEUE_LEN; i++) {
        assert(adv_ring_pop(&r, &out) == true);
        assert(memcmp(&out, &items[i], sizeof(out)) == 0);
    }
    assert(adv_ring_count(&r) == 0);
    assert(adv_ring_pop(&r, &out) == false);
    assert(adv_ring_dropped(&r) == 2);   /* draining doesn't touch dropped */

    /* count tracks correctly through a partial-fill/partial-drain wrap:
     * push ADV_QUEUE_LEN-1, pop 3, push 3 more (wrapping head past the
     * array end), checking count after every step. */
    adv_ring_init(&r);
    for (int i = 0; i < ADV_QUEUE_LEN - 1; i++) {
        adv_item_t in;
        make_item(&in, i);
        assert(adv_ring_push(&r, &in) == true);
    }
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN - 1);
    for (int i = 0; i < 3; i++) {
        assert(adv_ring_pop(&r, &out) == true);
    }
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN - 4);
    for (int i = 0; i < 3; i++) {
        adv_item_t in;
        make_item(&in, 200 + i);
        assert(adv_ring_push(&r, &in) == true);
    }
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN - 1);
    /* now fill it the rest of the way and confirm full-detection still works */
    adv_item_t last;
    make_item(&last, 300);
    assert(adv_ring_push(&r, &last) == true);
    assert(adv_ring_count(&r) == ADV_QUEUE_LEN);
    adv_item_t rejected;
    make_item(&rejected, 301);
    assert(adv_ring_push(&r, &rejected) == false);
    assert(adv_ring_dropped(&r) == 1);

    printf("test_adv_queue: OK\n");
    return 0;
}
