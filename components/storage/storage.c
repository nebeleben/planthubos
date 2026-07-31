#include "storage.h"
#include <stdio.h>
#include <string.h>

#define EMPTY_BOOT 0xFFFF
#define REC_SZ ((long)sizeof(storage_rec_t))
#define CACHE_SLOTS 32   /* 16 sensors x 2 tiers */

typedef struct {
    bool used;
    uint8_t mac[6];
    storage_tier_t tier;
    uint32_t next_idx;   /* next slot to write */
} cache_t;

static cache_t s_cache[CACHE_SLOTS];

void storage_reset_cache(void) { memset(s_cache, 0, sizeof(s_cache)); }

static uint32_t tier_cap(storage_tier_t t) { return t == STORAGE_TIER_RAW ? STORAGE_RAW_CAP : STORAGE_HOURLY_CAP; }

static void tier_path(char *out, size_t n, const char *base, const uint8_t mac[6], storage_tier_t t)
{
    snprintf(out, n, "%s/%02X%02X%02X%02X%02X%02X_%s.bin", base,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             t == STORAGE_TIER_RAW ? "raw" : "hr");
}

/* strictly-newer comparison on the (boot_id, rel_s) total order */
static bool newer(uint16_t b1, uint32_t r1, uint16_t b2, uint32_t r2)
{
    return b1 != b2 ? b1 > b2 : r1 > r2;
}

static cache_t *cache_get(const uint8_t mac[6], storage_tier_t tier)
{
    cache_t *free_slot = NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].tier == tier && memcmp(s_cache[i].mac, mac, 6) == 0)
            return &s_cache[i];
        if (!s_cache[i].used && !free_slot) free_slot = &s_cache[i];
    }
    return free_slot;   /* caller initializes; NULL if cache exhausted */
}

/* Scan the file for the newest record; returns the slot AFTER it (next write). */
static uint32_t scan_next_idx(FILE *f, uint32_t cap)
{
    storage_rec_t rec;
    uint32_t newest_idx = 0;
    uint16_t nb = EMPTY_BOOT; uint32_t nr = 0;
    bool any = false;
    fseek(f, 0, SEEK_SET);
    for (uint32_t i = 0; i < cap; i++) {
        if (fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if (rec.boot_id == EMPTY_BOOT) continue;
        if (!any || newer(rec.boot_id, rec.rel_s, nb, nr)) {
            any = true; nb = rec.boot_id; nr = rec.rel_s; newest_idx = i;
        }
    }
    return any ? (newest_idx + 1) % cap : 0;
}

static FILE *open_or_create(const char *path, uint32_t cap)
{
    FILE *f = fopen(path, "r+b");
    if (f) return f;
    f = fopen(path, "w+b");
    if (!f) return NULL;
    uint8_t block[256];
    memset(block, 0xFF, sizeof(block));
    long total = (long)cap * REC_SZ;
    for (long off = 0; off < total; off += (long)sizeof(block)) {
        long n = total - off < (long)sizeof(block) ? total - off : (long)sizeof(block);
        if (fwrite(block, 1, (size_t)n, f) != (size_t)n) { fclose(f); return NULL; }
    }
    return f;
}

int storage_append(const char *base, const uint8_t mac[6], storage_tier_t tier, const storage_rec_t *rec)
{
    uint32_t cap = tier_cap(tier);
    char path[128];
    tier_path(path, sizeof(path), base, mac, tier);

    FILE *f = open_or_create(path, cap);
    if (!f) return -1;

    cache_t *c = cache_get(mac, tier);
    if (!c) { fclose(f); return -1; }
    if (!c->used) {
        c->used = true;
        memcpy(c->mac, mac, 6);
        c->tier = tier;
        c->next_idx = scan_next_idx(f, cap);
    }

    if (fseek(f, (long)c->next_idx * REC_SZ, SEEK_SET) != 0 ||
        fwrite(rec, 1, sizeof(*rec), f) != sizeof(*rec)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    c->next_idx = (c->next_idx + 1) % cap;
    return 0;
}

int storage_query(const char *base, const uint8_t mac[6], storage_tier_t tier,
                  uint32_t from_epoch, uint32_t to_epoch,
                  storage_resolve_fn resolve, void *rctx,
                  storage_row_fn row, void *ctx)
{
    uint32_t cap = tier_cap(tier);
    char path[128];
    tier_path(path, sizeof(path), base, mac, tier);

    FILE *f = fopen(path, "rb");
    if (!f) return 0;   /* no data yet */

    /* ring order: oldest record sits right after the newest */
    uint32_t start = scan_next_idx(f, cap);   /* == oldest slot (or 0) */
    storage_rec_t rec;
    int emitted = 0;
    for (uint32_t k = 0; k < cap; k++) {
        uint32_t i = (start + k) % cap;
        if (fseek(f, (long)i * REC_SZ, SEEK_SET) != 0) break;
        if (fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if (rec.boot_id == EMPTY_BOOT) continue;
        uint32_t epoch;
        if (!resolve(rctx, rec.boot_id, rec.rel_s, &epoch)) continue;
        if (epoch < from_epoch || epoch > to_epoch) continue;
        row(ctx, epoch, &rec);
        emitted++;
    }
    fclose(f);
    return emitted;
}
