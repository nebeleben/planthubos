#include "boottab.h"
#include <stdio.h>
#include <string.h>

/* On-disk format: sequence of 6-byte little-endian records (u16 boot_id, u32 offset). */

static int write_all(boottab_t *t, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    for (uint16_t i = 0; i < t->count; i++) {
        uint8_t rec[6] = {
            (uint8_t)(t->e[i].boot_id & 0xFF), (uint8_t)(t->e[i].boot_id >> 8),
            (uint8_t)(t->e[i].offset), (uint8_t)(t->e[i].offset >> 8),
            (uint8_t)(t->e[i].offset >> 16), (uint8_t)(t->e[i].offset >> 24),
        };
        if (fwrite(rec, 1, 6, f) != 6) { fclose(f); return -1; }
    }
    fclose(f);
    return 0;
}

int boottab_load(boottab_t *t, const char *path)
{
    memset(t, 0, sizeof(*t));
    FILE *f = fopen(path, "rb");
    if (!f) return 0;   /* no table yet is a valid state */
    uint8_t rec[6];
    while (t->count < BOOTTAB_MAX && fread(rec, 1, 6, f) == 6) {
        t->e[t->count].boot_id = (uint16_t)(rec[0] | (rec[1] << 8));
        t->e[t->count].offset = (uint32_t)rec[2] | ((uint32_t)rec[3] << 8) |
                                ((uint32_t)rec[4] << 16) | ((uint32_t)rec[5] << 24);
        t->count++;
    }
    fclose(f);
    return 0;
}

int boottab_add(boottab_t *t, const char *path, uint16_t boot_id, uint32_t offset)
{
    for (uint16_t i = 0; i < t->count; i++)
        if (t->e[i].boot_id == boot_id) return 0;   /* first write wins */

    if (t->count >= BOOTTAB_MAX) {
        /* drop the oldest half (entries are appended in boot order) */
        uint16_t keep = BOOTTAB_MAX / 2;
        memmove(&t->e[0], &t->e[t->count - keep], keep * sizeof(t->e[0]));
        t->count = keep;
        t->e[t->count].boot_id = boot_id;
        t->e[t->count].offset = offset;
        t->count++;
        return write_all(t, path);
    }

    t->e[t->count].boot_id = boot_id;
    t->e[t->count].offset = offset;
    t->count++;
    /* fast path: append just the new record */
    FILE *f = fopen(path, "ab");
    if (!f) return -1;
    uint8_t rec[6] = {
        (uint8_t)(boot_id & 0xFF), (uint8_t)(boot_id >> 8),
        (uint8_t)offset, (uint8_t)(offset >> 8),
        (uint8_t)(offset >> 16), (uint8_t)(offset >> 24),
    };
    int ok = fwrite(rec, 1, 6, f) == 6;
    fclose(f);
    return ok ? 0 : -1;
}

bool boottab_resolve(const boottab_t *t, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out)
{
    for (uint16_t i = 0; i < t->count; i++) {
        if (t->e[i].boot_id == boot_id) {
            *epoch_out = t->e[i].offset + rel_s;
            return true;
        }
    }
    return false;
}
