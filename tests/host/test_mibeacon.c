#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mibeacon.h"

/* Frame-control bits (LE u16 at offset 0):
 * bit3 encrypted, bit4 mac_include, bit5 capability_include, bit6 object_include */
#define FC_ENC  0x0008
#define FC_MAC  0x0010
#define FC_CAP  0x0020
#define FC_OBJ  0x0040

static const uint8_t MAC[6] = { 0xC4, 0x7C, 0x8D, 0x11, 0x22, 0x33 };

/* Build a frame from fields: header + optional reversed MAC + optional
 * capability byte + one object (id, olen, payload). Returns total length. */
static size_t build(uint8_t *buf, uint16_t fc, uint16_t product, uint8_t cnt,
                    bool mac, bool cap, uint16_t obj_id, const uint8_t *payload, uint8_t olen)
{
    size_t off = 0;
    buf[off++] = fc & 0xFF; buf[off++] = fc >> 8;
    buf[off++] = product & 0xFF; buf[off++] = product >> 8;
    buf[off++] = cnt;
    if (mac) for (int i = 0; i < 6; i++) buf[off++] = MAC[5 - i];   /* wire order is reversed */
    if (cap) buf[off++] = 0x00;
    if (fc & FC_OBJ) {
        buf[off++] = obj_id & 0xFF; buf[off++] = obj_id >> 8;
        buf[off++] = olen;
        memcpy(buf + off, payload, olen); off += olen;
    }
    return off;
}

int main(void)
{
    uint8_t buf[64];
    mibeacon_t m;

    /* temperature 23.4 C -> 234 = 0x00EA, int16 LE */
    uint8_t t[] = { 0xEA, 0x00 };
    size_t n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 7, true, false, 0x1004, t, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK);
    assert(m.product_id == MIBEACON_PRODUCT_MIFLORA && m.frame_cnt == 7);
    assert(memcmp(m.mac, MAC, 6) == 0);
    assert(m.has_temp && m.temp_dc == 234);
    assert(!m.has_moisture && !m.has_lux && !m.has_conductivity && !m.has_battery);

    /* negative temperature -5.1 C -> -51 int16 LE */
    uint8_t tneg[] = { 0xCD, 0xFF };
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 8, true, false, 0x1004, tneg, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK && m.temp_dc == -51);

    /* moisture 42 % */
    uint8_t mo[] = { 42 };
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 9, true, false, 0x1008, mo, 1);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK && m.has_moisture && m.moisture_pct == 42);

    /* illuminance 70000 lux = 0x011170, uint24 LE */
    uint8_t lx[] = { 0x70, 0x11, 0x01 };
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 10, true, false, 0x1007, lx, 3);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK && m.has_lux && m.lux == 70000);

    /* conductivity 1152 uS/cm = 0x0480 LE */
    uint8_t co[] = { 0x80, 0x04 };
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 11, true, false, 0x1009, co, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK && m.has_conductivity && m.conductivity_us == 1152);

    /* capability byte between MAC and object is skipped correctly */
    n = build(buf, FC_MAC | FC_CAP | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 12, true, true, 0x1008, mo, 1);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK && m.moisture_pct == 42);

    /* unknown object id -> OK but no values set */
    uint8_t junk[] = { 1, 2 };
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 13, true, false, 0x1234, junk, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_OK);
    assert(!m.has_temp && !m.has_moisture && !m.has_lux && !m.has_conductivity && !m.has_battery);

    /* encrypted frame rejected */
    n = build(buf, FC_ENC | FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 14, true, false, 0x1004, t, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_ERR_ENCRYPTED);

    /* no MAC flag rejected */
    n = build(buf, FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 15, false, false, 0x1004, t, 2);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_ERR_NO_MAC);

    /* no object flag rejected */
    n = build(buf, FC_MAC, MIBEACON_PRODUCT_MIFLORA, 16, true, false, 0, NULL, 0);
    assert(mibeacon_parse(buf, n, &m) == MIBEACON_ERR_NO_OBJECT);

    /* truncated: object payload length exceeds buffer */
    n = build(buf, FC_MAC | FC_OBJ, MIBEACON_PRODUCT_MIFLORA, 17, true, false, 0x1004, t, 2);
    assert(mibeacon_parse(buf, n - 1, &m) == MIBEACON_ERR_TRUNCATED);

    /* shorter than header */
    assert(mibeacon_parse(buf, 4, &m) == MIBEACON_ERR_TRUNCATED);

    printf("test_mibeacon: OK\n");
    return 0;
}
