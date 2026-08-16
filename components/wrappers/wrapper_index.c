/* wrapper_index.c -- the pure matcher core (M3 spec §2). Deliberately no
 * ESP-IDF/FreeRTOS/file-I/O includes, same pattern
 * components/ble_collector/adv_queue.c already uses in this codebase, so
 * the host test (tests/host/test_wrapper_index.c) compiles and exercises
 * this file directly. wrapper_store.c (LittleFS side) is the only caller
 * that ever populates a wrapper_index_t from flash; every other caller
 * (including the host test) can build one by hand with wrapper_index_add().
 *
 * Entries are kept packed in [0, count) -- wrapper_index_remove() shifts the
 * tail down rather than leaving a hole -- so lookup/add only ever need to
 * scan exactly ix->count entries, not the full WRAPPERS_MAX table. */
#include "wrapper_index.h"
#include <string.h>

_Static_assert(sizeof(wrapper_entry_t) == 12,
               "wrapper_entry_t must be 12 B (M3 spec section 2's 16 * 12 B budget)");

void wrapper_index_init(wrapper_index_t *ix)
{
    memset(ix, 0, sizeof(*ix));
}

static bool key_taken(const wrapper_index_t *ix, uint8_t kind, uint32_t key)
{
    for (uint8_t i = 0; i < ix->count; i++) {
        if (ix->e[i].kind == kind && ix->e[i].key == key) return true;
    }
    return false;
}

int wrapper_index_add(wrapper_index_t *ix, uint8_t kind, uint32_t key, uint16_t id)
{
    if (ix->count >= WRAPPERS_MAX) return -1;
    if (key_taken(ix, kind, key)) return -1;

    wrapper_entry_t *e = &ix->e[ix->count];
    e->kind = kind;
    e->key = key;
    e->id = id;
    e->flags = 0;
    ix->count++;
    return 0;
}

bool wrapper_index_remove(wrapper_index_t *ix, uint16_t id)
{
    for (uint8_t i = 0; i < ix->count; i++) {
        if (ix->e[i].id != id) continue;
        /* Shift the tail down over the removed slot so entries stay packed
         * in [0, count) -- see this file's top comment. */
        for (uint8_t j = i; (uint8_t)(j + 1) < ix->count; j++) {
            ix->e[j] = ix->e[j + 1];
        }
        ix->count--;
        return true;
    }
    return false;
}

/* Packs a MAC's first 3 bytes big-endian into the low 24 bits of a u32 --
 * see wrapper_index.h's top comment for why this encoding was chosen (it
 * only has to be internally consistent between this and wrapper_index_add()
 * callers using WMATCH_MAC_PREFIX, which wrapper_store.c does). `mac` MUST
 * already be in display/human order by the time it reaches here (M3 review
 * fix 3) -- this function itself does no reversal, so a caller handing it
 * raw GAP order silently packs the wrong 3 bytes. */
static uint32_t mac_prefix_key(const uint8_t mac[6])
{
    return ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | (uint32_t)mac[2];
}

int wrapper_index_lookup(const wrapper_index_t *ix, uint32_t svc_uuid,
                         uint32_t manu_id, const uint8_t mac[6])
{
    uint32_t mac_key = mac_prefix_key(mac);
    for (uint8_t i = 0; i < ix->count; i++) {
        const wrapper_entry_t *e = &ix->e[i];
        switch (e->kind) {
        case WMATCH_SERVICE:
            if (svc_uuid != 0xFFFFFFFFu && e->key == svc_uuid) return e->id;
            break;
        case WMATCH_MANUFACTURER:
            if (manu_id != 0xFFFFFFFFu && e->key == manu_id) return e->id;
            break;
        case WMATCH_MAC_PREFIX:
            if (e->key == mac_key) return e->id;
            break;
        default:
            break;   /* unreachable via wrapper_index_add()'s own callers */
        }
    }
    return -1;
}

uint8_t wrapper_index_kind_of(const wrapper_index_t *ix, uint16_t id)
{
    for (uint8_t i = 0; i < ix->count; i++) {
        if (ix->e[i].id == id) return ix->e[i].kind;
    }
    return 0xFF;
}
