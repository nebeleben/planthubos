/* Pure DS3231 register <-> epoch conversion -- no ESP-IDF, no libc time, so
 * tests/host/test_rtc_ds3231.c exercises it directly and the device and host
 * agree bit for bit. Civil-date math is Howard Hinnant's days_from_civil /
 * civil_from_days (proleptic Gregorian, integer-only). */
#include "rtc_ds3231.h"

static uint8_t bcd2bin(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t bin2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Days since 1970-01-01 for a civil date (y = full year, m = 1..12). */
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= (m <= 2);
    int32_t era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    uint32_t doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint32_t doe = (uint32_t)(z - era * 146097);
    uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t yy = (int32_t)yoe + era * 400;
    uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    uint32_t mp = (5u * doy + 2u) / 153u;
    *d = doy - (153u * mp + 2u) / 5u + 1u;
    *m = mp + (mp < 10u ? 3u : -9u);
    *y = yy + (*m <= 2u);
}

bool rtc_ds3231_decode(const uint8_t regs[7], bool osf, uint32_t *epoch_out)
{
    if (!epoch_out) return false;
    if (osf) return false;   /* oscillator stopped -> time lost */

    uint8_t s  = bcd2bin(regs[0] & 0x7F);
    uint8_t mi = bcd2bin(regs[1] & 0x7F);
    uint8_t h  = bcd2bin(regs[2] & 0x3F);   /* 24h mode: bits 5..0 */
    uint8_t dd = bcd2bin(regs[4] & 0x3F);
    uint8_t mo = bcd2bin(regs[5] & 0x1F);   /* strip century bit */
    int32_t yr = 2000 + bcd2bin(regs[6]);

    if (s > 59 || mi > 59 || h > 23) return false;
    if (dd < 1 || dd > 31 || mo < 1 || mo > 12) return false;
    if (yr < RTC_DS3231_MIN_YEAR || yr > 2099) return false;

    int32_t days = days_from_civil(yr, mo, dd);
    *epoch_out = (uint32_t)(days * 86400 + h * 3600 + mi * 60 + s);
    return true;
}

void rtc_ds3231_encode(uint32_t epoch, uint8_t regs[7])
{
    int32_t days = (int32_t)(epoch / 86400u);
    uint32_t tod = epoch % 86400u;
    uint8_t h  = (uint8_t)(tod / 3600u);
    uint8_t mi = (uint8_t)((tod % 3600u) / 60u);
    uint8_t s  = (uint8_t)(tod % 60u);

    int32_t y; uint32_t m, d;
    civil_from_days(days, &y, &m, &d);

    /* day-of-week, 1=Sunday..7=Saturday. 1970-01-01 (day 0) was a Thursday. */
    int32_t dow = (days % 7 + 4) % 7;   /* 0=Sun..6=Sat */
    if (dow < 0) dow += 7;

    regs[0] = bin2bcd(s);
    regs[1] = bin2bcd(mi);
    regs[2] = bin2bcd(h);                       /* 24h: bit6=0 */
    regs[3] = (uint8_t)(dow + 1);               /* 1..7 */
    regs[4] = bin2bcd(d);
    regs[5] = bin2bcd((uint8_t)m);              /* century bit 0 */
    regs[6] = bin2bcd((uint8_t)(y - 2000));
}
