#pragma once

typedef enum {
    WIFI_ST_BOOT,
    WIFI_ST_STA_CONNECTING,
    WIFI_ST_STA_CONNECTED,
    WIFI_ST_AP_MODE,
} wifi_state_t;

typedef enum {
    WIFI_EV_CREDS_PRESENT,
    WIFI_EV_NO_CREDS,
    WIFI_EV_GOT_IP,
    WIFI_EV_DISCONNECTED,
    WIFI_EV_NEW_CREDS,
} wifi_fsm_event_t;

typedef enum {
    WIFI_ACT_NONE,
    WIFI_ACT_START_STA,
    WIFI_ACT_START_AP,
    WIFI_ACT_RECONNECT,
} wifi_action_t;

typedef struct {
    wifi_state_t state;
    int retries;
    int max_retries;
} wifi_fsm_t;

void          wifi_fsm_init(wifi_fsm_t *f, int max_retries);
wifi_action_t wifi_fsm_step(wifi_fsm_t *f, wifi_fsm_event_t ev);
