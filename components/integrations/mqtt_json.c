#include "mqtt_json.h"
#include <stdio.h>
#include <string.h>

bool mqtt_topic_state(char *out, size_t cap, const char *hub, uint8_t plant_id)
{
    int n = snprintf(out, cap, "planthub/%s/plant/%u/state", hub, (unsigned)plant_id);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_topic_device_state(char *out, size_t cap, const char *hub, const char *dev_id_str)
{
    int n = snprintf(out, cap, "planthub/%s/device/%s/state", hub, dev_id_str);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_topic_avail(char *out, size_t cap, const char *hub)
{
    int n = snprintf(out, cap, "planthub/%s/status", hub);
    return n >= 0 && (size_t)n < cap;
}

/* Copies name into out, turning every '.' into '_' -- the entity-id metric
 * segment (V1 pattern, spec Sec.6) derived from a capability's dotted name.
 * Truncates (still NUL-terminated) rather than overflow if name somehow
 * didn't fit, though every real capability name is well under out_cap. */
static void metric_segment(const char *name, char *out, size_t out_cap)
{
    size_t i = 0;
    for (; name[i] != '\0' && i + 1 < out_cap; i++)
        out[i] = (name[i] == '.') ? '_' : name[i];
    out[i] = '\0';
}

bool mqtt_topic_discovery(char *out, size_t cap, uint8_t plant_id, uint8_t cap_id)
{
    const capability_t *c = capability_get(cap_id);
    if (!c) return false;

    char metric[32];
    metric_segment(c->name, metric, sizeof metric);

    int n = snprintf(out, cap, "homeassistant/sensor/planthub_plant_%u_%s/config",
                      (unsigned)plant_id, metric);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_topic_device_discovery(char *out, size_t cap, const char *dev_id_str, uint8_t cap_id)
{
    const capability_t *c = capability_get(cap_id);
    if (!c) return false;

    char metric[32];
    metric_segment(c->name, metric, sizeof metric);

    int n = snprintf(out, cap, "homeassistant/sensor/planthub_device_%s_%s/config",
                      dev_id_str, metric);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_json_state(char *out, size_t cap, const mqtt_state_t *st)
{
    size_t pos = 0;
    int n;
    bool first = true;

    n = snprintf(out + pos, cap - pos, "{");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    for (uint8_t i = 0; i < CAPABILITY_COUNT; i++) {
        if (!st->present[i]) continue;
        const capability_t *c = capability_get(i);
        n = snprintf(out + pos, cap - pos, "%s\"%s\":%.*f", first ? "" : ",",
                     c->name, c->precision, (double)st->value[i]);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += (size_t)n;
        first = false;
    }

    n = snprintf(out + pos, cap - pos, "}");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    return true;
}

/* Escapes src for embedding as the body of a JSON string (caller supplies
 * the surrounding quotes): '"' and '\' become \" / \\, and any control
 * character (<0x20) becomes \u00XX. Without this, a plant name containing
 * either character (typed by a user into the webui, not validated against
 * any charset -- see plants_rename()) would land unescaped in hand-rolled
 * JSON here, producing a malformed discovery payload that HA silently
 * drops. dst is always NUL-terminated; if src would not fit, the output is
 * truncated at a whole-escape-unit boundary rather than overflowing dst --
 * a truncated but well-formed name beats no discovery config at all. */
static void json_escape(const char *src, char *dst, size_t dst_cap)
{
    if (dst_cap == 0) return;
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; p++) {
        char unit[8];   /* longest unit is "\u00XX" -- 6 chars + NUL */
        size_t ulen;
        if (*p == '"' || *p == '\\') {
            unit[0] = '\\';
            unit[1] = (char)*p;
            ulen = 2;
        } else if (*p < 0x20) {
            snprintf(unit, sizeof unit, "\\u%04x", (unsigned)*p);
            ulen = 6;
        } else {
            unit[0] = (char)*p;
            ulen = 1;
        }
        if (pos + ulen >= dst_cap) break;   /* leave room for the NUL below */
        memcpy(dst + pos, unit, ulen);
        pos += ulen;
    }
    dst[pos] = '\0';
}

/* HA's spelling for the capability table's unit string. Only "lux" -> "lx"
 * remains here (Triage item 9 fixwave, controller ruling): the table used
 * to store ASCII "C" and this function translated it to the JSON-escaped
 * "°C" HA expects, but the table itself now stores UTF-8 "°C" directly
 * (capability.c) -- writing that straight into a JSON string needs no
 * escaping (°  isn't a JSON control/quote/backslash character), so that
 * branch would now be permanently dead (table_unit is never the bare ASCII
 * "C" any more) and has been removed rather than kept as a no-op. Every
 * other unit in the table (%, µS/cm, hPa, dBm) is already what HA expects,
 * unchanged. */
static const char *ha_unit_json(const char *table_unit)
{
    if (strcmp(table_unit, "lux") == 0) return "lx";
    return table_unit;
}

/* Shared by mqtt_json_discovery()/mqtt_json_device_discovery() below: both
 * emit the identical payload shape (spec Sec.6), differing only in WHOSE
 * identity/topic they carry. uniq_prefix is both the "dev":{"ids":[...]}
 * entry AND (with "_<metric>" appended) the uniq_id -- true for both a
 * plant ("planthub_plant_7") and a device ("planthub_device_ble:AABBCC..")
 * identity string. state_topic is the already-fully-built stat_t value
 * (mqtt_topic_state()'s or mqtt_topic_device_state()'s shape -- callers
 * build it, this function just embeds it). raw_display_name "" falls back
 * to whatever the caller already decided (a plant's "Plant <id>" or a
 * device's id string) -- callers resolve their own fallback before calling,
 * this function only escapes and embeds it. */
static bool build_discovery(char *out, size_t cap, const char *hub, const char *uniq_prefix,
                            const char *state_topic, const char *raw_display_name, uint8_t cap_id)
{
    const capability_t *c = capability_get(cap_id);
    if (!c) return false;

    char metric[32];
    metric_segment(c->name, metric, sizeof metric);

    size_t pos = 0;
    int n;

    char display_name[6 * 32 + 1];   /* worst case: every byte of a 32-char name becomes \u00XX */
    json_escape(raw_display_name, display_name, sizeof display_name);

    n = snprintf(out + pos, cap - pos, "{");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* name: display_name capability-name */
    n = snprintf(out + pos, cap - pos, "\"name\":\"%s %s\",", display_name, c->name);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* uniq_id: V1 entity-id pattern, spec Sec.6 */
    n = snprintf(out + pos, cap - pos, "\"uniq_id\":\"%s_%s\",", uniq_prefix, metric);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* stat_t */
    n = snprintf(out + pos, cap - pos, "\"stat_t\":\"%s\",", state_topic);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* avty_t */
    n = snprintf(out + pos, cap - pos, "\"avty_t\":\"planthub/%s/status\",", hub);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* val_tpl -- bracket syntax: the field key contains '.' (capability
     * name), which dotted Jinja attribute access would not traverse. */
    n = snprintf(out + pos, cap - pos, "\"val_tpl\":\"{{ value_json['%s'] }}\",", c->name);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* dev_cla (omitted when the table has none, e.g. soil.conductivity) */
    if (c->ha_device_class != NULL) {
        n = snprintf(out + pos, cap - pos, "\"dev_cla\":\"%s\",", c->ha_device_class);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += (size_t)n;
    }

    /* unit_of_meas -- HA spelling, see ha_unit_json() above */
    n = snprintf(out + pos, cap - pos, "\"unit_of_meas\":\"%s\",", ha_unit_json(c->unit));
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* suggested_display_precision, from the capability table */
    n = snprintf(out + pos, cap - pos, "\"suggested_display_precision\":%u,", (unsigned)c->precision);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* stat_cla */
    n = snprintf(out + pos, cap - pos, "\"stat_cla\":\"measurement\",");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    /* dev object */
    n = snprintf(out + pos, cap - pos, "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"PlantHub\",\"via_device\":\"%s\"}}",
                 uniq_prefix, display_name, hub);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += (size_t)n;

    return true;
}

bool mqtt_json_discovery(char *out, size_t cap, const char *hub, uint8_t plant_id,
                          const char *plant_name, uint8_t cap_id)
{
    /* Fall back to "Plant <id>" when plant_name is empty -- build_discovery()
     * escapes whichever raw name it's handed, so the fallback itself never
     * needs escaping (it's built from a %u, not user input). */
    char fallback[16];   /* "Plant " + up to 3 digits + NUL */
    snprintf(fallback, sizeof fallback, "Plant %u", (unsigned)plant_id);
    const char *raw_display_name = (plant_name && *plant_name) ? plant_name : fallback;

    char uniq_prefix[24];   /* "planthub_plant_" + up to 3 digits + NUL */
    snprintf(uniq_prefix, sizeof uniq_prefix, "planthub_plant_%u", (unsigned)plant_id);

    char state_topic[64];
    if (!mqtt_topic_state(state_topic, sizeof state_topic, hub, plant_id)) return false;

    return build_discovery(out, cap, hub, uniq_prefix, state_topic, raw_display_name, cap_id);
}

bool mqtt_json_device_discovery(char *out, size_t cap, const char *hub, const char *dev_id_str,
                                const char *display_name, uint8_t cap_id)
{
    const char *raw_display_name = (display_name && *display_name) ? display_name : dev_id_str;

    char uniq_prefix[48];   /* "planthub_device_" + dev_id_str (<=23) + NUL */
    int n = snprintf(uniq_prefix, sizeof uniq_prefix, "planthub_device_%s", dev_id_str);
    if (n < 0 || (size_t)n >= sizeof uniq_prefix) return false;

    char state_topic[96];
    if (!mqtt_topic_device_state(state_topic, sizeof state_topic, hub, dev_id_str)) return false;

    return build_discovery(out, cap, hub, uniq_prefix, state_topic, raw_display_name, cap_id);
}
