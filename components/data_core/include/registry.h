#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mibeacon.h"

#define REGISTRY_MAX_SENSORS 16

typedef struct {
    bool      in_use;
    uint8_t   mac[6];
    uint32_t  last_seen_s;
    mibeacon_t latest;    /* merged latest values; has_* flags accumulate */
} sensor_entry_t;

typedef struct {
    sensor_entry_t sensors[REGISTRY_MAX_SENSORS];
} registry_t;

void registry_init(registry_t *r);
int  registry_update(registry_t *r, const mibeacon_t *m, uint32_t now_s); /* 1 merged, 0 dup, -1 full */
int  registry_find(const registry_t *r, const uint8_t mac[6]);
int  registry_count(const registry_t *r);
