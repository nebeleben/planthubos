/* bridge_table.c -- see bridge_table.h for the why. Pure C, no ESP-IDF;
 * swarm.c owns all file I/O and locking.
 */
#include "bridge_table.h"
#include <string.h>

void bridge_table_init(bridge_table_t *t)
{
    memset(t, 0, sizeof(*t));
}

bridge_node_t *bridge_table_node(bridge_table_t *t, const uint8_t mac[6], bool create)
{
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        if (t->n[i].in_use && memcmp(t->n[i].mac, mac, 6) == 0) return &t->n[i];
    }
    if (!create) return NULL;
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        if (!t->n[i].in_use) {
            memset(&t->n[i], 0, sizeof(t->n[i]));
            t->n[i].in_use = true;
            memcpy(t->n[i].mac, mac, 6);
            return &t->n[i];
        }
    }
    return NULL;   /* every node slot already in use */
}

static int find_device_idx(const bridge_node_t *n, const swarm_dev_addr_t *dev)
{
    for (int i = 0; i < n->count; i++) {
        if (n->dev[i].dev.kind == dev->kind &&
            memcmp(n->dev[i].dev.addr, dev->addr, SWARM_ADDR_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

bool bridge_table_upsert(bridge_table_t *t, const uint8_t mac[6], const swarm_device_announce_t *a)
{
    bridge_node_t *n = bridge_table_node(t, mac, true);
    if (!n) return false;   /* node table full */

    int idx = find_device_idx(n, &a->dev);
    if (idx >= 0) {
        n->dev[idx] = *a;
        return true;
    }
    if (n->count >= BRIDGE_MAX_DEVICES) return false;   /* this node's device table is full */
    n->dev[n->count++] = *a;
    return true;
}

bool bridge_table_remove(bridge_table_t *t, const uint8_t mac[6], const swarm_dev_addr_t *dev)
{
    bridge_node_t *n = bridge_table_node(t, mac, false);
    if (!n) return false;
    int idx = find_device_idx(n, dev);
    if (idx < 0) return false;
    for (int i = idx; i < n->count - 1; i++) n->dev[i] = n->dev[i + 1];
    n->count--;
    memset(&n->dev[n->count], 0, sizeof(n->dev[n->count]));
    return true;
}

void bridge_table_forget_node(bridge_table_t *t, const uint8_t mac[6])
{
    bridge_node_t *n = bridge_table_node(t, mac, false);
    if (!n) return;
    memset(n, 0, sizeof(*n));
}

const bridge_node_t *bridge_table_find_device(const bridge_table_t *t, const swarm_dev_addr_t *dev)
{
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        const bridge_node_t *n = &t->n[i];
        if (!n->in_use) continue;
        if (find_device_idx(n, dev) >= 0) return n;
    }
    return NULL;
}

/* ---- serialize/deserialize: magic 'B','T',1 + node_count, then per
 * in-use node: mac[6], status_valid, status (7 raw bytes, same field order
 * as swarm_coord_status_t -- NOT swarm_encode_coord_status(), which would
 * additionally prepend a 4-byte wire header this on-disk image has no use
 * for), device count, then each device as swarm_encode_device_announce()'s
 * full wire frame, itself prefixed by a 1-byte length (a device's encoded
 * frame is well under 255 bytes: 4-byte header + up to 8+1 addr + 2 fixed
 * + 1+24 name + 1+4*3 caps + 1+2 actions = 56 bytes worst case). ---- */

#define BRIDGE_STORE_MAGIC0 'B'
#define BRIDGE_STORE_MAGIC1 'T'
#define BRIDGE_STORE_VERSION 1
#define BRIDGE_DEV_ENCODE_MAX 96   /* generous margin over the ~56-byte worst case above */

typedef struct { uint8_t *p; size_t cap; size_t n; bool ok; } bw_t;
static void bw8(bw_t *w, uint8_t v) { if (w->n + 1 > w->cap) { w->ok = false; return; } w->p[w->n++] = v; }
static void bw16(bw_t *w, uint16_t v) { bw8(w, (uint8_t)v); bw8(w, (uint8_t)(v >> 8)); }
static void bwbytes(bw_t *w, const void *s, size_t n) {
    if (w->n + n > w->cap) { w->ok = false; return; }
    memcpy(w->p + w->n, s, n);
    w->n += n;
}

typedef struct { const uint8_t *p; size_t len; size_t i; bool ok; } br_t;
static uint8_t br8(br_t *r) { if (r->i + 1 > r->len) { r->ok = false; return 0; } return r->p[r->i++]; }
static uint16_t br16(br_t *r) { uint16_t lo = br8(r); uint16_t hi = br8(r); return (uint16_t)(lo | (hi << 8)); }
static void brbytes(br_t *r, void *d, size_t n) {
    if (r->i + n > r->len) { r->ok = false; return; }
    memcpy(d, r->p + r->i, n);
    r->i += n;
}

size_t bridge_table_serialize(const bridge_table_t *t, uint8_t *buf, size_t cap)
{
    if (!t || !buf) return 0;

    uint8_t node_count = 0;
    for (int i = 0; i < BRIDGE_MAX_NODES; i++) {
        if (t->n[i].in_use) node_count++;
    }

    bw_t w = { buf, cap, 0, true };
    bw8(&w, BRIDGE_STORE_MAGIC0);
    bw8(&w, BRIDGE_STORE_MAGIC1);
    bw8(&w, BRIDGE_STORE_VERSION);
    bw8(&w, node_count);

    for (int i = 0; i < BRIDGE_MAX_NODES && w.ok; i++) {
        const bridge_node_t *n = &t->n[i];
        if (!n->in_use) continue;

        bwbytes(&w, n->mac, 6);
        bw8(&w, n->status_valid ? 1 : 0);
        bw8(&w, n->status.radio_role);
        bw8(&w, n->status.formed);
        bw8(&w, n->status.channel);
        bw16(&w, n->status.pan_id);
        bw8(&w, n->status.permit_s);
        bw8(&w, n->status.device_count);
        bw8(&w, n->count);

        for (int j = 0; j < n->count && w.ok; j++) {
            uint8_t enc[BRIDGE_DEV_ENCODE_MAX];
            size_t elen = swarm_encode_device_announce(&n->dev[j], enc, sizeof(enc));
            if (elen == 0 || elen > 0xFF) { w.ok = false; break; }
            bw8(&w, (uint8_t)elen);
            bwbytes(&w, enc, elen);
        }
    }

    return w.ok ? w.n : 0;
}

bool bridge_table_deserialize(bridge_table_t *t, const uint8_t *buf, size_t len)
{
    if (!t || !buf) return false;

    br_t r = { buf, len, 0, true };
    uint8_t m0 = br8(&r), m1 = br8(&r), ver = br8(&r);
    if (!r.ok || m0 != BRIDGE_STORE_MAGIC0 || m1 != BRIDGE_STORE_MAGIC1 || ver != BRIDGE_STORE_VERSION) {
        return false;
    }
    uint8_t node_count = br8(&r);
    if (!r.ok || node_count > BRIDGE_MAX_NODES) return false;

    bridge_table_t tmp;
    bridge_table_init(&tmp);

    for (uint8_t i = 0; i < node_count; i++) {
        bridge_node_t *n = &tmp.n[i];
        n->in_use = true;
        brbytes(&r, n->mac, 6);
        uint8_t status_valid = br8(&r);
        n->status_valid = (status_valid != 0);
        n->status.radio_role = br8(&r);
        n->status.formed = br8(&r);
        n->status.channel = br8(&r);
        n->status.pan_id = br16(&r);
        n->status.permit_s = br8(&r);
        n->status.device_count = br8(&r);
        uint8_t count = br8(&r);
        if (!r.ok || count > BRIDGE_MAX_DEVICES) return false;
        n->count = count;

        for (uint8_t j = 0; j < count; j++) {
            uint8_t elen = br8(&r);
            if (!r.ok || elen > r.len - r.i) return false;
            if (!swarm_decode_device_announce(r.p + r.i, elen, &n->dev[j])) return false;
            r.i += elen;
        }
    }

    if (!r.ok || r.i != r.len) return false;   /* reject a short or trailing-bytes buffer */

    *t = tmp;
    return true;
}
