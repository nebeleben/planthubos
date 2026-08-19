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
 * version check below rather than misread against the new layout. */
#define ZB_STORE_VERSION 2
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
    p = put_u8(p, d->action_count);
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = put_u8(p, d->actions[i]);
    }
    p = put_u8(p, d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = put_u16le(p, d->unmapped_clusters[i]);
    }
    memset(p, 0, ZB_STORE_NAME_MAX);
    size_t nlen = strnlen(d->name, ZB_STORE_NAME_MAX - 1);
    memcpy(p, d->name, nlen);
    p += ZB_STORE_NAME_MAX;
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
    p = get_u8(p, &d->action_count);
    for (int i = 0; i < ZB_STORE_MAX_ACTIONS; i++) {
        p = get_u8(p, &d->actions[i]);
    }
    p = get_u8(p, &d->unmapped_count);
    for (int i = 0; i < ZB_STORE_MAX_UNMAPPED; i++) {
        p = get_u16le(p, &d->unmapped_clusters[i]);
    }
    memcpy(d->name, p, ZB_STORE_NAME_MAX);
    d->name[ZB_STORE_NAME_MAX - 1] = '\0';
    p += ZB_STORE_NAME_MAX;
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
    if (buf[4] != ZB_STORE_VERSION) {
        return false;
    }
    uint8_t count = buf[5];
    if (count > ZB_STORE_MAX_DEVICES) {
        return false;
    }
    size_t need = ZB_STORE_HEADER_SIZE + (size_t)count * ZB_STORE_RECORD_SIZE;
    if (len != need) {
        return false;
    }

    zb_table_t out;
    memset(&out, 0, sizeof out);
    out.count = count;
    const uint8_t *p = buf + ZB_STORE_HEADER_SIZE;
    for (int i = 0; i < count; i++) {
        p = get_record(p, &out.dev[i]);
        /* A cap_count/action_count beyond the fixed-size arrays they index
         * is as impossible as a bad table count -- the field exists so a
         * consumer can loop `for (i = 0; i < d->cap_count; i++)` over
         * caps[]/actions[], and a corrupted-but-length-valid file must not
         * hand back a zb_device_t that breaks that invariant. Reject the
         * whole file rather than clamp: clamping would silently alter what
         * the file said; *t stays untouched either way. */
        if (out.dev[i].cap_count > ZB_STORE_MAX_CAPS ||
            out.dev[i].action_count > ZB_STORE_MAX_ACTIONS ||
            out.dev[i].unmapped_count > ZB_STORE_MAX_UNMAPPED) {
            return false;
        }
    }
    *t = out;
    return true;
}
