/* Pure DS3231 register <-> epoch core, cross-checked against the host's
 * timegm() so no magic epoch numbers are hand-computed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "rtc_ds3231.h"

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Build the 7 DS3231 time registers by hand (BCD, 24h) for a civil UTC time. */
static void mk_regs(int Y, int Mo, int D, int h, int mi, int s, uint8_t r[7]) {
    r[0] = bcd(s);
    r[1] = bcd(mi);
    r[2] = bcd(h);                 /* 24h: bit6=0 */
    r[3] = 1;                      /* day-of-week: don't-care for decode */
    r[4] = bcd(D);
    r[5] = bcd(Mo);                /* century bit 0 */
    r[6] = bcd(Y - 2000);
}
static uint32_t host_epoch(int Y, int Mo, int D, int h, int mi, int s) {
    struct tm tm = {0};
    tm.tm_year = Y - 1900; tm.tm_mon = Mo - 1; tm.tm_mday = D;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return (uint32_t)timegm(&tm);
}

static void check_roundtrip(int Y, int Mo, int D, int h, int mi, int s) {
    uint8_t hand[7]; mk_regs(Y, Mo, D, h, mi, s, hand);
    uint32_t want = host_epoch(Y, Mo, D, h, mi, s);

    /* decode(hand regs) == host epoch */
    uint32_t got = 0;
    assert(rtc_ds3231_decode(hand, false, &got));
    assert(got == want);

    /* encode(epoch) reproduces the time fields (0,1,2,4,5,6; reg3 dow differs) */
    uint8_t enc[7]; rtc_ds3231_encode(want, enc);
    assert(enc[0] == hand[0] && enc[1] == hand[1] && enc[2] == hand[2]);
    assert(enc[4] == hand[4] && enc[5] == hand[5] && enc[6] == hand[6]);
    assert(enc[3] >= 1 && enc[3] <= 7);   /* day-of-week in range */

    /* and encode->decode round-trips to the same epoch */
    uint32_t rt = 0;
    assert(rtc_ds3231_decode(enc, false, &rt) && rt == want);
    printf("  %04d-%02d-%02d %02d:%02d:%02d -> %lu ok\n", Y, Mo, D, h, mi, s, (unsigned long)want);
}

int main(void) {
    printf("1 round-trips (incl. leap day):\n");
    check_roundtrip(2026, 10, 13, 12, 34, 56);
    check_roundtrip(2024,  2, 29, 23, 59, 59);   /* leap year */
    check_roundtrip(2025,  1,  1,  0,  0,  0);
    check_roundtrip(2099, 12, 31, 23, 59, 59);

    printf("2 oscillator-stop flag -> invalid\n");
    uint8_t r[7]; mk_regs(2026, 5, 5, 10, 0, 0, r);
    uint32_t e = 123;
    assert(!rtc_ds3231_decode(r, true, &e));   /* OSF set */
    assert(e == 123);                          /* out param untouched */

    printf("3 implausible year (2000, fresh chip) -> invalid\n");
    uint8_t r2[7]; mk_regs(2000, 1, 1, 0, 0, 0, r2);
    assert(!rtc_ds3231_decode(r2, false, &e));

    printf("4 out-of-range fields -> invalid\n");
    uint8_t r3[7]; mk_regs(2026, 5, 5, 10, 0, 0, r3);
    r3[2] = bcd(24);                            /* hour 24 */
    assert(!rtc_ds3231_decode(r3, false, &e));
    mk_regs(2026, 13, 5, 10, 0, 0, r3);         /* month 13 */
    assert(!rtc_ds3231_decode(r3, false, &e));

    printf("\nALL RTC-DS3231 CORE TESTS PASS\n");
    return 0;
}
