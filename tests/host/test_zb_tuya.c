/* tests/host/test_zb_tuya.c -- pure Tuya EF00 datapoint parser. */
#include "zb_tuya.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    zb_tuya_dp_t dps[ZB_TUYA_MAX_DPS];

    /* captured frame: status 00, seq 2c, DP 0x65 type value len 4 = 53 */
    uint8_t f1[] = {0x00,0x2c, 0x65,0x02,0x00,0x04, 0x00,0x00,0x00,0x35};
    assert(zb_tuya_parse(f1, sizeof f1, dps, ZB_TUYA_MAX_DPS) == 1);
    assert(dps[0].dp_id == 0x65 && dps[0].type == 0x02 && dps[0].value == 53);

    /* enum: DP 0x0e type enum len 1 = 2 */
    uint8_t f2[] = {0x00,0x2f, 0x0e,0x04,0x00,0x01, 0x02};
    assert(zb_tuya_parse(f2, sizeof f2, dps, ZB_TUYA_MAX_DPS) == 1);
    assert(dps[0].dp_id == 0x0e && dps[0].type == 0x04 && dps[0].value == 2);

    /* bool len 1 = 1 */
    uint8_t f3[] = {0x00,0x01, 0x01,0x01,0x00,0x01, 0x01};
    assert(zb_tuya_parse(f3, sizeof f3, dps, ZB_TUYA_MAX_DPS) == 1);
    assert(dps[0].value == 1);

    /* two DPs in one frame */
    uint8_t f4[] = {0x00,0x03, 0x65,0x02,0x00,0x04,0x00,0x00,0x00,0x35, 0x0e,0x04,0x00,0x01,0x02};
    assert(zb_tuya_parse(f4, sizeof f4, dps, ZB_TUYA_MAX_DPS) == 2);
    assert(dps[0].dp_id == 0x65 && dps[1].dp_id == 0x0e);

    /* signed int32 BE: 0xFFFFFFFF = -1 */
    uint8_t f5[] = {0x00,0x04, 0x05,0x02,0x00,0x04, 0xFF,0xFF,0xFF,0xFF};
    assert(zb_tuya_parse(f5, sizeof f5, dps, ZB_TUYA_MAX_DPS) == 1 && dps[0].value == -1);

    /* string DP skipped (not written) */
    uint8_t f6[] = {0x00,0x05, 0x66,0x03,0x00,0x02, 'h','i'};
    assert(zb_tuya_parse(f6, sizeof f6, dps, ZB_TUYA_MAX_DPS) == 0);

    /* truncated: header only, or a DP whose len runs past the buffer -> parse what fits */
    uint8_t f7[] = {0x00,0x06};
    assert(zb_tuya_parse(f7, sizeof f7, dps, ZB_TUYA_MAX_DPS) == 0);
    uint8_t f8[] = {0x00,0x07, 0x65,0x02,0x00,0x04, 0x00,0x00};  /* claims 4, only 2 present */
    assert(zb_tuya_parse(f8, sizeof f8, dps, ZB_TUYA_MAX_DPS) == 0);

    /* NULL/short guards */
    assert(zb_tuya_parse(NULL, 10, dps, ZB_TUYA_MAX_DPS) == 0);
    assert(zb_tuya_parse(f1, 1, dps, ZB_TUYA_MAX_DPS) == 0);

    printf("test_zb_tuya: OK\n");
    return 0;
}
