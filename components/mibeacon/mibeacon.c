#include "mibeacon.h"
#include <string.h>

#define FC_ENCRYPTED   0x0008
#define FC_MAC_INCLUDE 0x0010
#define FC_CAP_INCLUDE 0x0020
#define FC_OBJ_INCLUDE 0x0040

mibeacon_err_t mibeacon_parse(const uint8_t *data, size_t len, mibeacon_t *out)
{
    if (len < 5) return MIBEACON_ERR_TRUNCATED;
    memset(out, 0, sizeof(*out));

    uint16_t fc = (uint16_t)(data[0] | (data[1] << 8));
    out->product_id = (uint16_t)(data[2] | (data[3] << 8));
    out->frame_cnt = data[4];
    size_t off = 5;

    if (fc & FC_ENCRYPTED) return MIBEACON_ERR_ENCRYPTED;
    if (!(fc & FC_MAC_INCLUDE)) return MIBEACON_ERR_NO_MAC;
    if (off + 6 > len) return MIBEACON_ERR_TRUNCATED;
    for (int i = 0; i < 6; i++) out->mac[i] = data[off + 5 - i];  /* wire order reversed */
    off += 6;

    if (fc & FC_CAP_INCLUDE) {
        if (off + 1 > len) return MIBEACON_ERR_TRUNCATED;
        off += 1;
    }

    if (!(fc & FC_OBJ_INCLUDE)) return MIBEACON_ERR_NO_OBJECT;

    while (off + 3 <= len) {
        uint16_t id  = (uint16_t)(data[off] | (data[off + 1] << 8));
        uint8_t olen = data[off + 2];
        off += 3;
        if (off + olen > len) return MIBEACON_ERR_TRUNCATED;
        const uint8_t *p = data + off;
        switch (id) {
        case 0x1004: if (olen >= 2) { out->temp_dc = (int16_t)(p[0] | (p[1] << 8)); out->has_temp = true; } break;
        case 0x1007: if (olen >= 3) { out->lux = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16); out->has_lux = true; } break;
        case 0x1008: if (olen >= 1) { out->moisture_pct = p[0]; out->has_moisture = true; } break;
        case 0x1009: if (olen >= 2) { out->conductivity_us = (uint16_t)(p[0] | (p[1] << 8)); out->has_conductivity = true; } break;
        case 0x100A: if (olen >= 1) { out->battery_pct = p[0]; out->has_battery = true; } break;
        default: break;  /* unknown object: skip */
        }
        off += olen;
    }
    return MIBEACON_OK;
}
