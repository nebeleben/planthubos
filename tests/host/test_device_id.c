#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "capability.h"

int main(void) {
    const uint8_t mac[6] = {0xA4,0xC1,0x38,0x11,0x22,0x33};
    char buf[24];

    device_id_t ble = device_id_from_mac(DEV_KIND_BLE, mac);
    assert(ble.addr[6] == 0 && ble.addr[7] == 0);
    assert(strcmp(device_id_format(&ble, buf, sizeof buf), "ble:A4C138112233") == 0);

    device_id_t en = device_id_from_mac(DEV_KIND_ESPNOW, mac);
    assert(strcmp(device_id_format(&en, buf, sizeof buf), "espnow:A4C138112233") == 0);
    assert(!device_id_equal(&ble, &en));            /* kind participates in identity */

    device_id_t zb = { .kind = DEV_KIND_ZIGBEE,
                       .addr = {0x00,0x12,0x4B,0x00,0x0A,0x0B,0x0C,0x0D} };
    assert(strcmp(device_id_format(&zb, buf, sizeof buf), "zb:00124B000A0B0C0D") == 0);

    /* round-trip parse for all three kinds */
    device_id_t p;
    assert(device_id_parse("ble:A4C138112233", &p) && device_id_equal(&p, &ble));
    assert(device_id_parse("espnow:A4C138112233", &p) && device_id_equal(&p, &en));
    assert(device_id_parse("zb:00124B000A0B0C0D", &p) && device_id_equal(&p, &zb));
    /* colon MAC form accepted for ble (V1 habit) and pads identically */
    assert(device_id_parse("A4:C1:38:11:22:33", &p) && device_id_equal(&p, &ble));
    /* lower-case hex accepted on input, canonical output stays upper-case */
    assert(device_id_parse("ble:a4c138112233", &p) && device_id_equal(&p, &ble));

    /* rejections */
    assert(!device_id_parse("ble:A4C13811223", &p));      /* odd length */
    assert(!device_id_parse("ble:A4C1381122ZZ", &p));     /* non-hex */
    assert(!device_id_parse("zb:A4C138112233", &p));      /* zigbee needs 8 bytes */
    assert(!device_id_parse("wifi:A4C138112233", &p));    /* unknown kind */
    assert(!device_id_parse("", &p));
    /* truncation safety: short buffer must not overflow */
    char small[8];
    device_id_format(&zb, small, sizeof small);
    assert(strlen(small) < sizeof small);

    printf("test_device_id: all passed\n");
    return 0;
}
