#include "radio_role_str.h"
#include <string.h>

static const char *const NAMES[] = { "wifi_only", "ble", "zigbee" };

const char *radio_role_str(radio_role_t role)
{
    if ((int)role < 0 || (int)role > (int)RADIO_ROLE_ZIGBEE) return NAMES[RADIO_ROLE_WIFI_ONLY];
    return NAMES[role];
}

bool radio_role_parse(const char *s, radio_role_t *out)
{
    if (!s) return false;
    for (int i = 0; i <= (int)RADIO_ROLE_ZIGBEE; i++) {
        if (strcmp(s, NAMES[i]) == 0) {
            *out = (radio_role_t)i;
            return true;
        }
    }
    return false;
}
