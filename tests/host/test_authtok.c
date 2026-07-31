#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "authtok.h"

int main(void)
{
    uint8_t in[32], out[32];
    char hex[65];

    for (int i = 0; i < 32; i++) in[i] = (uint8_t)(i * 7 + 3);
    authtok_hex_encode(in, hex);
    assert(strlen(hex) == 64);
    for (int i = 0; i < 64; i++) assert(!(hex[i] >= 'A' && hex[i] <= 'F'));  /* lowercase */
    assert(authtok_hex_decode(hex, out) && memcmp(in, out, 32) == 0);

    /* uppercase accepted */
    char upper[65];
    for (int i = 0; i < 65; i++) upper[i] = (hex[i] >= 'a' && hex[i] <= 'f') ? hex[i] - 32 : hex[i];
    assert(authtok_hex_decode(upper, out) && memcmp(in, out, 32) == 0);

    /* malformed rejected: short, long, non-hex, empty */
    assert(!authtok_hex_decode("abc", out));
    char long65[66];
    memcpy(long65, hex, 64); long65[64] = 'a'; long65[65] = '\0';
    assert(!authtok_hex_decode(long65, out));
    char bad[65];
    memcpy(bad, hex, 65); bad[10] = 'g';
    assert(!authtok_hex_decode(bad, out));
    assert(!authtok_hex_decode("", out));

    /* ct_equal */
    uint8_t a[32], b[32];
    memset(a, 0x5A, 32); memset(b, 0x5A, 32);
    assert(authtok_ct_equal(a, b));
    b[31] ^= 1;
    assert(!authtok_ct_equal(a, b));
    b[31] ^= 1; b[0] ^= 0x80;
    assert(!authtok_ct_equal(a, b));

    printf("test_authtok: OK\n");
    return 0;
}
