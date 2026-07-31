#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "boottab.h"

int main(void)
{
    char dir[] = "/tmp/planthub_boottab_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof(path), "%s/boottab.bin", dir);

    boottab_t t;
    /* missing file loads as empty */
    assert(boottab_load(&t, path) == 0);
    assert(t.count == 0);
    uint32_t epoch;
    assert(!boottab_resolve(&t, 1, 100, &epoch));

    /* add + resolve: boot 1 started at epoch 1700000000 */
    assert(boottab_add(&t, path, 1, 1700000000u) == 0);
    assert(boottab_resolve(&t, 1, 3600, &epoch) && epoch == 1700003600u);
    assert(!boottab_resolve(&t, 2, 10, &epoch));   /* unknown boot */

    /* duplicate boot_id ignored (first write wins) */
    assert(boottab_add(&t, path, 1, 1800000000u) == 0);
    assert(boottab_resolve(&t, 1, 0, &epoch) && epoch == 1700000000u);
    assert(t.count == 1);

    /* persists across reload */
    boottab_t t2;
    assert(boottab_load(&t2, path) == 0);
    assert(t2.count == 1);
    assert(boottab_resolve(&t2, 1, 60, &epoch) && epoch == 1700000060u);

    /* fill to capacity + 1: oldest half dropped, newest retained */
    for (uint16_t b = 2; b <= BOOTTAB_MAX + 1; b++)
        assert(boottab_add(&t2, path, b, 1700000000u + b) == 0);
    assert(t2.count <= BOOTTAB_MAX);
    assert(boottab_resolve(&t2, BOOTTAB_MAX + 1, 0, &epoch) && epoch == 1700000000u + BOOTTAB_MAX + 1);
    assert(!boottab_resolve(&t2, 1, 0, &epoch));   /* oldest dropped */
    boottab_t t3;  /* the rewrite persisted */
    assert(boottab_load(&t3, path) == 0);
    assert(boottab_resolve(&t3, BOOTTAB_MAX + 1, 0, &epoch));

    snprintf(path, sizeof(path), "rm -rf %s", dir);
    system(path);
    printf("test_boottab: OK\n");
    return 0;
}
