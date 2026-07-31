#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STORAGE_RAW_CAP     2880
#define STORAGE_HOURLY_CAP  720

#define STORAGE_TEMP_NONE   INT16_MIN
#define STORAGE_U8_NONE     0xFF
#define STORAGE_LUX_NONE    0xFFFFFFFFu
#define STORAGE_U16_NONE    0xFFFF

typedef struct __attribute__((packed)) {
    uint16_t boot_id;          /* 0xFFFF = empty slot */
    uint32_t rel_s;            /* uptime seconds at sample time */
    int16_t  temp_dc;
    uint8_t  moisture_pct;
    uint8_t  battery_pct;
    uint32_t lux;
    uint16_t conductivity_us;
} storage_rec_t;               /* exactly 16 bytes */

typedef enum { STORAGE_TIER_RAW, STORAGE_TIER_HOURLY } storage_tier_t;

typedef bool (*storage_resolve_fn)(void *rctx, uint16_t boot_id, uint32_t rel_s, uint32_t *epoch_out);
typedef void (*storage_row_fn)(void *ctx, uint32_t epoch, const storage_rec_t *rec);

int  storage_append(const char *base, const uint8_t mac[6], storage_tier_t tier, const storage_rec_t *rec);
int  storage_query(const char *base, const uint8_t mac[6], storage_tier_t tier,
                   uint32_t from_epoch, uint32_t to_epoch,
                   storage_resolve_fn resolve, void *rctx,
                   storage_row_fn row, void *ctx);
void storage_reset_cache(void);
