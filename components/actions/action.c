#include "action.h"
#include <string.h>

static const action_t ACTION_TABLE[ACTION_COUNT] = {
    [ACT_SWITCH_ON]       = { .id = ACT_SWITCH_ON, .name = "switch.on",
                              .param = ACTION_PARAM_NONE, .param_max = 0,
                              .ha_component = "switch" },
    [ACT_SWITCH_OFF]      = { .id = ACT_SWITCH_OFF, .name = "switch.off",
                              .param = ACTION_PARAM_NONE, .param_max = 0,
                              .ha_component = "switch" },
    [ACT_IRRIGATION_OPEN] = { .id = ACT_IRRIGATION_OPEN, .name = "irrigation.open",
                              .param = ACTION_PARAM_DURATION_S, .param_max = 300,
                              .ha_component = "button" },
    [ACT_PUMP_RUN]        = { .id = ACT_PUMP_RUN, .name = "pump.run",
                              .param = ACTION_PARAM_DURATION_S, .param_max = 120,
                              .ha_component = "button" },
};

const action_t *action_get(uint8_t id)
{
    return (id < ACTION_COUNT) ? &ACTION_TABLE[id] : NULL;
}

const action_t *action_by_name(const char *name)
{
    if (!name) return NULL;
    for (uint8_t i = 0; i < ACTION_COUNT; i++) {
        if (strcmp(ACTION_TABLE[i].name, name) == 0) return &ACTION_TABLE[i];
    }
    return NULL;
}

bool action_param_ok(uint8_t id, uint16_t param)
{
    const action_t *a = action_get(id);
    if (!a) return false;
    if (a->param == ACTION_PARAM_NONE) return param == 0;
    return param >= 1 && param <= a->param_max;
}
