#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>
#include <math.h>
#include "lineproto.h"

/* Appends prefix (a complete "measurement,tag=value " string the caller
 * already built) plus one field per present[i] (named by influx_field,
 * formatted per capability.h's precision -- see lineproto.h) plus the
 * timestamp, to buf+*off. Shared by lineproto_append_plant()/_device()
 * below -- the only difference between the two is the measurement/tag
 * prefix. Same "whole scratch buffer or nothing" discipline as V1: every
 * snprintf's return value is checked against the space remaining in
 * scratch before pos advances, and the whole append fails on truncation. */
static bool append_point(char *buf, size_t cap, size_t *off, const char *prefix,
                         const lp_fields_t *f, int64_t epoch_s)
{
    bool any = false;
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
        if (f->present[i]) { any = true; break; }
    }
    if (!any) return false;

    char scratch[512];
    size_t pos = 0;
    int n;

    n = snprintf(scratch + pos, sizeof(scratch) - pos, "%s", prefix);
    if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
    pos += (size_t)n;

    bool first = true;
    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
        if (!f->present[i]) continue;
        const capability_t *c = capability_get(i);
        if (c->precision == 0) {
            n = snprintf(scratch + pos, sizeof(scratch) - pos, "%s%s=%ldi",
                         first ? "" : ",", c->influx_field, lroundf(f->value[i]));
        } else {
            n = snprintf(scratch + pos, sizeof(scratch) - pos, "%s%s=%.*f",
                         first ? "" : ",", c->influx_field, c->precision, (double)f->value[i]);
        }
        if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
        pos += (size_t)n;
        first = false;
    }

    n = snprintf(scratch + pos, sizeof(scratch) - pos, " %" PRId64 "\n", epoch_s);
    if (n < 0 || (size_t)n >= sizeof(scratch) - pos) return false;
    pos += (size_t)n;

    if (*off + pos + 1 > cap) return false;

    memcpy(buf + *off, scratch, pos);
    *off += pos;
    buf[*off] = '\0';   /* NUL-terminate for strcmp to work */
    return true;
}

bool lineproto_append_plant(char *buf, size_t cap, size_t *off, const lp_plant_point_t *p)
{
    char prefix[32];
    int n = snprintf(prefix, sizeof prefix, "plant,plant=%u ", (unsigned)p->plant_id);
    if (n < 0 || (size_t)n >= sizeof prefix) return false;
    return append_point(buf, cap, off, prefix, &p->fields, p->epoch_s);
}

bool lineproto_append_device(char *buf, size_t cap, size_t *off, const lp_device_point_t *p)
{
    char prefix[48];
    int n = snprintf(prefix, sizeof prefix, "device,device=%s ", p->dev_id_str);
    if (n < 0 || (size_t)n >= sizeof prefix) return false;
    return append_point(buf, cap, off, prefix, &p->fields, p->epoch_s);
}
