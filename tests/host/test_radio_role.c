#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "radio_role_str.h"

int main(void)
{
    radio_role_t r;

    /* the three canonical strings parse to their enum */
    assert(radio_role_parse("wifi_only", &r) && r == RADIO_ROLE_WIFI_ONLY);
    assert(radio_role_parse("ble", &r) && r == RADIO_ROLE_BLE);
    assert(radio_role_parse("zigbee", &r) && r == RADIO_ROLE_ZIGBEE);

    /* str() round-trips */
    for (int i = 0; i <= (int)RADIO_ROLE_ZIGBEE; i++) {
        radio_role_t back;
        assert(radio_role_parse(radio_role_str((radio_role_t)i), &back) && back == (radio_role_t)i);
    }

    /* strict: case, whitespace, prefixes, empty, NULL all rejected and out untouched */
    r = RADIO_ROLE_BLE;
    assert(!radio_role_parse("BLE", &r));
    assert(!radio_role_parse("Zigbee", &r));
    assert(!radio_role_parse(" ble", &r));
    assert(!radio_role_parse("ble ", &r));
    assert(!radio_role_parse("wifi", &r));
    assert(!radio_role_parse("wifi_only_", &r));
    assert(!radio_role_parse("", &r));
    assert(!radio_role_parse(NULL, &r));
    assert(r == RADIO_ROLE_BLE);

    /* str() of an out-of-range value is a stable non-NULL string, never a crash */
    assert(radio_role_str((radio_role_t)99) != NULL);
    assert(strcmp(radio_role_str((radio_role_t)99), "wifi_only") == 0);

    printf("test_radio_role: OK\n");
    return 0;
}
