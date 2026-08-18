#include "events_json_escape.h"
#include <string.h>
#include <stdio.h>

void events_json_escape(const char *src, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) return;
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char unit[8];   /* longest unit is "\u00XX" -- 6 chars + NUL */
        size_t ulen;
        switch (*p) {
        case '"':  unit[0] = '\\'; unit[1] = '"';  ulen = 2; break;
        case '\\': unit[0] = '\\'; unit[1] = '\\'; ulen = 2; break;
        case '\b': unit[0] = '\\'; unit[1] = 'b';  ulen = 2; break;
        case '\f': unit[0] = '\\'; unit[1] = 'f';  ulen = 2; break;
        case '\n': unit[0] = '\\'; unit[1] = 'n';  ulen = 2; break;
        case '\r': unit[0] = '\\'; unit[1] = 'r';  ulen = 2; break;
        case '\t': unit[0] = '\\'; unit[1] = 't';  ulen = 2; break;
        default:
            if (*p < 0x20) {
                snprintf(unit, sizeof unit, "\\u%04x", (unsigned)*p);
                ulen = 6;
            } else {
                unit[0] = (char)*p;
                ulen = 1;
            }
            break;
        }
        if (pos + ulen >= dst_cap) break;   /* leave room for the NUL below */
        memcpy(dst + pos, unit, ulen);
        pos += ulen;
    }
    dst[pos] = '\0';
}
