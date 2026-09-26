#include "zb_tuya.h"
#include <stddef.h>

int zb_tuya_parse(const uint8_t *buf, uint16_t len, zb_tuya_dp_t *out, int max) {
    if (!buf || !out || max <= 0 || len < 2) return 0;   /* need [status][seq] */
    int n = 0;
    uint16_t i = 2;                                       /* skip status + seq */
    while (i + 4 <= len && n < max) {                     /* need dp_id,type,len(2) */
        uint8_t dp_id = buf[i];
        uint8_t type  = buf[i + 1];
        uint16_t dlen = (uint16_t)((buf[i + 2] << 8) | buf[i + 3]);
        i += 4;
        if ((uint32_t)i + dlen > len) break;              /* truncated value -> stop */
        /* integer types only: 0x01 bool, 0x02 value(int32), 0x04 enum,
         * 0x05 bitmap. 0x00 raw / 0x03 string are skipped. */
        if (type == 0x01 || type == 0x02 || type == 0x04 || type == 0x05) {
            int32_t v = 0;
            /* big-endian bytes into a signed int; a full 4-byte value
             * sign-extends naturally (its top bit lands in bit 31). */
            for (uint16_t k = 0; k < dlen && k < 4; k++) v = (v << 8) | buf[i + k];
            out[n].dp_id = dp_id;
            out[n].type = type;
            out[n].value = v;
            n++;
        }
        i += dlen;
    }
    return n;
}
