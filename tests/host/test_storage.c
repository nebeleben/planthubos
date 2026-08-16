#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "storage.h"
#include "capability.h"

static const uint8_t PLANT_ID = 7;

static storage_rec_t mk(uint16_t boot, uint32_t rel, int16_t val)
{
    storage_rec_t r;
    memset(&r, 0xFF, sizeof(r));           /* all absent markers */
    r.boot_id = boot; r.rel_s = rel;
    for (int i = 0; i < HISTORY_COLS; i++) r.col[i] = CAP_VALUE_NONE;
    r.col[0] = val;
    return r;
}

/* resolver: boot 1 offset 1000000, boot 2 offset 2000000, boot 3 unknown */
static bool resolve(void *rctx, uint16_t boot, uint32_t rel, uint32_t *out)
{
    (void)rctx;
    if (boot == 1) { *out = 1000000u + rel; return true; }
    if (boot == 2) { *out = 2000000u + rel; return true; }
    return false;
}

typedef struct { uint32_t epochs[4000]; int n; } rows_t;
static void row(void *ctx, uint32_t epoch, const storage_rec_t *rec)
{
    rows_t *r = ctx;
    (void)rec;
    r->epochs[r->n++] = epoch;
}

/* mirrors storage.c's private tier_path() naming scheme, for poking a raw
 * ring file directly in the torn-write guard / malformed-header tests
 * below */
static void raw_path(char *out, size_t n, const char *dir, uint8_t plant_id)
{
    snprintf(out, n, "%s/P%u_raw.bin", dir, plant_id);
}

int main(void)
{
    char dir[] = "/tmp/planthub_storage_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    assert(sizeof(storage_rec_t) == 22);

    rows_t rows;
    history_map_t map;

    /* append 3 records across two boots, query all */
    storage_rec_t r1 = mk(1, 100, 210), r2 = mk(1, 1000, 220), r3 = mk(2, 50, 230);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r1) == 0);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r2) == 0);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r3) == 0);

    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, &map) == 3);
    assert(rows.n == 3);
    assert(rows.epochs[0] == 1000100 && rows.epochs[1] == 1001000 && rows.epochs[2] == 2000050);

    /* range filter */
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 1000500, 1999999, resolve, NULL, row, &rows, NULL) == 1);
    assert(rows.epochs[0] == 1001000);

    /* unresolvable boot skipped */
    storage_rec_t r4 = mk(3, 10, 240);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r4) == 0);
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == 3);

    /* write-position recovery after "reboot" (cache reset): next append lands after r4 */
    storage_reset_cache();
    storage_rec_t r5 = mk(3, 20, 250);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r5) == 0);

    /* hourly tier is a separate file with its own capacity */
    storage_rec_t h1 = mk(1, 0, 215);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_HOURLY, &h1) == 0);
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_HOURLY, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == 1);

    /* wraparound: fill the hourly ring past capacity; oldest overwritten, order preserved */
    for (uint32_t i = 1; i <= STORAGE_HOURLY_CAP + 10; i++) {
        storage_rec_t h = mk(2, i * 3600, 200);
        assert(storage_append(dir, PLANT_ID, STORAGE_TIER_HOURLY, &h) == 0);
    }
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_HOURLY, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == STORAGE_HOURLY_CAP);
    for (int i = 1; i < rows.n; i++) assert(rows.epochs[i] > rows.epochs[i - 1]);
    assert(rows.epochs[rows.n - 1] == 2000000u + (STORAGE_HOURLY_CAP + 10) * 3600);

    /* the ring file's header persists the column map: col[0] was the only
     * column ever written above, so it -- and only it -- gets allocated by
     * a caller that ensures it through storage_col_for() */
    assert(storage_col_for(dir, PLANT_ID, STORAGE_TIER_HOURLY, CAP_AIR_TEMPERATURE) == 0);
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_HOURLY, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, &map) == STORAGE_HOURLY_CAP);
    assert(map.fmt == 2);
    assert(map.cap[0] == CAP_AIR_TEMPERATURE);
    for (int i = 1; i < HISTORY_COLS; i++) assert(map.cap[i] == CAP_NONE);

    /* torn-write guard: a garbage 22-byte record (implausible rel_s) must
     * not misdirect the write cursor and must be skipped on query, with the
     * genuine records still emitted in order */
    const uint8_t PLANT_ID2 = 42;
    storage_rec_t g1 = mk(1, 100, 210);
    assert(storage_append(dir, PLANT_ID2, STORAGE_TIER_RAW, &g1) == 0);

    char raw_file[160];
    raw_path(raw_file, sizeof(raw_file), dir, PLANT_ID2);
    FILE *gf = fopen(raw_file, "r+b");
    assert(gf != NULL);
    uint8_t garbage[sizeof(storage_rec_t)];
    memset(garbage, 0xAB, sizeof(garbage));
    /* header (history_map_t) precedes the record area */
    long hdr_sz = (long)sizeof(history_map_t);
    assert(fseek(gf, hdr_sz + 5 * (long)sizeof(storage_rec_t), SEEK_SET) == 0);
    assert(fwrite(garbage, 1, sizeof(garbage), gf) == sizeof(garbage));
    fclose(gf);

    storage_reset_cache();
    storage_rec_t g2 = mk(1, 200, 220);
    assert(storage_append(dir, PLANT_ID2, STORAGE_TIER_RAW, &g2) == 0);

    rows.n = 0;
    assert(storage_query(dir, PLANT_ID2, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == 2);
    assert(rows.n == 2);
    assert(rows.epochs[0] == 1000100 && rows.epochs[1] == 1000200);
    for (int i = 1; i < rows.n; i++) assert(rows.epochs[i] > rows.epochs[i - 1]);

    /* storage_drop(): CACHE_SLOTS exhaustion + reuse (M8 Task 6 review fix
     * -- MEDIUM 3a). Ids are never reused (plants_table.h), so without
     * storage_drop() a deleted plant's cache slot(s) sit claimed forever;
     * fill every one of the 32 slots (16 distinct plant ids x 2 tiers),
     * confirm a 17th distinct id has nowhere to go (the "kills ALL future
     * appends" failure mode the review found), then storage_drop() one busy
     * id and confirm a DIFFERENT plant id's append now succeeds by reusing
     * the freed slot. */
    storage_reset_cache();
    for (uint8_t pid = 100; pid < 100 + 16; pid++) {
        storage_rec_t r = mk(1, 1, 111);
        assert(storage_append(dir, pid, STORAGE_TIER_RAW, &r) == 0);
        assert(storage_append(dir, pid, STORAGE_TIER_HOURLY, &r) == 0);
    }
    storage_rec_t over = mk(1, 1, 111);
    assert(storage_append(dir, 200, STORAGE_TIER_RAW, &over) == -1);

    storage_drop(100);   /* frees plant 100's raw + hourly slots */
    assert(storage_append(dir, 200, STORAGE_TIER_RAW, &over) == 0);

    /* storage_drop() only touches the cache, never the on-disk file: plant
     * 100's earlier raw record is still there, untouched by the drop. */
    rows.n = 0;
    assert(storage_query(dir, 100, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == 1);
    assert(rows.epochs[0] == 1000001);

    /* dropping an id with no cached slot at all is a safe no-op */
    storage_drop(250);

    /* empty ring: a plant that has never appended anything queries as
     * zero rows and an all-CAP_NONE map, no crash */
    rows.n = 0;
    history_map_t empty_map;
    memset(&empty_map, 0xAA, sizeof(empty_map));   /* poison, must be overwritten */
    assert(storage_query(dir, 254, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, &empty_map) == 0);
    assert(rows.n == 0);
    assert(empty_map.fmt == 2);
    for (int i = 0; i < HISTORY_COLS; i++) assert(empty_map.cap[i] == CAP_NONE);

    /* malformed header: a file with no header at all (e.g. a pre-M2 V1
     * ring, or plain garbage) must not crash storage_query()/
     * storage_append() -- it is treated as unreadable/no-data on query,
     * and discarded + recreated fresh (with a valid header) on the next
     * append, rather than misinterpreting its bytes as a column map or
     * corrupting the write cursor. */
    const uint8_t PLANT_ID3 = 99;
    char bogus_file[160];
    raw_path(bogus_file, sizeof(bogus_file), dir, PLANT_ID3);
    FILE *bf = fopen(bogus_file, "wb");
    assert(bf != NULL);
    uint8_t junk[5] = { 1, 2, 3, 4, 5 };   /* shorter than history_map_t (9 bytes) */
    assert(fwrite(junk, 1, sizeof(junk), bf) == sizeof(junk));
    fclose(bf);

    rows.n = 0;
    history_map_t bogus_map;
    memset(&bogus_map, 0xAA, sizeof(bogus_map));
    assert(storage_query(dir, PLANT_ID3, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, &bogus_map) == 0);
    assert(rows.n == 0);
    assert(bogus_map.fmt == 2);   /* query reports "no data" with a safe empty map */

    storage_reset_cache();
    storage_rec_t after_bogus = mk(1, 1, 111);
    assert(storage_append(dir, PLANT_ID3, STORAGE_TIER_RAW, &after_bogus) == 0);
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID3, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows, NULL) == 1);
    assert(rows.epochs[0] == 1000001);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
    printf("test_storage: OK\n");
    return 0;
}
