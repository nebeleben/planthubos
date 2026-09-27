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
    d.cap_endpoints[0] = 1;
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

    /* --- Task 13: unmapped_count/unmapped_clusters survive the round
     * trip too, alongside the mapped fields above. --- */
    {
        zb_device_t u = mk(50, 1);
        u.unmapped_count = 3;
        u.unmapped_clusters[0] = 0xEF00;
        u.unmapped_clusters[1] = 0xFC00;
        u.unmapped_clusters[2] = 0xFC01;
        zb_table_t ut;
        zb_store_init(&ut);
        assert(zb_store_upsert(&ut, &u) == 0);
        uint8_t ubuf[ZB_STORE_IMAGE_MAX];
        size_t un = zb_store_serialize(&ut, ubuf, sizeof ubuf);
        assert(un > 0);
        zb_table_t ur;
        assert(zb_store_deserialize(&ur, ubuf, un));
        assert(ur.dev[0].unmapped_count == 3);
        assert(ur.dev[0].unmapped_clusters[0] == 0xEF00);
        assert(ur.dev[0].unmapped_clusters[1] == 0xFC00);
        assert(ur.dev[0].unmapped_clusters[2] == 0xFC01);
    }

    /* --- per-endpoint caps/actions round trip (multi-endpoint) --- */
    {
        zb_device_t g = mk(60, 8);   /* two gangs, one EUI-64 */
        g.cap_count = 2;
        g.caps[0] = 8; g.cap_clusters[0] = 0x0006; g.cap_endpoints[0] = 1;
        g.caps[1] = 8; g.cap_clusters[1] = 0x0006; g.cap_endpoints[1] = 2;
        g.action_count = 4;
        g.actions[0] = 0; g.action_endpoints[0] = 1;   /* switch.on  ep1 */
        g.actions[1] = 1; g.action_endpoints[1] = 1;   /* switch.off ep1 */
        g.actions[2] = 0; g.action_endpoints[2] = 2;   /* switch.on  ep2 */
        g.actions[3] = 1; g.action_endpoints[3] = 2;   /* switch.off ep2 */
        zb_table_t gt; zb_store_init(&gt);
        assert(zb_store_upsert(&gt, &g) == 0);
        uint8_t gbuf[ZB_STORE_IMAGE_MAX];
        size_t gn = zb_store_serialize(&gt, gbuf, sizeof gbuf);
        assert(gn == 8 + ZB_STORE_RECORD_SIZE);
        zb_table_t gr;
        assert(zb_store_deserialize(&gr, gbuf, gn));
        assert(gr.dev[0].cap_endpoints[1] == 2);
        assert(gr.dev[0].action_endpoints[3] == 2);
    }

    /* --- legacy (v2, 65-byte) record migrates its single endpoint into
     * every cap_endpoints[]/action_endpoints[] slot -- a pre-multi-gang
     * file loads as if every cap/action lived on the device's one
     * endpoint. Hand-assembled v2 image: caps[4]/cap_clusters[4]/
     * actions[2], no cap_endpoints/action_endpoints, record 65. --- */
    {
        uint8_t v2[8 + 65];
        memset(v2, 0, sizeof v2);
        v2[0] = 'P'; v2[1] = 'H'; v2[2] = 'Z'; v2[3] = 'B';
        v2[4] = 2;              /* version */
        v2[5] = 1;              /* count */
        uint8_t *p = v2 + 8;
        const uint8_t eui[8] = { 0x00, 0x12, 0x4B, 0x00, 0x0A, 0x0B, 0x0C, 0x99 };
        memcpy(p, eui, 8); p += 8;
        p[0] = 0x34; p[1] = 0x12; p += 2;          /* short_addr = 0x1234 */
        *p++ = 7;                                   /* endpoint = 7 */
        *p++ = 1;                                    /* interviewed */
        *p++ = 2;                                    /* cap_count */
        p[0] = 8; p[1] = 0; p[2] = 0; p[3] = 0; p += 4;      /* caps[4] */
        p[0] = 0x06; p[1] = 0x00;                    /* cap_clusters[0] = 6 */
        p[2] = 0x06; p[3] = 0x00;                    /* cap_clusters[1] = 6 */
        p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0;
        p += 8;
        *p++ = 2;                                    /* action_count */
        p[0] = 0; p[1] = 1; p += 2;                  /* actions[2] */
        *p++ = 0;                                    /* unmapped_count */
        p += 12;                                     /* unmapped_clusters[6] */
        memcpy(p, "legacy", 6);
        p += ZB_STORE_NAME_MAX;
        assert((size_t)(p - v2) == sizeof v2);

        zb_table_t vt;
        assert(zb_store_deserialize(&vt, v2, sizeof v2));
        assert(vt.count == 1);
        assert(vt.dev[0].endpoint == 7);
        assert(vt.dev[0].cap_count == 2);
        assert(vt.dev[0].cap_endpoints[0] == 7);
        assert(vt.dev[0].cap_endpoints[1] == 7);
        assert(vt.dev[0].action_count == 2);
        assert(vt.dev[0].action_endpoints[0] == 7);
        assert(vt.dev[0].action_endpoints[1] == 7);
        assert(strcmp(vt.dev[0].name, "legacy") == 0);
    }

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
         * -> cap_count @ 12; + cap_count 1 + caps 6 + cap_clusters 12
         * + cap_endpoints 6 -> action_count @ 37; + action_count 1
         * + actions 8 + action_endpoints 8 -> unmapped_count @ 54
         * (multi-endpoint caps/actions, record v3). */
        const size_t rec0 = 8;                 /* header size */
        const size_t cap_count_off = rec0 + 12;
        const size_t action_count_off = rec0 + 37;
        const size_t unmapped_count_off = rec0 + 54;

        uint8_t corrupt[ZB_STORE_IMAGE_MAX];
        memcpy(corrupt, buf, n);
        corrupt[cap_count_off] = ZB_STORE_MAX_CAPS + 1;
        assert(!zb_store_deserialize(&bad, corrupt, n));

        memcpy(corrupt, buf, n);
        corrupt[action_count_off] = ZB_STORE_MAX_ACTIONS + 1;
        assert(!zb_store_deserialize(&bad, corrupt, n));

        /* Task 13: an out-of-range unmapped_count is rejected the same
         * way -- a corrupt-but-length-valid file must not be clamped. */
        memcpy(corrupt, buf, n);
        corrupt[unmapped_count_off] = ZB_STORE_MAX_UNMAPPED + 1;
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
        full.unmapped_count = ZB_STORE_MAX_UNMAPPED;
        for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
            full.unmapped_clusters[i] = (uint16_t)(0xFC00 + i);
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
        assert(legal_r.dev[0].unmapped_count == ZB_STORE_MAX_UNMAPPED);
        assert(legal_r.dev[0].unmapped_clusters[ZB_STORE_MAX_UNMAPPED - 1] ==
               (uint16_t)(0xFC00 + ZB_STORE_MAX_UNMAPPED - 1));
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

    /* --- power-metering: meter_state persists (record v4) --- */
    {
        zb_table_t t; memset(&t, 0, sizeof t);
        zb_device_t d; memset(&d, 0, sizeof d);
        uint8_t eui[8] = {1,2,3,4,5,6,7,8};
        memcpy(d.eui64, eui, 8); d.endpoint = 1; d.cap_count = 0;
        d.meter_state = ZB_METER_PRESENT;
        assert(zb_store_upsert(&t, &d) >= 0);
        uint8_t buf[ZB_STORE_IMAGE_MAX];
        size_t n = zb_store_serialize(&t, buf, sizeof buf);
        assert(n > 0);
        zb_table_t back; memset(&back, 0, sizeof back);
        assert(zb_store_deserialize(&back, buf, n));
        int i = zb_store_find(&back, eui);
        assert(i >= 0 && back.dev[i].meter_state == ZB_METER_PRESENT);
    }
    /* record size grew by exactly the meter_state byte (v3 91 -> v4 92) --
     * a historical fact about v4, pinned as the literal 92 below rather
     * than against ZB_STORE_RECORD_SIZE, which now names the *current*
     * (v5, 156-byte) record; see the v5 test below for the live
     * assertion against that macro. */

    /* --- legacy (v3, 91-byte) record defaults meter_state to
     * ZB_METER_UNKNOWN -- a pre-power-metering file loads as if the
     * field had never been probed. Hand-assembled v3 image: full
     * multi-endpoint width (caps[6]/cap_clusters[6]/cap_endpoints[6]/
     * actions[8]/action_endpoints[8]), no trailing meter_state byte,
     * record 91. --- */
    {
        uint8_t v3[8 + 91];             /* header + one v3 (91-byte) record */
        memset(v3, 0, sizeof v3);
        v3[0] = 'P'; v3[1] = 'H'; v3[2] = 'Z'; v3[3] = 'B';
        v3[4] = 3;              /* version */
        v3[5] = 1;              /* count */
        uint8_t *p = v3 + 8;
        const uint8_t eui[8] = { 0x00, 0x12, 0x4B, 0x00, 0x0A, 0x0B, 0x0C, 0xA0 };
        memcpy(p, eui, 8); p += 8;
        p[0] = 0x78; p[1] = 0x56; p += 2;          /* short_addr = 0x5678 */
        *p++ = 3;                                   /* endpoint = 3 */
        *p++ = 1;                                    /* interviewed */
        *p++ = 0;                                    /* cap_count */
        p += 6;                                       /* caps[6] */
        p += 12;                                      /* cap_clusters[6] */
        p += 6;                                       /* cap_endpoints[6] */
        *p++ = 0;                                    /* action_count */
        p += 8;                                       /* actions[8] */
        p += 8;                                       /* action_endpoints[8] */
        *p++ = 0;                                    /* unmapped_count */
        p += 12;                                      /* unmapped_clusters[6] */
        memcpy(p, "v3dev", 5);
        p += ZB_STORE_NAME_MAX;
        assert((size_t)(p - v3) == sizeof v3);
        assert(sizeof v3 == 8 + 92 - 1);  /* v3 (91) = v4 (92) minus meter_state */

        zb_table_t vt3;
        assert(zb_store_deserialize(&vt3, v3, sizeof v3));
        assert(vt3.count == 1);
        assert(vt3.dev[0].endpoint == 3);
        assert(strcmp(vt3.dev[0].name, "v3dev") == 0);
        assert(vt3.dev[0].meter_state == ZB_METER_UNKNOWN);
    }

    /* --- v5: identity round-trip --- */
    {
        zb_table_t t5; zb_store_init(&t5);
        zb_device_t d = mk(7, 0);
        snprintf(d.manufacturer, sizeof d.manufacturer, "_TZE284_o9ofysmo");
        snprintf(d.model, sizeof d.model, "TS0601");
        d.meter_state = 1;
        assert(zb_store_upsert(&t5, &d) == 0);
        uint8_t buf[ZB_STORE_IMAGE_MAX];
        size_t n = zb_store_serialize(&t5, buf, sizeof buf);
        assert(n == 8 + ZB_STORE_RECORD_SIZE);            /* one 156-byte record */
        assert(buf[4] == ZB_STORE_VERSION);               /* == 5 */
        zb_table_t r5; zb_store_init(&r5);
        assert(zb_store_deserialize(&r5, buf, n));
        assert(strcmp(r5.dev[0].manufacturer, "_TZE284_o9ofysmo") == 0);
        assert(strcmp(r5.dev[0].model, "TS0601") == 0);
        assert(r5.dev[0].meter_state == 1);
    }

    /* --- v4 legacy read: identity defaults empty --- */
    {
        /* Build a v4 image by hand: header with version 4 + one 92-byte
         * record. The record is a v5 serialize truncated to the pre-identity
         * length, with the version byte forced to 4. */
        zb_table_t t; zb_store_init(&t);
        zb_device_t d = mk(9, 5);
        assert(zb_store_upsert(&t, &d) == 0);
        uint8_t buf[ZB_STORE_IMAGE_MAX];
        size_t n = zb_store_serialize(&t, buf, sizeof buf);
        (void)n;
        /* forge a v4 image: version=4, record length 92 (drop the trailing 64
         * identity bytes of the single record) */
        uint8_t v4[8 + 92];
        memcpy(v4, buf, 8);
        v4[4] = 4;
        memcpy(v4 + 8, buf + 8, 92);
        zb_table_t r; zb_store_init(&r);
        assert(zb_store_deserialize(&r, v4, sizeof v4));
        assert(r.count == 1);
        assert(r.dev[0].manufacturer[0] == '\0');
        assert(r.dev[0].model[0] == '\0');
        assert(r.dev[0].caps[0] == 5);                    /* rest survived */
    }

    printf("test_zb_store: OK\n");
    return 0;
}
