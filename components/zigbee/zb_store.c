/* zb_store.c -- the joined-device table (M6b spec section 4). See
 * zb_store.h for the why: this is PlantHub's half of a two-store split,
 * keyed on EUI-64 because M5b proved registry indices move across boots.
 *
 * Pure: no file I/O and no ESP-IDF. zigbee.c owns the tmp+rename write.
 */
#include "zb_store.h"
#include <string.h>

#define ZB_STORE_MAGIC0 'P'
#define ZB_STORE_MAGIC1 'H'
#define ZB_STORE_MAGIC2 'Z'
#define ZB_STORE_MAGIC3 'B'
/* Task 13 grew the record with unmapped_count/unmapped_clusters (52 -> 65
 * bytes, see zb_store.h) -- bumped so an old file is rejected by the
 * version check below rather than misread against the new layout.
 *
 * Multi-endpoint caps/actions grew the record again, 65 -> 91 bytes (new
 * cap_endpoints[]/action_endpoints[], wider caps[]/actions[] -- see
 * zb_store.h), and bumped this to 3. Unlike the 2->... jump, a v2 file is
 * NOT rejected outright: zb_store_deserialize() reads it with the old
 * 65-byte layout via get_record_v2() and fans its single `endpoint` into
 * every new array slot, so an existing hub's persisted devices survive
 * the upgrade instead of coming back empty.
 *
 * Power metering grew the record again, 91 -> 92 bytes (a trailing
 * meter_state byte -- see zb_store.h), and bumped this to 4. A v3 file is
 * likewise not rejected: it is read via get_record_v3(), which defaults
 * meter_state to ZB_METER_UNKNOWN. */
#define ZB_STORE_VERSION 4
/* The exact v2 (pre-multi-endpoint) record layout, kept only so
 * get_record_v2() can read an old file: eui64 8 + short_addr 2
 * + endpoint 1 + interviewed 1 + cap_count 1 + caps 4 + cap_clusters 8
 * + action_count 1 + actions 2 + unmapped_count 1 + unmapped_clusters 12
 * + name 24 = 65. */
#define ZB_STORE_V2_MAX_CAPS    4
#define ZB_STORE_V2_MAX_ACTIONS 2
#define ZB_STORE_V2_RECORD_SIZE 65
/* The exact v3 (pre-power-metering) record layout, kept only so
 * get_record_v3() can read an old file: identical to the current record
 * minus the trailing meter_state byte -- see zb_store.h. */
#define ZB_STORE_V3_RECORD_SIZE 91
#define ZB_STORE_HEADER_SIZE 8

void zb_store_init(zb_table_t *t) {
    memset(t, 0, sizeof *t);
    t->count = 0;
}

int zb_store_find(const zb_table_t *t, const uint8_t eui64[8]) {
    for (int i = 0; i < t->count; i++) {
        if (memcmp(t->dev[i].eui64, eui64, 8) == 0) {
            return i;
        }
    }
    return -1;
}

int zb_store_upsert(zb_table_t *t, const zb_device_t *d) {
    int idx = zb_store_find(t, d->eui64);
    if (idx >= 0) {
        t->dev[idx] = *d;
        return idx;
    }
    if (t->count >= ZB_STORE_MAX_DEVICES) {
        /* Never evict to make room: an eviction would silently orphan
         * whichever device lost its slot. */
        return -1;
    }
    idx = t->count;
    t->dev[idx] = *d;
    t->count++;
    return idx;
}

bool zb_store_remove(zb_table_t *t, const uint8_t eui64[8]) {
    int idx = zb_store_find(t, eui64);
    if (idx < 0) {
        return false;
    }
    for (int i = idx; i < t->count - 1; i++) {
        t->dev[i] = t->dev[i + 1];
    }
    t->count--;
    memset(&t->dev[t->count], 0, sizeof t->dev[t->count]);
    return true;
}

static uint8_t *put_u8(uint8_t *p, uint8_t v) {
    p[0] = v;
    return p + 1;
}

static uint8_t *put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    return p + 2;
}

static const uint8_t *get_u8(const uint8_t *p, uint8_t *v) {
    *v = p[0];
    return p + 1;
}

static const uint8_t *get_u16le(const uint8_t *p, uint16_t *v) {
    *v = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    return p + 2;
}

static uint8_t *put_record(uint8_t *p, const zb_device_t *d) {
    memcpy(p, d->eui64, 8);
    p += 8;
    p = put_u16le(p, d->short_addr);
    p = put_u8(p, d->endpoint);
    p = put_u8(p, d->interviewed);
    p = put_u8(p, d->cap_count);
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = put_u8(p, d->caps[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = put_u16le(p, d->cap_clusters[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = put_u8(p, d->cap_endpoints[i]);
    }
    p = put_u8(p, d->action_count);
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = put_u8(p, d->actions[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = put_u8(p, d->action_endpoints[i]);
    }
    p = put_u8(p, d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = put_u16le(p, d->unmapped_clusters[i]);
    }
    memset(p, 0, ZB_STORE_NAME_MAX);
    size_t nlen = strnlen(d->name, ZB_STORE_NAME_MAX - 1);
    memcpy(p, d->name, nlen);
    p += ZB_STORE_NAME_MAX;
    p = put_u8(p, d->meter_state);
    return p;
}

static const uint8_t *get_record(const uint8_t *p, zb_device_t *d) {
    memcpy(d->eui64, p, 8);
    p += 8;
    p = get_u16le(p, &d->short_addr);
    p = get_u8(p, &d->endpoint);
    p = get_u8(p, &d->interviewed);
    p = get_u8(p, &d->cap_count);
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u8(p, &d->caps[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u16le(p, &d->cap_clusters[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u8(p, &d->cap_endpoints[i]);
    }
    p = get_u8(p, &d->action_count);
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->actions[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->action_endpoints[i]);
    }
    p = get_u8(p, &d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = get_u16le(p, &d->unmapped_clusters[i]);
    }
    memcpy(d->name, p, ZB_STORE_NAME_MAX);
    d->name[ZB_STORE_NAME_MAX - 1] = '\0';
    p += ZB_STORE_NAME_MAX;
    p = get_u8(p, &d->meter_state);
    return p;
}

/* Reads one v3 (91-byte, pre-power-metering) record. `d` must already be
 * zeroed by the caller. Identical to get_record() through the name
 * field; v3 had no meter_state byte, so it is defaulted to
 * ZB_METER_UNKNOWN rather than left whatever the caller's zero-fill
 * happened to produce (they coincide today, but the default should not
 * depend on that). */
static const uint8_t *get_record_v3(const uint8_t *p, zb_device_t *d) {
    memcpy(d->eui64, p, 8);
    p += 8;
    p = get_u16le(p, &d->short_addr);
    p = get_u8(p, &d->endpoint);
    p = get_u8(p, &d->interviewed);
    p = get_u8(p, &d->cap_count);
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u8(p, &d->caps[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u16le(p, &d->cap_clusters[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_CAPS; i++) {
        p = get_u8(p, &d->cap_endpoints[i]);
    }
    p = get_u8(p, &d->action_count);
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->actions[i]);
    }
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->action_endpoints[i]);
    }
    p = get_u8(p, &d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = get_u16le(p, &d->unmapped_clusters[i]);
    }
    memcpy(d->name, p, ZB_STORE_NAME_MAX);
    d->name[ZB_STORE_NAME_MAX - 1] = '\0';
    p += ZB_STORE_NAME_MAX;
    d->meter_state = ZB_METER_UNKNOWN;
    return p;
}

/* Reads one v2 (65-byte, pre-multi-endpoint) record. `d` must already be
 * zeroed by the caller -- this only touches the old, narrower
 * caps[0..3]/cap_clusters[0..3]/actions[0..1] slots, leaving the new
 * cap_endpoints[]/action_endpoints[]/wider caps[]/actions[] slots at
 * their zeroed default until the fan-out below sets the ones that
 * cap_count/action_count actually cover. Every cap/action a v2 record
 * names is fanned onto the device's one scalar `endpoint`, since v2 had
 * no per-endpoint concept at all. */
static const uint8_t *get_record_v2(const uint8_t *p, zb_device_t *d) {
    memcpy(d->eui64, p, 8);
    p += 8;
    p = get_u16le(p, &d->short_addr);
    p = get_u8(p, &d->endpoint);
    p = get_u8(p, &d->interviewed);
    p = get_u8(p, &d->cap_count);
    for (int i = 0; i < ZB_STORE_V2_MAX_CAPS; i++) {
        p = get_u8(p, &d->caps[i]);
    }
    for (int i = 0; i < ZB_STORE_V2_MAX_CAPS; i++) {
        p = get_u16le(p, &d->cap_clusters[i]);
    }
    p = get_u8(p, &d->action_count);
    for (int i = 0; i < ZB_STORE_V2_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->actions[i]);
    }
    p = get_u8(p, &d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = get_u16le(p, &d->unmapped_clusters[i]);
    }
    memcpy(d->name, p, ZB_STORE_NAME_MAX);
    d->name[ZB_STORE_NAME_MAX - 1] = '\0';
    p += ZB_STORE_NAME_MAX;

    for (int i = 0; i < d->cap_count && i < ZB_STORE_MAX_CAPS; i++) {
        d->cap_endpoints[i] = d->endpoint;
    }
    for (int i = 0; i < d->action_count && i < ZB_STORE_MAX_ACTIONS; i++) {
        d->action_endpoints[i] = d->endpoint;
    }
    return p;
}

size_t zb_store_serialize(const zb_table_t *t, uint8_t *buf, size_t cap) {
    size_t need = ZB_STORE_HEADER_SIZE + (size_t)t->count * ZB_STORE_RECORD_SIZE;
    if (need > cap) {
        return 0;
    }
    uint8_t *p = buf;
    p = put_u8(p, ZB_STORE_MAGIC0);
    p = put_u8(p, ZB_STORE_MAGIC1);
    p = put_u8(p, ZB_STORE_MAGIC2);
    p = put_u8(p, ZB_STORE_MAGIC3);
    p = put_u8(p, ZB_STORE_VERSION);
    p = put_u8(p, t->count);
    p = put_u8(p, 0);
    p = put_u8(p, 0);
    for (int i = 0; i < t->count; i++) {
        p = put_record(p, &t->dev[i]);
    }
    return (size_t)(p - buf);
}

bool zb_store_deserialize(zb_table_t *t, const uint8_t *buf, size_t len) {
    if (len < ZB_STORE_HEADER_SIZE) {
        return false;
    }
    if (buf[0] != ZB_STORE_MAGIC0 || buf[1] != ZB_STORE_MAGIC1 ||
        buf[2] != ZB_STORE_MAGIC2 || buf[3] != ZB_STORE_MAGIC3) {
        return false;
    }
    /* v2 (pre-multi-endpoint, 65-byte record) and v3 (pre-power-metering,
     * 91-byte record) are not rejected outright: each is read via its own
     * legacy layout below -- v2 fanning its single `endpoint` into the
     * new per-cap/per-action arrays, v3 defaulting the new meter_state --
     * so an existing hub's persisted devices survive the upgrade rather
     * than the table coming back empty. Any OTHER unknown version is
     * still rejected -- there is no layout to read it with. */
    uint8_t ver = buf[4];
    if (ver != ZB_STORE_VERSION && ver != 3 && ver != 2) {
        return false;
    }
    uint8_t count = buf[5];
    if (count > ZB_STORE_MAX_DEVICES) {
        return false;
    }
    size_t rec = (ver == 2) ? ZB_STORE_V2_RECORD_SIZE
               : (ver == 3) ? ZB_STORE_V3_RECORD_SIZE
               : ZB_STORE_RECORD_SIZE;
    size_t need = ZB_STORE_HEADER_SIZE + (size_t)count * rec;
    if (len != need) {
        return false;
    }

    uint8_t max_caps = (ver == 2) ? ZB_STORE_V2_MAX_CAPS : ZB_STORE_MAX_CAPS;
    uint8_t max_actions = (ver == 2) ? ZB_STORE_V2_MAX_ACTIONS : ZB_STORE_MAX_ACTIONS;

    zb_table_t out;
    memset(&out, 0, sizeof out);
    out.count = count;
    const uint8_t *p = buf + ZB_STORE_HEADER_SIZE;
    for (int i = 0; i < count; i++) {
        p = (ver == 2) ? get_record_v2(p, &out.dev[i])
          : (ver == 3) ? get_record_v3(p, &out.dev[i])
          : get_record(p, &out.dev[i]);
        /* A cap_count/action_count beyond the fixed-size arrays they index
         * is as impossible as a bad table count -- the field exists so a
         * consumer can loop `for (i = 0; i < d->cap_count; i++)` over
         * caps[]/actions[], and a corrupted-but-length-valid file must not
         * hand back a zb_device_t that breaks that invariant. Reject the
         * whole file rather than clamp: clamping would silently alter what
         * the file said; *t stays untouched either way. Bounds are
         * per-version: a v2 record's real array width was narrower
         * (caps[4]/actions[2]) than v3's, so it is checked against those,
         * not v3's wider limits. */
        if (out.dev[i].cap_count > max_caps ||
            out.dev[i].action_count > max_actions ||
            out.dev[i].unmapped_count > ZB_STORE_MAX_UNMAPPED) {
            return false;
        }
    }
    *t = out;
    return true;
}
