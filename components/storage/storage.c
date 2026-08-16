#include "storage.h"
#include <stdio.h>
#include <string.h>

#define EMPTY_BOOT 0xFFFF
#define REC_SZ ((long)sizeof(storage_rec_t))
#define HDR_SZ ((long)sizeof(history_map_t))
#define CACHE_SLOTS 32   /* 16 sensors x 2 tiers */

typedef struct {
    bool used;
    uint8_t plant_id;
    storage_tier_t tier;
    uint32_t next_idx;   /* next slot to write */
} cache_t;

static cache_t s_cache[CACHE_SLOTS];

/* ---------------------------------------------------------------------
 * Pure column-map helpers. No file I/O -- kept host-testable so
 * tests/host/test_history_cols.c can link storage.c directly and exercise
 * them without a filesystem. See storage.h's doc comments for the
 * contract.
 * ------------------------------------------------------------------- */

void history_map_init(history_map_t *m)
{
    m->fmt = 2;
    for (int i = 0; i < HISTORY_COLS; i++) m->cap[i] = CAP_NONE;
}

int history_map_col(const history_map_t *m, uint8_t cap_id)
{
    for (int i = 0; i < HISTORY_COLS; i++)
        if (m->cap[i] == cap_id) return i;
    return -1;
}

int history_map_ensure(history_map_t *m, uint8_t cap_id)
{
    int existing = history_map_col(m, cap_id);
    if (existing >= 0) return existing;
    for (int i = 0; i < HISTORY_COLS; i++) {
        if (m->cap[i] == CAP_NONE) {
            m->cap[i] = cap_id;
            return i;
        }
    }
    return -1;   /* map full: caller logs, live values unaffected */
}

/* ---------------------------------------------------------------------
 * Ring-file I/O. Layout: history_map_t header, then `cap` fixed-size
 * storage_rec_t slots (ring buffer semantics unchanged from V1).
 * ------------------------------------------------------------------- */

void storage_reset_cache(void) { memset(s_cache, 0, sizeof(s_cache)); }

void storage_drop(uint8_t plant_id)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].plant_id == plant_id) {
            s_cache[i].used = false;
        }
    }
}

static uint32_t tier_cap(storage_tier_t t) { return t == STORAGE_TIER_RAW ? STORAGE_RAW_CAP : STORAGE_HOURLY_CAP; }

static void tier_path(char *out, size_t n, const char *base, uint8_t plant_id, storage_tier_t t)
{
    snprintf(out, n, "%s/P%u_%s.bin", base, plant_id,
             t == STORAGE_TIER_RAW ? "raw" : "hr");
}

/* strictly-newer comparison on the (boot_id, rel_s) total order */
static bool newer(uint16_t b1, uint32_t r1, uint16_t b2, uint32_t r2)
{
    return b1 != b2 ? b1 > b2 : r1 > r2;
}

/* littlefs's copy-on-write commit semantics are the primary defense against
 * torn 22-byte record writes on-target; this check is belt-and-braces for
 * other filesystems (host tests) and any residual corruption. A torn write
 * can leave a slot with a non-empty boot_id but garbage rel_s, which would
 * otherwise win the (boot_id, rel_s) newest-comparison and misplace the
 * write cursor. */
static bool rec_plausible(const storage_rec_t *r)
{
    return r->boot_id != EMPTY_BOOT && r->rel_s != 0xFFFFFFFFu && r->rel_s < 320000000u; /* ~10y uptime ceiling */
}

static bool write_header(FILE *f, const history_map_t *m)
{
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    return fwrite(m, 1, sizeof(*m), f) == sizeof(*m);
}

/* fmt != 2 covers both a genuinely corrupt header and a foreign one -- most
 * notably a pre-M2 V1 ring file, which has no header at all (its first 9
 * bytes are just the start of its first 16-byte record) and will only
 * coincidentally read back fmt==2. Either way the caller must not trust
 * cap[]. */
static bool read_header(FILE *f, history_map_t *m)
{
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fread(m, 1, sizeof(*m), f) != sizeof(*m)) return false;
    return m->fmt == 2;
}

/* Creates a brand-new ring file at path: a fresh fmt=2 empty-map header
 * followed by `cap` 0xFF-filled (empty-slot) records. On any write failure
 * the partial file is removed so a later retry starts clean rather than
 * fseeking past a short file and leaving dead zero-filled slots (e.g.
 * ENOSPC partway through preallocation). */
static FILE *create_fresh(const char *path, uint32_t cap, history_map_t *map_out)
{
    FILE *f = fopen(path, "w+b");
    if (!f) return NULL;

    history_map_t m;
    history_map_init(&m);
    if (!write_header(f, &m)) {
        fclose(f);
        remove(path);
        return NULL;
    }

    uint8_t block[256];
    memset(block, 0xFF, sizeof(block));
    long total = (long)cap * REC_SZ;
    for (long off = 0; off < total; off += (long)sizeof(block)) {
        long n = total - off < (long)sizeof(block) ? total - off : (long)sizeof(block);
        if (fwrite(block, 1, (size_t)n, f) != (size_t)n) {
            fclose(f);
            remove(path);
            return NULL;
        }
    }

    if (map_out) *map_out = m;
    return f;
}

/* Opens path's ring file for read+write, creating it fresh if missing. A
 * file whose header is short/unreadable or not fmt==2 -- a corrupt file,
 * or a foreign one, notably a pre-M2 V1 ring (no header, 16-byte records)
 * -- is NOT migrated in place (Task 5 wipes V1 rings outright before M2
 * ships), but must never crash or misdirect the write cursor either: it is
 * discarded and recreated fresh here, same as a missing file. */
static FILE *open_or_create(const char *path, uint32_t cap, history_map_t *map_out)
{
    FILE *f = fopen(path, "r+b");
    if (f) {
        history_map_t m;
        if (read_header(f, &m)) {
            if (map_out) *map_out = m;
            return f;
        }
        fclose(f);
        remove(path);
    }
    return create_fresh(path, cap, map_out);
}

/* Scan the file for the newest record; returns the slot AFTER it (next write). */
static uint32_t scan_next_idx(FILE *f, uint32_t cap)
{
    storage_rec_t rec;
    uint32_t newest_idx = 0;
    uint16_t nb = EMPTY_BOOT; uint32_t nr = 0;
    bool any = false;
    fseek(f, HDR_SZ, SEEK_SET);
    for (uint32_t i = 0; i < cap; i++) {
        if (fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if (!rec_plausible(&rec)) continue;
        if (!any || newer(rec.boot_id, rec.rel_s, nb, nr)) {
            any = true; nb = rec.boot_id; nr = rec.rel_s; newest_idx = i;
        }
    }
    return any ? (newest_idx + 1) % cap : 0;
}

static cache_t *cache_get(uint8_t plant_id, storage_tier_t tier)
{
    cache_t *free_slot = NULL;
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].used && s_cache[i].tier == tier && s_cache[i].plant_id == plant_id)
            return &s_cache[i];
        if (!s_cache[i].used && !free_slot) free_slot = &s_cache[i];
    }
    return free_slot;   /* caller initializes; NULL if cache exhausted */
}

int storage_append(const char *base, uint8_t plant_id, storage_tier_t tier, const storage_rec_t *rec)
{
    uint32_t cap = tier_cap(tier);
    char path[128];
    tier_path(path, sizeof(path), base, plant_id, tier);

    FILE *f = open_or_create(path, cap, NULL);
    if (!f) return -1;

    cache_t *c = cache_get(plant_id, tier);
    if (!c) { fclose(f); return -1; }
    if (!c->used) {
        c->used = true;
        c->plant_id = plant_id;
        c->tier = tier;
        c->next_idx = scan_next_idx(f, cap);
    }

    if (fseek(f, HDR_SZ + (long)c->next_idx * REC_SZ, SEEK_SET) != 0 ||
        fwrite(rec, 1, sizeof(*rec), f) != sizeof(*rec)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    c->next_idx = (c->next_idx + 1) % cap;
    return 0;
}

int storage_query(const char *base, uint8_t plant_id, storage_tier_t tier,
                  uint32_t from_epoch, uint32_t to_epoch,
                  storage_resolve_fn resolve, void *rctx,
                  storage_row_fn row, void *ctx,
                  history_map_t *map_out)
{
    uint32_t cap = tier_cap(tier);
    char path[128];
    tier_path(path, sizeof(path), base, plant_id, tier);

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (map_out) history_map_init(map_out);
        return 0;   /* no data yet */
    }

    history_map_t m;
    if (!read_header(f, &m)) {
        /* Corrupt/foreign header on a read-only pass: nothing sane to
         * recover in place here (this path may not even be writable, e.g.
         * a read replica), so report "no data" rather than guess at
         * column meanings or crash. storage_append()/storage_col_for()
         * will discard and recreate the file the next time this plant is
         * actually written. */
        fclose(f);
        if (map_out) history_map_init(map_out);
        return 0;
    }
    if (map_out) *map_out = m;

    /* ring order: oldest record sits right after the newest */
    uint32_t start = scan_next_idx(f, cap);   /* == oldest slot (or 0) */
    storage_rec_t rec;
    int emitted = 0;
    for (uint32_t k = 0; k < cap; k++) {
        uint32_t i = (start + k) % cap;
        if (fseek(f, HDR_SZ + (long)i * REC_SZ, SEEK_SET) != 0) break;
        if (fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if (!rec_plausible(&rec)) continue;
        uint32_t epoch;
        if (!resolve(rctx, rec.boot_id, rec.rel_s, &epoch)) continue;
        if (epoch < from_epoch || epoch > to_epoch) continue;
        row(ctx, epoch, &rec);
        emitted++;
    }
    fclose(f);
    return emitted;
}

int storage_col_for(const char *base, uint8_t plant_id, storage_tier_t tier, uint8_t cap_id)
{
    uint32_t cap = tier_cap(tier);
    char path[128];
    tier_path(path, sizeof(path), base, plant_id, tier);

    history_map_t before;
    FILE *f = open_or_create(path, cap, &before);
    if (!f) return -1;

    history_map_t m = before;
    int col = history_map_ensure(&m, cap_id);
    if (col >= 0 && memcmp(&before, &m, sizeof(m)) != 0) {
        if (!write_header(f, &m)) {
            fclose(f);
            return -1;
        }
    }
    fclose(f);
    return col;
}
