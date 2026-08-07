#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "storage.h"

static const uint8_t PLANT_ID = 7;

static storage_rec_t mk(uint16_t boot, uint32_t rel, int16_t temp)
{
    storage_rec_t r;
    memset(&r, 0xFF, sizeof(r));           /* all absent markers */
    r.boot_id = boot; r.rel_s = rel; r.temp_dc = temp;
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
 * ring file directly in the torn-write guard test below */
static void raw_path(char *out, size_t n, const char *dir, uint8_t plant_id)
{
    snprintf(out, n, "%s/P%u_raw.bin", dir, plant_id);
}

int main(void)
{
    char dir[] = "/tmp/planthub_storage_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    assert(sizeof(storage_rec_t) == 16);

    rows_t rows;

    /* append 3 records across two boots, query all */
    storage_rec_t r1 = mk(1, 100, 210), r2 = mk(1, 1000, 220), r3 = mk(2, 50, 230);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r1) == 0);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r2) == 0);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r3) == 0);

    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows) == 3);
    assert(rows.n == 3);
    assert(rows.epochs[0] == 1000100 && rows.epochs[1] == 1001000 && rows.epochs[2] == 2000050);

    /* range filter */
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 1000500, 1999999, resolve, NULL, row, &rows) == 1);
    assert(rows.epochs[0] == 1001000);

    /* unresolvable boot skipped */
    storage_rec_t r4 = mk(3, 10, 240);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r4) == 0);
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows) == 3);

    /* write-position recovery after "reboot" (cache reset): next append lands after r4 */
    storage_reset_cache();
    storage_rec_t r5 = mk(3, 20, 250);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_RAW, &r5) == 0);

    /* hourly tier is a separate file with its own capacity */
    storage_rec_t h1 = mk(1, 0, 215);
    assert(storage_append(dir, PLANT_ID, STORAGE_TIER_HOURLY, &h1) == 0);
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_HOURLY, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows) == 1);

    /* wraparound: fill the hourly ring past capacity; oldest overwritten, order preserved */
    for (uint32_t i = 1; i <= STORAGE_HOURLY_CAP + 10; i++) {
        storage_rec_t h = mk(2, i * 3600, 200);
        assert(storage_append(dir, PLANT_ID, STORAGE_TIER_HOURLY, &h) == 0);
    }
    rows.n = 0;
    assert(storage_query(dir, PLANT_ID, STORAGE_TIER_HOURLY, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows) == STORAGE_HOURLY_CAP);
    for (int i = 1; i < rows.n; i++) assert(rows.epochs[i] > rows.epochs[i - 1]);
    assert(rows.epochs[rows.n - 1] == 2000000u + (STORAGE_HOURLY_CAP + 10) * 3600);

    /* torn-write guard: a garbage 16-byte record (implausible rel_s) must
     * not misdirect the write cursor and must be skipped on query, with the
     * genuine records still emitted in order */
    const uint8_t PLANT_ID2 = 42;
    storage_rec_t g1 = mk(1, 100, 210);
    assert(storage_append(dir, PLANT_ID2, STORAGE_TIER_RAW, &g1) == 0);

    char raw_file[160];
    raw_path(raw_file, sizeof(raw_file), dir, PLANT_ID2);
    FILE *gf = fopen(raw_file, "r+b");
    assert(gf != NULL);
    uint8_t garbage[16];
    memset(garbage, 0xAB, sizeof(garbage));
    assert(fseek(gf, 5 * (long)sizeof(storage_rec_t), SEEK_SET) == 0);
    assert(fwrite(garbage, 1, sizeof(garbage), gf) == sizeof(garbage));
    fclose(gf);

    storage_reset_cache();
    storage_rec_t g2 = mk(1, 200, 220);
    assert(storage_append(dir, PLANT_ID2, STORAGE_TIER_RAW, &g2) == 0);

    rows.n = 0;
    assert(storage_query(dir, PLANT_ID2, STORAGE_TIER_RAW, 0, 0xFFFFFFFFu, resolve, NULL, row, &rows) == 2);
    assert(rows.n == 2);
    assert(rows.epochs[0] == 1000100 && rows.epochs[1] == 1000200);
    for (int i = 1; i < rows.n; i++) assert(rows.epochs[i] > rows.epochs[i - 1]);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
    printf("test_storage: OK\n");
    return 0;
}
