#include "wifi_fsm.h"

void wifi_fsm_init(wifi_fsm_t *f, int max_retries)
{
    f->state = WIFI_ST_BOOT;
    f->retries = 0;
    f->max_retries = max_retries;
}

wifi_action_t wifi_fsm_step(wifi_fsm_t *f, wifi_fsm_event_t ev)
{
    switch (ev) {
    case WIFI_EV_CREDS_PRESENT:
    case WIFI_EV_NEW_CREDS:
        f->state = WIFI_ST_STA_CONNECTING;
        f->retries = 0;
        return WIFI_ACT_START_STA;

    case WIFI_EV_NO_CREDS:
        f->state = WIFI_ST_AP_MODE;
        return WIFI_ACT_START_AP;

    case WIFI_EV_GOT_IP:
        f->state = WIFI_ST_STA_CONNECTED;
        f->retries = 0;
        return WIFI_ACT_NONE;

    case WIFI_EV_DISCONNECTED:
        if (f->state == WIFI_ST_AP_MODE) return WIFI_ACT_NONE;
        f->state = WIFI_ST_STA_CONNECTING;
        f->retries++;
        if (f->retries > f->max_retries) {
            f->state = WIFI_ST_AP_MODE;
            return WIFI_ACT_START_AP;
        }
        return WIFI_ACT_RECONNECT;
    }
    return WIFI_ACT_NONE;
}
