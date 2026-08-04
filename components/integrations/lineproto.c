#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "lineproto.h"

/* Escape a tag value: comma, equals, space each become backslash + char */
static size_t escape_tag_value(const char *src, char *dst, size_t dst_size)
{
    size_t pos = 0;
    for (const char *p = src; *p && pos + 2 < dst_size; p++) {
        if (*p == ',' || *p == '=' || *p == ' ') {
            if (pos + 2 >= dst_size) break;
            dst[pos++] = '\\';
            dst[pos++] = *p;
        } else {
            dst[pos++] = *p;
        }
    }
    dst[pos] = '\0';
    return pos;
}

bool lineproto_append(char *buf, size_t cap, size_t *off, const lp_point_t *p)
{
    /* Check if at least one field is set */
    if (!p->has_temp && !p->has_moisture && !p->has_lux &&
        !p->has_conductivity && !p->has_battery) {
        return false;
    }

    /* Build the line protocol in a scratch buffer */
    char scratch[1024];
    size_t pos = 0;

    /* Measurement and sensor tag (always present) */
    pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                    "plant,sensor=%02X%02X%02X%02X%02X%02X",
                    p->mac[0], p->mac[1], p->mac[2],
                    p->mac[3], p->mac[4], p->mac[5]);

    /* Name tag (only if non-empty) */
    if (p->name[0] != '\0') {
        char escaped_name[128];
        escape_tag_value(p->name, escaped_name, sizeof(escaped_name));
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        ",name=%s", escaped_name);
    }

    /* Space before fields */
    pos += snprintf(scratch + pos, sizeof(scratch) - pos, " ");

    /* Fields (only if corresponding flag is set) */
    bool first = true;
    if (p->has_temp) {
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%stemp=%.1f", first ? "" : ",", p->temp_c);
        first = false;
    }
    if (p->has_moisture) {
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%smoisture=%" PRIu8 "i", first ? "" : ",", p->moisture_pct);
        first = false;
    }
    if (p->has_lux) {
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%slux=%" PRIu32 "i", first ? "" : ",", p->lux);
        first = false;
    }
    if (p->has_conductivity) {
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%sconductivity=%" PRIu16 "i", first ? "" : ",", p->conductivity);
        first = false;
    }
    if (p->has_battery) {
        pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%sbattery=%" PRIu8 "i", first ? "" : ",", p->battery_pct);
        first = false;
    }

    /* Timestamp */
    pos += snprintf(scratch + pos, sizeof(scratch) - pos,
                    " %" PRId64 "\n", p->epoch_s);

    /* Check if it would fit (need space for the data plus null terminator) */
    if (*off + pos + 1 > cap) {
        return false;
    }

    /* Copy to buffer */
    memcpy(buf + *off, scratch, pos);
    *off += pos;
    /* Null-terminate for strcmp to work */
    buf[*off] = '\0';
    return true;
}
