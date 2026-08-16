#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bthome.h"
#include "capability.h"

static void expect_close(float got, float want, float eps, const char *what)
{
    if (fabsf(got - want) > eps) {
        fprintf(stderr, "%s: got %f, want %f\n", what, (double)got, (double)want);
        assert(0);
    }
}

int main(void)
{
    bthome_emit_t out[BTHOME_MAX_EMITS];
    size_t n;

    /* --- temperature + humidity, unencrypted (bthome.io format-page example
     * shape: device info 0x40, obj 0x02 sint16 x0.01, obj 0x03 uint16 x0.01) --- */
    {
        uint8_t p[] = { 0x40, 0x02, 0xCA, 0x09, 0x03, 0xBF, 0x13 };
        n = 0;
        assert(bthome_decode(p, sizeof p, NULL, NULL, out, &n) == BTHOME_OK);
        assert(n == 2);
        assert(out[0].cap_id == CAP_AIR_TEMPERATURE);
        expect_close(out[0].value, 25.06f, 0.001f, "temp");
        assert(out[1].cap_id == CAP_AIR_HUMIDITY);
        expect_close(out[1].value, 50.55f, 0.001f, "humidity");
    }

    /* --- battery + illuminance --- */
    {
        uint8_t p[] = { 0x40, 0x05, 0x13, 0x8A, 0x14, 0x01, 0x57 };
        n = 0;
        assert(bthome_decode(p, sizeof p, NULL, NULL, out, &n) == BTHOME_OK);
        assert(n == 2);
        assert(out[0].cap_id == CAP_LIGHT_ILLUMINANCE);
        expect_close(out[0].value, 13460.67f, 0.01f, "illuminance");
        assert(out[1].cap_id == CAP_BATTERY_LEVEL);
        expect_close(out[1].value, 87.0f, 0.001f, "battery");
    }

    /* --- unknown-but-sized object (voltage, 0x0C) in the middle is skipped;
     * the objects before and after it still decode --- */
    {
        uint8_t p[] = { 0x40, 0x02, 0xCA, 0x09, 0x0C, 0x34, 0x12, 0x01, 0x64 };
        n = 0;
        assert(bthome_decode(p, sizeof p, NULL, NULL, out, &n) == BTHOME_OK);
        assert(n == 2);
        assert(out[0].cap_id == CAP_AIR_TEMPERATURE);
        expect_close(out[0].value, 25.06f, 0.001f, "temp (skip-middle)");
        assert(out[1].cap_id == CAP_BATTERY_LEVEL);
        expect_close(out[1].value, 100.0f, 0.001f, "battery (skip-middle)");
    }

    /* --- truncated payloads never read past the buffer --- */
    {
        /* empty: not even the device-info byte */
        n = 0;
        assert(bthome_decode(NULL, 0, NULL, NULL, out, &n) == BTHOME_ERR_TRUNCATED);
        assert(n == 0);

        /* humidity (0x03) declares 2 bytes but only 1 is present */
        uint8_t p[] = { 0x40, 0x02, 0xCA, 0x09, 0x03, 0xBF };
        n = 0;
        assert(bthome_decode(p, sizeof p, NULL, NULL, out, &n) == BTHOME_ERR_TRUNCATED);
    }

    /* --- encryption bit set, no key supplied --- */
    {
        uint8_t p[] = { 0x41, 0x00 };
        n = 0;
        assert(bthome_decode(p, sizeof p, NULL, NULL, out, &n) == BTHOME_ERR_ENCRYPTED_NO_KEY);
    }

#ifndef BTHOME_NO_MBEDTLS_CCM
    /* --- encrypted vector, published by bthome.io/encryption (fetched
     * verbatim -- not self-constructed):
     *   MAC:        54:48:E6:8F:80:A5
     *   key:        231d39c1d7cc1ab1aee224cd096db932
     *   plaintext:  02ca0903bf13   (temperature 0x02 + humidity 0x03)
     *   counter:    1122867        (LE bytes 33 22 11 00)
     *   ciphertext: e445f3c9962b
     *   MIC:        6c7c4519
     *   service data (after the FCD2 UUID): 41 e445f3c9962b 33221100 6c7c4519
     * Nonce cross-checked against the site's own worked example
     * (5448e68f80a5 d2fc 41 33221100). */
    {
        const uint8_t mac[6] = { 0x54, 0x48, 0xE6, 0x8F, 0x80, 0xA5 };
        const uint8_t key[16] = {
            0x23, 0x1d, 0x39, 0xc1, 0xd7, 0xcc, 0x1a, 0xb1,
            0xae, 0xe2, 0x24, 0xcd, 0x09, 0x6d, 0xb9, 0x32,
        };
        uint8_t p[] = {
            0x41,                                     /* device info: encrypted, v2 */
            0xe4, 0x45, 0xf3, 0xc9, 0x96, 0x2b,        /* ciphertext */
            0x33, 0x22, 0x11, 0x00,                    /* counter, LE */
            0x6c, 0x7c, 0x45, 0x19,                    /* MIC */
        };
        n = 0;
        bthome_err_t err = bthome_decode(p, sizeof p, key, mac, out, &n);
        assert(err == BTHOME_OK);
        assert(n == 2);
        assert(out[0].cap_id == CAP_AIR_TEMPERATURE);
        expect_close(out[0].value, 25.06f, 0.001f, "encrypted temp");
        assert(out[1].cap_id == CAP_AIR_HUMIDITY);
        expect_close(out[1].value, 50.55f, 0.001f, "encrypted humidity");

        /* wrong key -> auth failure, not a crash or a silent bad decode */
        uint8_t bad_key[16];
        memcpy(bad_key, key, 16);
        bad_key[0] ^= 0xFF;
        n = 0;
        assert(bthome_decode(p, sizeof p, bad_key, mac, out, &n) == BTHOME_ERR_DECRYPT);
    }
#else
    fprintf(stderr, "test_bthome: mbedtls not available on this host build -- "
                     "encrypted-vector case SKIPPED (device build covers it; "
                     "see Task 3 report)\n");
#endif

    printf("test_bthome: OK\n");
    return 0;
}
