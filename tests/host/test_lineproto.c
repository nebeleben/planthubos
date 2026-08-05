#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "lineproto.h"

int main(void)
{
    char buf[512]; size_t off = 0;
    lp_point_t p = { .mac = {0x80,0xEA,0xCA,0x89,0x25,0x63},
                     .has_temp = 1, .temp_c = 27.3f,
                     .has_moisture = 1, .moisture_pct = 18,
                     .has_lux = 1, .lux = 585,
                     .has_conductivity = 1, .conductivity = 118,
                     .has_battery = 1, .battery_pct = 64,
                     .epoch_s = 1785780419 };
    strcpy(p.name, "Wohnzimmer Palme");
    assert(lineproto_append(buf, sizeof buf, &off, &p));
    assert(strcmp(buf, "plant,sensor=80EACA892563,name=Wohnzimmer\\ Palme "
                  "temp=27.3,moisture=18i,lux=585i,conductivity=118i,battery=64i "
                  "1785780419\n") == 0);

    /* no name -> no name tag; partial fields */
    off = 0;
    lp_point_t q = { .mac = {1,2,3,4,5,6}, .has_lux = 1, .lux = 42, .epoch_s = 100 };
    assert(lineproto_append(buf, sizeof buf, &off, &q));
    assert(strcmp(buf, "plant,sensor=010203040506 lux=42i 100\n") == 0);

    /* appends accumulate */
    assert(lineproto_append(buf, sizeof buf, &off, &q));
    assert(off == 2 * strlen("plant,sensor=010203040506 lux=42i 100\n"));

    /* nothing set -> rejected */
    off = 0;
    lp_point_t r = { .mac = {1,2,3,4,5,6}, .epoch_s = 100 };
    assert(!lineproto_append(buf, sizeof buf, &off, &r));
    assert(off == 0);

    /* doesn't fit -> rejected, off unchanged */
    char tiny[16]; off = 0;
    assert(!lineproto_append(tiny, sizeof tiny, &off, &q));
    assert(off == 0);

    /* tag escaping of = and , */
    off = 0;
    strcpy(q.name, "a=b,c");
    assert(lineproto_append(buf, sizeof buf, &off, &q));
    assert(strstr(buf, "name=a\\=b\\,c ") != NULL);

    /* backslash in a tag value is itself escaped */
    off = 0;
    strcpy(q.name, "a\\b");
    assert(lineproto_append(buf, sizeof buf, &off, &q));
    assert(strstr(buf, "name=a\\\\b ") != NULL);

    /* an embedded control char (newline) is replaced with '_', not escaped
     * or passed through -- letting it through would corrupt the line
     * boundary for the rest of the batch. */
    off = 0;
    strcpy(q.name, "a\nb");
    assert(lineproto_append(buf, sizeof buf, &off, &q));
    assert(strstr(buf, "name=a_b ") != NULL);

    printf("test_lineproto: OK\n");
    return 0;
}
