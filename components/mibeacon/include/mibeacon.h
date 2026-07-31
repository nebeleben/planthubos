#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIBEACON_PRODUCT_MIFLORA 0x0098

typedef struct {
    uint8_t  mac[6];            /* big-endian (human order) */
    uint16_t product_id;
    uint8_t  frame_cnt;
    bool     has_temp, has_moisture, has_lux, has_conductivity, has_battery;
    int16_t  temp_dc;           /* deci-degrees Celsius */
    uint8_t  moisture_pct;
    uint32_t lux;
    uint16_t conductivity_us;   /* uS/cm */
    uint8_t  battery_pct;
} mibeacon_t;

typedef enum {
    MIBEACON_OK            = 0,
    MIBEACON_ERR_TRUNCATED = -1,
    MIBEACON_ERR_ENCRYPTED = -2,
    MIBEACON_ERR_NO_MAC    = -3,
    MIBEACON_ERR_NO_OBJECT = -4,
} mibeacon_err_t;

/* data/len = service-data payload AFTER the 2-byte 0xFE95 UUID. */
mibeacon_err_t mibeacon_parse(const uint8_t *data, size_t len, mibeacon_t *out);
