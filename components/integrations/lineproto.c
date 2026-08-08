#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include "lineproto.h"

/* Escapes src for embedding as an Influx line-protocol STRING FIELD value
 * (caller supplies the surrounding quotes): '"' and '\' are the only two
 * characters line protocol requires escaped inside a string field, and each
 * becomes backslash + itself. Any control character (0x00-0x1F or 0x7F --
 * an embedded newline/CR/tab included) is replaced with '_' outright rather
 * than escaped: this component's batch buffer is a sequence of '\n'-
 * terminated lines (see lineproto_append() below and influx.c's build_batch()),
 * so a real newline let through here would insert a literal line break into
 * a quoted field, corrupting not just this field but the line boundary for
 * the rest of the batch that follows it. */
static void escape_string_field(const char *src, char *dst, size_t dst_size)
{
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        if (*p == '"' || *p == '\\') {
            if (pos + 2 >= dst_size) break;
            dst[pos++] = '\\';
            dst[pos++] = (char)*p;
        } else if (*p < 0x20 || *p == 0x7F) {
            if (pos + 1 >= dst_size) break;
            dst[pos++] = '_';
        } else {
            if (pos + 1 >= dst_size) break;
            dst[pos++] = (char)*p;
        }
    }
    dst[pos] = '\0';
}

bool lineproto_append(char *buf, size_t cap, size_t *off, const lp_point_t *p)
{
    /* Check if at least one field is set */
    if (!p->has_temp && !p->has_moisture && !p->has_lux &&
        !p->has_conductivity && !p->has_battery) {
        return false;
    }

    /* Build the line protocol in a scratch buffer. Every snprintf's return
     * value is checked against the space remaining in scratch before pos
     * advances, and the whole append fails on truncation -- pos += snprintf
     * unchecked would let a future wider field silently walk pos past
     * sizeof(scratch), and every write after that would then be relative to
     * a bogus (or, since snprintf's return is the would-be length, possibly
     * huge) offset. */
    char scratch[1024];
    size_t pos = 0;
    int n;

    /* Measurement and plant tag (always present; numeric, never needs
     * line-protocol tag-value escaping) */
    n = snprintf(scratch + pos, sizeof(scratch) - pos,
                    "plant,plant=%u ", (unsigned)p->plant_id);
    if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
    pos += (size_t)n;

    /* Fields (only if corresponding flag/value is set) */
    bool first = true;
    if (p->has_temp) {
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%stemp=%.1f", first ? "" : ",", p->temp_c);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }
    if (p->has_moisture) {
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%smoisture=%" PRIu8 "i", first ? "" : ",", p->moisture_pct);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }
    if (p->has_lux) {
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%slux=%" PRIu32 "i", first ? "" : ",", p->lux);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }
    if (p->has_conductivity) {
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%sconductivity=%" PRIu16 "i", first ? "" : ",", p->conductivity);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }
    if (p->has_battery) {
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%sbattery=%" PRIu8 "i", first ? "" : ",", p->battery_pct);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }

    /* name (string field, only if non-empty) */
    if (p->name[0] != '\0') {
        char escaped_name[128];
        escape_string_field(p->name, escaped_name, sizeof(escaped_name));
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%sname=\"%s\"", first ? "" : ",", escaped_name);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }

    /* sensor (string field: assigned probe's mac12, only if non-empty) */
    if (p->sensor_mac12[0] != '\0') {
        char escaped_sensor[32];
        escape_string_field(p->sensor_mac12, escaped_sensor, sizeof(escaped_sensor));
        n = snprintf(scratch + pos, sizeof(scratch) - pos,
                        "%ssensor=\"%s\"", first ? "" : ",", escaped_sensor);
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }

    /* Timestamp */
    n = snprintf(scratch + pos, sizeof(scratch) - pos,
                    " %" PRId64 "\n", p->epoch_s);
    if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
    pos += (size_t)n;

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
