#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BOOTTAB_MAX 64

struct boottab_entry { uint16_t boot_id; uint32_t offset; };

typedef struct {
    uint16_t count;
    struct boottab_entry e[BOOTTAB_MAX];
} boottab_t;

int  boottab_load(boottab_t *t, const char *path);
int  boottab_add(boottab_t *t, const char *path, uint16_t boot_id, uint32_t offset);
bool boottab_resolve(const boottab_t *t, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
