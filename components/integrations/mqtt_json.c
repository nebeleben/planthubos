#include "mqtt_json.h"
#include <stdio.h>
#include <string.h>

bool mqtt_topic_state(char *out, size_t cap, const char *hub, const char *mac12)
{
    int n = snprintf(out, cap, "planthub/%s/%s/state", hub, mac12);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_topic_avail(char *out, size_t cap, const char *hub)
{
    int n = snprintf(out, cap, "planthub/%s/status", hub);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_topic_discovery(char *out, size_t cap, const char *mac12, const char *metric)
{
    int n = snprintf(out, cap, "homeassistant/sensor/planthub_%s_%s/config", mac12, metric);
    return n >= 0 && (size_t)n < cap;
}

bool mqtt_json_state(char *out, size_t cap, const mqtt_state_t *st)
{
    size_t pos = 0;
    int n;
    bool first = true;

    /* Start with opening brace */
    n = snprintf(out + pos, cap - pos, "{");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* Add optional fields */
    if (st->has_temp) {
        n = snprintf(out + pos, cap - pos, "%s\"temp\":%.1f", first ? "" : ",", st->temp_c);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
        first = false;
    }

    if (st->has_moisture) {
        n = snprintf(out + pos, cap - pos, "%s\"moisture\":%u", first ? "" : ",", st->moisture_pct);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
        first = false;
    }

    if (st->has_lux) {
        n = snprintf(out + pos, cap - pos, "%s\"lux\":%lu", first ? "" : ",", (unsigned long)st->lux);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
        first = false;
    }

    if (st->has_conductivity) {
        n = snprintf(out + pos, cap - pos, "%s\"conductivity\":%u", first ? "" : ",", st->conductivity);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
        first = false;
    }

    if (st->has_battery) {
        n = snprintf(out + pos, cap - pos, "%s\"battery\":%u", first ? "" : ",", st->battery_pct);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
        first = false;
    }

    /* rssi always emitted */
    n = snprintf(out + pos, cap - pos, "%s\"rssi\":%d}", first ? "" : ",", st->rssi);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    return true;
}

/* Metric metadata table */
typedef struct {
    const char *metric;
    const char *dev_cla;        /* NULL for conductivity */
    const char *unit;
    const char *val_tpl;
    const char *stat_cla;
} metric_info_t;

static const metric_info_t metrics[] = {
    {
        .metric = "temp",
        .dev_cla = "temperature",
        .unit = "°C",
        .val_tpl = "{{ value_json.temp }}",
        .stat_cla = "measurement",
    },
    {
        .metric = "moisture",
        .dev_cla = "moisture",
        .unit = "%",
        .val_tpl = "{{ value_json.moisture }}",
        .stat_cla = "measurement",
    },
    {
        .metric = "lux",
        .dev_cla = "illuminance",
        .unit = "lx",
        .val_tpl = "{{ value_json.lux }}",
        .stat_cla = "measurement",
    },
    {
        .metric = "conductivity",
        .dev_cla = NULL,
        .unit = "µS/cm",
        .val_tpl = "{{ value_json.conductivity }}",
        .stat_cla = "measurement",
    },
    {
        .metric = "battery",
        .dev_cla = "battery",
        .unit = "%",
        .val_tpl = "{{ value_json.battery }}",
        .stat_cla = "measurement",
    },
};

static const metric_info_t *find_metric(const char *metric)
{
    for (size_t i = 0; i < sizeof(metrics) / sizeof(metrics[0]); i++) {
        if (strcmp(metrics[i].metric, metric) == 0) {
            return &metrics[i];
        }
    }
    return NULL;
}

/* Escapes src for embedding as the body of a JSON string (caller supplies
 * the surrounding quotes): '"' and '\' become \" / \\, and any control
 * character (<0x20) becomes \u00XX. Without this, a sensor name containing
 * either character (typed by a user into the webui, not validated against
 * any charset -- see app_config's sensor-name setter) would land unescaped
 * in hand-rolled JSON here, producing a malformed discovery payload that HA
 * silently drops. dst is always NUL-terminated; if src would not fit, the
 * output is truncated at a whole-escape-unit boundary rather than
 * overflowing dst -- a truncated but well-formed name beats no discovery
 * config at all. */
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

bool mqtt_json_discovery(char *out, size_t cap, const char *hub, const char *mac12,
                          const char *sensor_name, const char *metric)
{
    const metric_info_t *info = find_metric(metric);
    if (info == NULL) {
        return false;
    }

    size_t pos = 0;
    int n;

    /* Use mac12 if sensor_name is empty; either way, escape before it goes
     * into either JSON string field below (mac12 never needs escaping, but
     * running it through json_escape too keeps this branch-free). */
    const char *raw_display_name = (sensor_name && *sensor_name) ? sensor_name : mac12;
    char display_name[6 * 32 + 1];   /* worst case: every byte of a 32-char name becomes \u00XX */
    json_escape(raw_display_name, display_name, sizeof display_name);

    /* Start with opening brace */
    n = snprintf(out + pos, cap - pos, "{");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* name: display_name metric */
    n = snprintf(out + pos, cap - pos, "\"name\":\"%s %s\",", display_name, metric);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* uniq_id */
    n = snprintf(out + pos, cap - pos, "\"uniq_id\":\"planthub_%s_%s\",", mac12, metric);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* stat_t */
    n = snprintf(out + pos, cap - pos, "\"stat_t\":\"planthub/%s/%s/state\",", hub, mac12);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* avty_t */
    n = snprintf(out + pos, cap - pos, "\"avty_t\":\"planthub/%s/status\",", hub);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* val_tpl */
    n = snprintf(out + pos, cap - pos, "\"val_tpl\":\"%s\",", info->val_tpl);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* dev_cla (omitted for conductivity) */
    if (info->dev_cla != NULL) {
        n = snprintf(out + pos, cap - pos, "\"dev_cla\":\"%s\",", info->dev_cla);
        if (n < 0 || (size_t)n >= cap - pos) return false;
        pos += n;
    }

    /* unit_of_meas - use unicode escapes for special characters */
    if (strcmp(info->unit, "°C") == 0) {
        n = snprintf(out + pos, cap - pos, "\"unit_of_meas\":\"\\u00b0C\",");
    } else if (strcmp(info->unit, "µS/cm") == 0) {
        n = snprintf(out + pos, cap - pos, "\"unit_of_meas\":\"\\u00b5S/cm\",");
    } else {
        n = snprintf(out + pos, cap - pos, "\"unit_of_meas\":\"%s\",", info->unit);
    }
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* stat_cla */
    n = snprintf(out + pos, cap - pos, "\"stat_cla\":\"%s\",", info->stat_cla);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* dev object */
    n = snprintf(out + pos, cap - pos, "\"dev\":{\"ids\":[\"planthub_%s\"],\"name\":\"%s\",\"mf\":\"PlantHub\",\"via_device\":\"%s\"}}",
                 mac12, display_name, hub);
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    return true;
}
