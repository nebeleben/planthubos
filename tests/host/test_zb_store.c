/* Host test for zb_store.c (M6b spec section 4). Pure table + byte
 * serialisation, no file I/O -- zigbee.c owns the tmp+rename write. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "zb_store.h"

static zb_device_t mk(uint8_t last, uint8_t cap) {
    zb_device_t d;
    memset(&d, 0, sizeof d);
    const uint8_t base[8] = { 0x00, 0x12, 0x4B, 0x00, 0x0A, 0x0B, 0x0C, 0x00 };
    memcpy(d.eui64, base, 8);
    d.eui64[7]     = last;
    d.short_addr   = 0x1000 + last;
    d.endpoint     = 1;
    d.interviewed  = 1;
    d.cap_count    = 1;
    d.caps[0]      = cap;
    d.cap_clusters[0] = 0x0402;
    snprintf(d.name, sizeof d.name, "dev%u", last);
    return d;
}

int main(void) {
    zb_table_t t;
    zb_store_init(&t);
    assert(t.count == 0);

    /* --- upsert / find --- */
    zb_device_t a = mk(1, 1), b = mk(2, 5);
    assert(zb_store_upsert(&t, &a) == 0);
    assert(zb_store_upsert(&t, &b) == 1);
    assert(t.count == 2);
    assert(zb_store_find(&t, a.eui64) == 0);
    assert(zb_store_find(&t, b.eui64) == 1);

    /* Upsert of a known EUI-64 REPLACES in place, never appends -- a
     * re-interview must not double the device. */
    zb_device_t a2 = mk(1, 5);
    snprintf(a2.name, sizeof a2.name, "renamed");
    assert(zb_store_upsert(&t, &a2) == 0);
    assert(t.count == 2);
    assert(t.dev[0].caps[0] == 5);
    assert(strcmp(t.dev[0].name, "renamed") == 0);

    /* --- serialise / deserialise round trip --- */
    uint8_t buf[ZB_STORE_IMAGE_MAX];
    size_t n = zb_store_serialize(&t, buf, sizeof buf);
    assert(n > 0 && n <= sizeof buf);
    zb_table_t r;
    assert(zb_store_deserialize(&r, buf, n));
    assert(r.count == 2);
    assert(zb_store_find(&r, a.eui64) == 0);
    assert(strcmp(r.dev[0].name, "renamed") == 0);
    assert(r.dev[1].short_addr == b.short_addr);
    assert(r.dev[0].cap_clusters[0] == 0x0402);

    /* --- corruption is refused, never half-loaded --- */
    zb_table_t bad;
    assert(!zb_store_deserialize(&bad, buf, n - 1));    /* truncated */
    uint8_t wrong[ZB_STORE_IMAGE_MAX];
    memcpy(wrong, buf, n);
    wrong[0] ^= 0xFF;                                    /* broken magic */
    assert(!zb_store_deserialize(&bad, wrong, n));
    memcpy(wrong, buf, n);
    wrong[4] = 0x7F;                                     /* wrong version */
    assert(!zb_store_deserialize(&bad, wrong, n));

    /* --- cap_count / action_count bounds are enforced on deserialise
     * (Task 3 fix round 1): a per-record count beyond the fixed-size
     * caps[]/actions[] arrays it indexes is as impossible as a bad table
     * count, and must be REJECTED rather than clamped -- clamping would
     * silently alter what the file said. --- */
    {
        /* Offsets within a ZB_STORE_RECORD_SIZE record, per zb_store.h's
         * field list: eui64 8 + short_addr 2 + endpoint 1 + interviewed 1
         * -> cap_count @ 12; + caps 4 + cap_clusters 8 -> action_count @ 25. */
        const size_t rec0 = 8;                 /* header size */
        const size_t cap_count_off = rec0 + 12;
        const size_t action_count_off = rec0 + 25;

        uint8_t corrupt[ZB_STORE_IMAGE_MAX];
        memcpy(corrupt, buf, n);
        corrupt[cap_count_off] = ZB_STORE_MAX_CAPS + 1;
        assert(!zb_store_deserialize(&bad, corrupt, n));

        memcpy(corrupt, buf, n);
        corrupt[action_count_off] = ZB_STORE_MAX_ACTIONS + 1;
        assert(!zb_store_deserialize(&bad, corrupt, n));

        /* The maximum LEGAL values must still load -- the new guard must
         * not be off by one and reject a fully-populated device. */
        zb_device_t full = mk(9, 1);
        full.cap_count = ZB_STORE_MAX_CAPS;
        for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
            full.caps[i] = (uint8_t)i;
            full.cap_clusters[i] = (uint16_t)(0x0400 + i);
        }
        full.action_count = ZB_STORE_MAX_ACTIONS;
        for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
            full.actions[i] = (uint8_t)i;
        }
        zb_table_t legal;
        zb_store_init(&legal);
        assert(zb_store_upsert(&legal, &full) == 0);
        uint8_t lbuf[ZB_STORE_IMAGE_MAX];
        size_t ln = zb_store_serialize(&legal, lbuf, sizeof lbuf);
        assert(ln > 0);
        zb_table_t legal_r;
        assert(zb_store_deserialize(&legal_r, lbuf, ln));
        assert(legal_r.dev[0].cap_count == ZB_STORE_MAX_CAPS);
        assert(legal_r.dev[0].action_count == ZB_STORE_MAX_ACTIONS);
    }

    /* --- remove --- */
    assert(zb_store_remove(&t, a.eui64));
    assert(t.count == 1);
    assert(zb_store_find(&t, a.eui64) == -1);
    assert(zb_store_find(&t, b.eui64) == 0);   /* survivor compacted down */
    assert(!zb_store_remove(&t, a.eui64));     /* already gone */

    /* --- full table refuses rather than overwrites --- */
    zb_store_init(&t);
    for (int i = 0; i < ZB_STORE_MAX_DEVICES; i++) {
        zb_device_t d = mk((uint8_t)(i + 1), 1);
        assert(zb_store_upsert(&t, &d) == i);
    }
    zb_device_t overflow = mk(200, 1);
    assert(zb_store_upsert(&t, &overflow) == -1);
    assert(t.count == ZB_STORE_MAX_DEVICES);

    printf("test_zb_store: OK\n");
    return 0;
}
