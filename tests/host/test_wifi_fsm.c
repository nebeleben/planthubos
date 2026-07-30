#include <assert.h>
#include <stdio.h>
#include "wifi_fsm.h"

int main(void)
{
    wifi_fsm_t f;

    /* boot with creds -> connect; success path */
    wifi_fsm_init(&f, 5);
    assert(wifi_fsm_step(&f, WIFI_EV_CREDS_PRESENT) == WIFI_ACT_START_STA);
    assert(f.state == WIFI_ST_STA_CONNECTING);
    assert(wifi_fsm_step(&f, WIFI_EV_GOT_IP) == WIFI_ACT_NONE);
    assert(f.state == WIFI_ST_STA_CONNECTED);

    /* connected drop -> reconnect attempts, then AP after max_retries */
    assert(wifi_fsm_step(&f, WIFI_EV_DISCONNECTED) == WIFI_ACT_RECONNECT);
    assert(f.state == WIFI_ST_STA_CONNECTING);
    for (int i = 0; i < 4; i++)
        assert(wifi_fsm_step(&f, WIFI_EV_DISCONNECTED) == WIFI_ACT_RECONNECT);
    assert(wifi_fsm_step(&f, WIFI_EV_DISCONNECTED) == WIFI_ACT_START_AP);
    assert(f.state == WIFI_ST_AP_MODE);

    /* AP mode + new creds -> try STA again with fresh retry budget */
    assert(wifi_fsm_step(&f, WIFI_EV_NEW_CREDS) == WIFI_ACT_START_STA);
    assert(f.state == WIFI_ST_STA_CONNECTING);
    assert(f.retries == 0);

    /* boot without creds -> straight to AP */
    wifi_fsm_init(&f, 5);
    assert(wifi_fsm_step(&f, WIFI_EV_NO_CREDS) == WIFI_ACT_START_AP);
    assert(f.state == WIFI_ST_AP_MODE);

    /* getting IP resets the retry counter */
    wifi_fsm_init(&f, 5);
    wifi_fsm_step(&f, WIFI_EV_CREDS_PRESENT);
    wifi_fsm_step(&f, WIFI_EV_DISCONNECTED);
    wifi_fsm_step(&f, WIFI_EV_GOT_IP);
    assert(f.retries == 0);

    printf("test_wifi_fsm: OK\n");
    return 0;
}
