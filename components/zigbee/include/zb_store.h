/* zb_store.h -- the joined-device table (M6b spec section 4).
 *
 * PlantHub's half of a two-store split. The Zigbee stack's own zb_storage
 * partition holds the NETWORK -- keys, PAN id, short addresses, bindings.
 * This holds the INTERVIEW RESULT: which endpoint and clusters a device
 * has, which capability and action ids the auto-map gave them, and the
 * user's name for it. Neither subsumes the other: the stack can route a
 * frame but knows nothing about soil.moisture; this knows the mapping but
 * cannot route.
 *
 * Keyed on EUI-64 and never on a registry index. M5b learned that the hard
 * way -- the same valve came back as dev=3, dev=0 and dev=1 on successive
 * boots, which is why its persisted obligations key on device_id_t too.
 *
 * Pure: no file I/O and no ESP-IDF. zigbee.c owns the tmp+rename write,
 * the same atomicity actor_persist.c uses.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ZB_STORE_MAX_DEVICES 16
#define ZB_STORE_MAX_CAPS    4
#define ZB_STORE_MAX_ACTIONS 2
#define ZB_STORE_NAME_MAX    24

typedef struct {
    uint8_t  eui64[8];
    uint16_t short_addr;
    uint8_t  endpoint;
    uint8_t  interviewed;                          /* 0 = joined, not interviewed */
    uint8_t  cap_count;
    uint8_t  caps[ZB_STORE_MAX_CAPS];              /* capability ids */
    uint16_t cap_clusters[ZB_STORE_MAX_CAPS];      /* the cluster each came from */
    uint8_t  action_count;
    uint8_t  actions[ZB_STORE_MAX_ACTIONS];        /* action ids */
    char     name[ZB_STORE_NAME_MAX];
} zb_device_t;

typedef struct {
    uint8_t     count;
    zb_device_t dev[ZB_STORE_MAX_DEVICES];
} zb_table_t;

/* Serialised image: 8-byte header + count * ZB_STORE_RECORD_SIZE.
 *
 * The record is written field-by-field, little-endian, and is EXACTLY:
 *   eui64 8 + short_addr 2 + endpoint 1 + interviewed 1 + cap_count 1
 *   + caps 4 + cap_clusters 8 + action_count 1 + actions 2 + name 24 = 52.
 * Not sizeof(zb_device_t): struct padding is not a file format, and a
 * compiler or field-order change would silently invalidate every stored
 * file. zb_store_deserialize() requires len to equal the header plus
 * count * this exactly, which is what makes a truncated file detectable. */
#define ZB_STORE_RECORD_SIZE 52
#define ZB_STORE_IMAGE_MAX   (8 + ZB_STORE_MAX_DEVICES * ZB_STORE_RECORD_SIZE)

void zb_store_init(zb_table_t *t);

/* Index of eui64, or -1. */
int  zb_store_find(const zb_table_t *t, const uint8_t eui64[8]);

/* Inserts d, or REPLACES the existing entry with the same EUI-64 in place.
 * Returns its index, or -1 when the table is full (an existing entry is
 * never evicted to make room -- a silent eviction would orphan a device). */
int  zb_store_upsert(zb_table_t *t, const zb_device_t *d);

/* Removes eui64, compacting the survivors down. Returns false if absent. */
bool zb_store_remove(zb_table_t *t, const uint8_t eui64[8]);

/* Writes the image, returns its length, or 0 if buf is too small. */
size_t zb_store_serialize(const zb_table_t *t, uint8_t *buf, size_t cap);

/* Loads an image. Returns false -- leaving *t untouched -- on a bad magic,
 * an unknown version, a truncated buffer or an impossible count. A partly
 * loaded table is worse than none: it would silently drop devices. */
bool zb_store_deserialize(zb_table_t *t, const uint8_t *buf, size_t len);
