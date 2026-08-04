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

bool mqtt_json_discovery(char *out, size_t cap, const char *hub, const char *mac12,
                          const char *sensor_name, const char *metric)
{
    const metric_info_t *info = find_metric(metric);
    if (info == NULL) {
        return false;
    }

    size_t pos = 0;
    int n;

    /* Use mac12 if sensor_name is empty */
    const char *display_name = (sensor_name && *sensor_name) ? sensor_name : mac12;

    /* Start with opening brace */
    n = snprintf(out + pos, cap - pos, "{");
    if (n < 0 || (size_t)n >= cap - pos) return false;
    pos += n;

    /* name: display_name metric (or just mac12 if no name) */
    if (sensor_name && *sensor_name) {
        n = snprintf(out + pos, cap - pos, "\"name\":\"%s %s\",", sensor_name, metric);
    } else {
        n = snprintf(out + pos, cap - pos, "\"name\":\"%s %s\",", mac12, metric);
    }
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

    /* unit_of_meas - use unicode escape for µ */
    if (strcmp(info->unit, "µS/cm") == 0) {
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
