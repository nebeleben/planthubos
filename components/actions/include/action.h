/* components/actions/include/action.h */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* The action vocabulary (spec section 1.1). Actions are NOT capabilities:
 * a capability is a measurement with a unit and an int16 value stored per
 * device, an action is a command with a typed parameter and a hard bound.
 * See the spec for why they are kept in separate vocabularies -- in short,
 * every capability id costs 192 B across the registry whether used or not,
 * and every consumer of that array would have to learn to skip actions. */
#define ACTION_COUNT 4
#define ACTION_NONE  0xFF

enum { ACT_SWITCH_ON = 0, ACT_SWITCH_OFF = 1,
       ACT_IRRIGATION_OPEN = 2, ACT_PUMP_RUN = 3 };

typedef enum { ACTION_PARAM_NONE = 0, ACTION_PARAM_DURATION_S = 1 } action_param_t;

typedef struct {
    uint8_t        id;
    const char    *name;          /* "irrigation.open" */
    action_param_t param;
    uint16_t       param_max;     /* the HARD bound; 0 when param == ACTION_PARAM_NONE */
    const char    *ha_component;  /* "switch" | "button", for HA discovery */
} action_t;

const action_t *action_get(uint8_t id);              /* NULL if unknown */
const action_t *action_by_name(const char *name);    /* NULL if unknown or NULL */

/* True when `param` is acceptable for action `id`. This is the hard safety
 * bound and the ONLY place it is decided. A parameterless action accepts
 * only 0; a parameterised one accepts 1..param_max -- zero is refused
 * because a zero-second open is not an open, it is a caller bug that would
 * otherwise look like a successful no-op. */
bool action_param_ok(uint8_t id, uint16_t param);
