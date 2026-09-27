/* tests/host/test_registry_persist.c -- pure serialize/deserialize core. */
#include "registry_persist.h"
#include "registry.h"
#include "capability.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static device_id_t mkid(uint8_t kind, uint8_t b0) {
    device_id_t id; memset(&id, 0, sizeof id);
    id.kind = kind; id.addr[0] = b0; id.addr[7] = 0xAB;
    return id;
}

static void set_cap(device_entry_t *d, uint8_t cap, int16_t raw) {
    d->caps[cap].raw = raw; d->caps[cap].updated_s = 123; d->caps[cap].valid = true;
}

/* Build a registry: dev 0 = two caps + via/rssi; dev 2 = one cap; dev 1 unused;
 * dev 3 in_use but one invalid cap slot (must not survive). */
static void seed(registry_t *r) {
    registry_init(r);
    r->devices[0].in_use = true; r->devices[0].id = mkid(1, 0x10);
    r->devices[0].last_seen_s = 999;
    set_cap(&r->devices[0], 1, 2955);   /* air.temperature raw */
    set_cap(&r->devices[0], 4, 90);     /* battery.level */
    r->devices[0].via_node_valid = true;
    memcpy(r->devices[0].via_node, (uint8_t[]){1,2,3,4,5,6}, 6);
    r->devices[0].best_rssi = -71;

    r->devices[2].in_use = true; r->devices[2].id = mkid(2, 0x20);
    set_cap(&r->devices[2], 5, 4470);   /* air.humidity */

    r->devices[3].in_use = true; r->devices[3].id = mkid(1, 0x30);
    r->devices[3].caps[2].raw = 5; r->devices[3].caps[2].valid = false;  /* invalid: skipped */
}

static int in_use_count(const registry_t *r) {
    int n = 0;
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) if (r->devices[i].in_use) n++;
    return n;
}
static const device_entry_t *find(const registry_t *r, uint8_t kind, uint8_t b0) {
    for (int i = 0; i < REGISTRY_MAX_DEVICES; i++) {
        const device_entry_t *d = &r->devices[i];
        if (d->in_use && d->id.kind == kind && d->id.addr[0] == b0) return d;
    }
    return NULL;
}

static void test_round_trip(void) {
    registry_t r; seed(&r);
    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    size_t n = registry_persist_serialize(&r, 1700000000u, buf, sizeof buf);
    assert(n > 0 && n <= sizeof buf);

    registry_t out; uint32_t epoch = 0;
    assert(registry_persist_deserialize(buf, n, &out, &epoch));
    assert(epoch == 1700000000u);

    /* dev 3 had no VALID caps and no via -> it still round-trips as a known
     * device with zero caps (it exists), so 3 in_use devices. */
    assert(in_use_count(&out) == 3);

    const device_entry_t *d0 = find(&out, 1, 0x10);
    assert(d0 && d0->snapshot_only);            /* restored rows are stale */
    assert(d0->caps[1].valid && d0->caps[1].raw == 2955);
    assert(d0->caps[4].valid && d0->caps[4].raw == 90);
    assert(!d0->caps[0].valid);                 /* never set */
    assert(d0->via_node_valid && d0->via_node[5] == 6 && d0->best_rssi == -71);

    const device_entry_t *d2 = find(&out, 2, 0x20);
    assert(d2 && d2->caps[5].valid && d2->caps[5].raw == 4470);

    const device_entry_t *d3 = find(&out, 1, 0x30);
    assert(d3 && !d3->caps[2].valid);           /* invalid cap dropped */

    /* uptime fields are meaningless across a reboot -> restored as 0. */
    assert(d0->last_seen_s == 0 && d0->caps[1].updated_s == 0);
}

static void test_empty(void) {
    registry_t r; registry_init(&r);
    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    size_t n = registry_persist_serialize(&r, 42, buf, sizeof buf);
    assert(n == REGISTRY_PERSIST_HEADER_LEN);   /* header only */
    registry_t out; uint32_t epoch = 1;
    assert(registry_persist_deserialize(buf, n, &out, &epoch));
    assert(in_use_count(&out) == 0 && epoch == 42);
}

static void test_crc_rejected(void) {
    registry_t r; seed(&r);
    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    size_t n = registry_persist_serialize(&r, 7, buf, sizeof buf);
    buf[n - 1] ^= 0xFF;                          /* corrupt a record byte */
    registry_t out; uint32_t epoch = 999;
    assert(!registry_persist_deserialize(buf, n, &out, &epoch));
    assert(in_use_count(&out) == 0);             /* left empty on reject */
}

static void test_version_rejected(void) {
    registry_t r; seed(&r);
    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    size_t n = registry_persist_serialize(&r, 7, buf, sizeof buf);
    buf[0] = REGISTRY_PERSIST_FMT + 1;           /* wrong format (also breaks crc) */
    registry_t out; uint32_t epoch;
    assert(!registry_persist_deserialize(buf, n, &out, &epoch));
}

static void test_truncation_rejected(void) {
    registry_t r; seed(&r);
    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    uint32_t epoch;
    size_t n = registry_persist_serialize(&r, 7, buf, sizeof buf);
    registry_t out;
    assert(!registry_persist_deserialize(buf, 4, &out, &epoch));      /* < header */
    assert(!registry_persist_deserialize(buf, n - 2, &out, &epoch));  /* mid-record */
}

static void test_buffer_too_small(void) {
    registry_t r; seed(&r);
    uint8_t tiny[8];
    assert(registry_persist_serialize(&r, 7, tiny, sizeof tiny) == 0);
    assert(registry_persist_serialize(NULL, 7, tiny, sizeof tiny) == 0);
}

/* --- FMT 2: extra-endpoint side-list round-trips through serialize/deserialize --- */
static void test_extra_ep_round_trip(void) {
    registry_t r; registry_init(&r);
    device_id_t z = mkid(2, 0x40);
    assert(registry_set_cap_ep(&r, &z, CAP_SWITCH_STATE, 1, 7, 100) >= 0);
    assert(registry_set_cap_ep(&r, &z, CAP_SWITCH_STATE, 2, 9, 101) >= 0);

    uint8_t buf[REGISTRY_PERSIST_MAX_BYTES];
    size_t n = registry_persist_serialize(&r, 555, buf, sizeof buf);
    assert(n > 0 && n <= sizeof buf);

    registry_t out; uint32_t epoch = 0;
    assert(registry_persist_deserialize(buf, n, &out, &epoch));

    const device_entry_t *d = find(&out, 2, 0x40);
    assert(d != NULL);
    assert(d->cap_endpoint[CAP_SWITCH_STATE] == 1);
    assert(d->caps[CAP_SWITCH_STATE].valid && d->caps[CAP_SWITCH_STATE].raw == 7);
    const cap_slot_t *ep2 = registry_get_cap_ep(&out, &z, CAP_SWITCH_STATE, 2);
    assert(ep2 && ep2->valid && ep2->raw == 9);
}

/* CRC-16/CCITT-FALSE, copied inline from registry_persist.c so this fixture
 * doesn't need a symbol the module doesn't export. */
static uint16_t fixture_crc16(uint16_t crc, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x1021u)
                                             : (uint16_t)(crc << 1));
    }
    return crc;
}

/* A hand-assembled FMT-1 image (old layout: 3-byte cap rows, no per-cap
 * endpoint byte, no extra-ep section) must still load -- an old snapshot on
 * disk from before this task must not be rejected, just treated as having
 * no endpoint info. */
static void test_fmt1_backward_compat(void) {
    uint8_t buf[64];
    memset(buf, 0, sizeof buf);
    size_t off = REGISTRY_PERSIST_HEADER_LEN;
    buf[off++] = 1;                       /* kind */
    memset(&buf[off], 0, 8); buf[off] = 0x50; off += 8;   /* addr, addr[0]=0x50 */
    buf[off++] = 0;                       /* via_valid */
    off += 6;                             /* via[6] left zero */
    buf[off++] = (uint8_t)(-60);          /* best_rssi */
    buf[off++] = 1;                       /* cap_count */
    buf[off++] = CAP_SWITCH_STATE;        /* cap_id */
    buf[off++] = 3; buf[off++] = 0;       /* raw = 3, little-endian */

    buf[0] = 1;                           /* fmt = FMT 1 (old format) */
    uint32_t epoch = 42;
    buf[3] = (uint8_t)(epoch & 0xFF);
    buf[4] = (uint8_t)((epoch >> 8) & 0xFF);
    buf[5] = (uint8_t)((epoch >> 16) & 0xFF);
    buf[6] = (uint8_t)((epoch >> 24) & 0xFF);
    buf[7] = 1;                           /* device count */

    uint16_t crc = fixture_crc16(0xFFFFu, &buf[0], 1);
    crc = fixture_crc16(crc, &buf[3], off - 3);
    buf[1] = (uint8_t)(crc & 0xFF);
    buf[2] = (uint8_t)(crc >> 8);

    registry_t out; uint32_t epoch_out = 0;
    assert(registry_persist_deserialize(buf, off, &out, &epoch_out));
    assert(epoch_out == 42);
    const device_entry_t *d = find(&out, 1, 0x50);
    assert(d != NULL);
    assert(d->caps[CAP_SWITCH_STATE].valid && d->caps[CAP_SWITCH_STATE].raw == 3);
    /* old snapshot carries no endpoint info -- untracked, not multi-endpoint */
    assert(d->extra_ep_cap_count == 0);
    assert(d->cap_endpoint[CAP_SWITCH_STATE] == 0);
}

int main(void) {
    test_round_trip();
    test_empty();
    test_crc_rejected();
    test_version_rejected();
    test_truncation_rejected();
    test_buffer_too_small();
    test_extra_ep_round_trip();
    test_fmt1_backward_compat();
    printf("test_registry_persist: OK\n");
    return 0;
}
